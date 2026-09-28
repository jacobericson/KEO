#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/pu_layout.h"
#include "render/name_list.h"
#include "render/particle_policy.h"
#include "render/module_hooks.h"
#include "render/ptr_set.h"
#include "game/game.h"
#include "base/core.h"
#include <cstring>
#include <intrin.h>
#include <math.h>

namespace particle_levers_detail {
typedef const void* (*PuTemplateName_t)(void* sys);   // returns const std::string&
} // namespace particle_levers_detail
using namespace particle_levers_detail;
namespace particle_levers_detail {
typedef void (*PuSetNonVisible_t)(void* sys, float seconds);
} // namespace particle_levers_detail
using namespace particle_levers_detail;
namespace particle_levers_detail {
typedef void (*PuUpdate_t)(void* sys, float dt);
} // namespace particle_levers_detail
using namespace particle_levers_detail;

static PuTemplateName_t  s_getTemplateName      = NULL;
static PuSetNonVisible_t s_setNonVisibleTimeout = NULL;
static bool              s_tried                = false;
static bool              s_installed            = false;

static PuUpdate_t    s_origUpdate       = NULL;
static const float*  s_gameSpeed        = NULL;   // GameWorld::frameSpeedMult
static bool          s_stepCapTried     = false;
static bool          s_stepCapInstalled = false;

static const int    TICK_INTERVAL = 60;      // main-loop passes between walks
static const size_t FX_WALK_MAX   = 4096;
static const int    NAME_LEN      = 128;

// Main ParticleLevers_MainThreadTick rebuilds the inactive table, then
// PublishTable atomically exchanges its index. Any-thread particle update
// readers select one table through ParticleSystemIsLooping; only the main
// next walk reads age. Save loading or disabled levers publish an empty table.
// Looping is a behavior input: publication orders the completed fill, but
// there is no reader pin or sequence check if a later walk reuses its table.
namespace particle_levers_detail {
struct SysEntry
{
	void* volatile sys;
	volatile LONG  looping;
	float          age;
};
} // namespace particle_levers_detail
using namespace particle_levers_detail;
static const int SYS_TABLE    = 1024;   // power of two
static const int SYS_MAX_FILL = 768;
static SysEntry      g_sysTable[2][SYS_TABLE];
static volatile LONG g_sysCur = 0;

// Main thread only: every system whose non-visible timeout this lever set
// and has not cleared since. Systems leave the active list and come back
// from Kenshi's effect pools still carrying it, so the record outlives the
// walks, and a save load (addresses only, nothing dereferenced). A system
// is timed out only once it is recorded, so a full record means the lever
// stops setting timeouts, never that one goes untracked.
static PtrSet<1024> s_timedOut;

static unsigned SysHash(void* sys)
{
	return (unsigned)(((uintptr_t)sys >> 4) * 2654435761u);
}

static bool Plausible(uintptr_t p)
{
	return p >= 0x10000 && p < 0x00007FFFFFFFFFFFULL && (p & 7) == 0;
}

static const SysEntry* SysFind(const SysEntry* t, void* sys)
{
	unsigned h = SysHash(sys);
	for (int i = 0; i < SYS_TABLE; ++i)
	{
		const SysEntry& e = t[(h + i) & (SYS_TABLE - 1)];
		void* k = e.sys;
		if (k == sys)
			return &e;
		if (!k)
			return NULL;
	}
	return NULL;
}

bool ParticleSystemIsLooping(void* sys)
{
	if (!sys)
		return false;
	const SysEntry* t = g_sysTable[g_sysCur & 1];
	_ReadBarrier();
	const SysEntry* e = SysFind(t, sys);
	return e && e->looping != 0;
}

namespace particle_levers_detail {
enum SysInsertResult { SYS_ADDED, SYS_SEEN, SYS_FULL };
} // namespace particle_levers_detail
using namespace particle_levers_detail;

// Adds sys to the table being built; *entry is its slot unless SYS_FULL.
// SYS_SEEN when it is already there (the active list can name one system twice).
static SysInsertResult SysInsert(SysEntry* t, int* filled, void* sys, bool looping, float age,
                                 SysEntry** entry)
{
	unsigned h = SysHash(sys);
	for (int i = 0; i < SYS_TABLE; ++i)
	{
		SysEntry& e = t[(h + i) & (SYS_TABLE - 1)];
		if (e.sys == sys)
		{
			*entry = &e;
			return SYS_SEEN;
		}
		if (!e.sys)
		{
			if (*filled >= SYS_MAX_FILL)
				return SYS_FULL;
			e.looping = looping ? 1 : 0;
			e.age = age;
			e.sys = sys;
			*entry = &e;
			++*filled;
			return SYS_ADDED;
		}
	}
	return SYS_FULL;
}

// The effect record's name (fire1_small, smoke3_small) is what the list is
// written against; the template name is the record's stringID, checked too so
// a stringID can be listed.
static bool EffectIsLooping(uintptr_t fx, void* sys)
{
	const char* list = g_renderCfg.particleLoopingNames;
	char name[NAME_LEN];
	uintptr_t gd = *(const uintptr_t*)(fx + FX_GAMEDATA);
	if (Plausible(gd))
	{
		ReadStdString((const void*)(gd + GD_NAME), name, sizeof(name));
		if (NameMatchesList(name, list))
			return true;
	}
	ReadStdString(s_getTemplateName(sys), name, sizeof(name));
	return NameMatchesList(name, list);
}

static void PublishTable(int next)
{
	_WriteBarrier();
	InterlockedExchange(&g_sysCur, next);
}

static void SetTimeout(void* sys)
{
	if (!s_timedOut.Add(sys))
	{
		InterlockedIncrement(&g_renderStats.fxRecordFull);
		return;
	}
	s_setNonVisibleTimeout(sys, g_renderCfg.particleOffscreenSeconds);
	InterlockedIncrement(&g_renderStats.fxTimeoutSet);
}

static void ClearTimeout(void* sys)
{
	s_setNonVisibleTimeout(sys, 0.0f);
	s_timedOut.Remove(sys);
	InterlockedIncrement(&g_renderStats.fxTimeoutCleared);
}

static void PublishEmptyTable()
{
	int next = (int)((g_sysCur & 1) ^ 1);
	memset(g_sysTable[next], 0, sizeof(g_sysTable[0]));
	PublishTable(next);
}

void ParticleLevers_MainThreadTick(bool saveLoading)
{
	static int  pass = 0;
	static bool emptied = false;   // the published table is the empty one
	static int  keysWas = -1;      // the two keys on the last pass; -1 before the first
	if (!s_installed)
		return;

	// A save load tears effects and their systems down; nothing is read or
	// written through the old list, and readers see no system as looping.
	if (saveLoading)
	{
		if (!emptied)
			PublishEmptyTable();
		emptied = true;
		pass = 0;
		return;
	}

	// Walks are needed while either lever is on, and while the off-screen
	// skip is off with timeouts it set still out. Otherwise readers get the
	// empty table rather than a stale one.
	bool skipOn = g_renderCfg.particleOffscreenSkip;
	int keys = (skipOn ? 1 : 0) | (g_renderCfg.particleStepCap ? 2 : 0);
	bool switched = keysWas >= 0 && keys != keysWas;
	keysWas = keys;
	if (!keys && s_timedOut.count == 0)
	{
		if (!emptied)
			PublishEmptyTable();
		emptied = true;
		return;
	}
	// A switched key walks on this pass, so switching the skip off clears
	// its timeouts at once rather than up to TICK_INTERVAL passes later.
	if (switched)
		pass = 0;
	else if ((++pass % TICK_INTERVAL) != 0)
		return;
	emptied = false;

	int cur = (int)(g_sysCur & 1);
	int next = cur ^ 1;
	const SysEntry* prev = g_sysTable[cur];
	SysEntry* t = g_sysTable[next];
	memset(t, 0, sizeof(g_sysTable[0]));
	int filled = 0;

	uintptr_t em = *(const uintptr_t*)GameAddr(RVA_EFFECTS_MGR);
	uintptr_t data = Plausible(em) ? *(const uintptr_t*)(em + EM_ACTIVE_DATA) : 0;
	size_t n = Plausible(data) ? *(const size_t*)(em + EM_ACTIVE_SIZE) : 0;
	if (n > FX_WALK_MAX)
		n = FX_WALK_MAX;

	const uintptr_t* list = (const uintptr_t*)data;
	for (size_t i = 0; i < n; ++i)
	{
		uintptr_t fx = list[i];
		if (!Plausible(fx))
			continue;
		uintptr_t handler = *(const uintptr_t*)(fx + FX_HANDLER);
		if (!Plausible(handler))
			continue;
		uintptr_t sysAddr = *(const uintptr_t*)(handler + PSH_SYSTEM);
		if (!Plausible(sysAddr))
			continue;
		void* sys = (void*)sysAddr;
		bool looping = EffectIsLooping(fx, sys);
		float age = *(const float*)(fx + FX_AGE);
		SysEntry* e = NULL;
		if (SysInsert(t, &filled, sys, looping, age, &e) == SYS_SEEN)
			continue;   // already handled on this walk

		const SysEntry* last = SysFind(prev, sys);
		bool restarted = !last || age < last->age;
		bool timeoutSet = *(const unsigned char*)(sysAddr + PS_NONVIS_SET) != 0;
		// A recorded system is still the one the lever timed out while it
		// carries a timeout and either its name is looping (a pooled system
		// comes back under its own name) or it runs on from the last walk
		// (the looping list itself changed). Anything else at a recorded
		// address has nothing of the lever's on it: only the entry goes.
		bool recorded = s_timedOut.Contains(sys);
		bool ours = recorded && timeoutSet && (looping || (last && !restarted));
		if (recorded && !ours)
			s_timedOut.Remove(sys);

		if (!skipOn || (ours && !looping))
		{
			if (ours)
				ClearTimeout(sys);
			continue;
		}
		int state = *(const int*)(fx + FX_STATE);
		bool stale = ours && *(const float*)(sysAddr + PS_NONVIS_TIME) != g_renderCfg.particleOffscreenSeconds;
		switch (OffscreenTimeoutAction(looping, state, timeoutSet, age,
		                               g_renderCfg.particleOffscreenMinAge, restarted, stale))
		{
		case OFFSCREEN_SET:
			SetTimeout(sys);
			break;
		case OFFSCREEN_CLEAR:
			ClearTimeout(sys);
			break;
		default:
			break;
		}
	}

	PublishTable(next);
	g_renderStats.fxTimedOut = s_timedOut.count;
}

bool InstallParticleLevers()
{
	if (s_tried)
		return s_installed;
	s_tried = true;
	HMODULE pu = GetModuleHandleA(PU_DLL);
	HMODULE ogre = GetModuleHandleA(OGRE_DLL);
	if (!pu || !ogre)
	{
		LogMsg("Render: particle levers: ParticleUniverse or OgreMain not loaded");
		return false;
	}
	s_getTemplateName      = (PuTemplateName_t)GetProcAddress(pu, SYM_PU_TEMPLATE);
	s_setNonVisibleTimeout = (PuSetNonVisible_t)GetProcAddress(pu, SYM_PU_SET_NONVISIBLE);
	if (!s_getTemplateName || !s_setNonVisibleTimeout || !VerifyParticleLayout(pu, ogre))
	{
		LogMsg("Render: particle levers: ParticleUniverse layout does not match this build");
		return false;
	}
	s_installed = true;
	return true;
}

static const ModuleSite s_puUpdateSite =
{
	"ParticleSystem::_update", PU_DLL, SYM_PU_UPDATE, 0,
	{ 0x40,0x56,0x48,0x83,0xEC,0x40,0x48,0x83,0x79,0x28,0x00,0x0F,0x29,0x74,0x24,0x30 }
};

// Ogre worker threads, and the main thread through ParticlePool::update: reads
// the game-speed float and the looping table, then calls the original. No
// logging, allocation or locks.
static void hook_ParticleUpdate(void* sys, float dt)
{
	if (g_renderCfg.particleStepCap && s_gameSpeed)
	{
		float speed = *s_gameSpeed;
		float cap = g_renderCfg.particleStepCapSpeed;
		if (_finite(speed) && speed > cap && ParticleSystemIsLooping(sys))
		{
			dt *= cap / speed;
			InterlockedIncrement(&g_renderStats.fxStepCapped);
		}
	}
	s_origUpdate(sys, dt);
}

bool InstallParticleStepCap()
{
	if (s_stepCapTried)
		return s_stepCapInstalled;
	s_stepCapTried = true;
	// The looping table is needed to answer ParticleSystemIsLooping; it is
	// only built when a particle lever is installed.
	if (!InstallParticleLevers())
		return false;
	s_gameSpeed = (const float*)((const char*)GameAddr(RVA_GLOBAL_GAMEWORLD) +
	                              OFF_GAMEWORLD_FRAME_SPEED_MULT);
	if (!InstallModuleHook(s_puUpdateSite, (void*)hook_ParticleUpdate, (void**)&s_origUpdate, NULL))
		return false;
	s_stepCapInstalled = true;
	return true;
}
