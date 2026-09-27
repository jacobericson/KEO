// audit_fx.cpp - Particle census and update timing.
// Main thread runs the census; Ogre workers and main run hk_PuUpdate.
// No probe takes a lock, allocates, or logs.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {
// ---- Particle effects -----------------------------------------------------

// Census map: ParticleUniverse::ParticleSystem* -> template id. Each census
// fills the other of two tables (bumping a table's generation empties it), so
// the previous census stays readable for the entry/exit diff. The census runs
// right before the particle job; the job's workers only read the table it
// just filled, and nothing writes it again until the next census.
const int FX_HASH     = 8192;            // power of two
const int MAX_FX_LIVE = FX_HASH / 2;

struct FxSlot
{
	const void*    sys;
	const void*    effect;               // the Effect holding the system
	float          age;                  // Effect age at the census
	unsigned       gen;                  // live while equal to its table's gen
	unsigned short tpl;
};

struct FxTable
{
	unsigned gen;
	int      n;
	int      list[MAX_FX_LIVE];          // slots filled by this census
	FxSlot   slot[FX_HASH];
};

FxTable       g_fxTab[2];
volatile LONG g_fxCurTab = -1;           // table of the last census, -1 before the first

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
RootSingleton_t     g_rootSingleton = NULL;
PuParticles_t       g_puParticles   = NULL;
const char* volatile g_root         = NULL;   // Ogre::Root*
bool g_fxHooked   = false;   // ParticleUniverse::_update detour installed
bool g_fxLayoutOk = false;   // PU / Root field offsets verified against their code
bool g_fxCensusOn = false;   // census runs at the `particles` site
volatile LONG g_fxPaused = 0;   // set by the census: the game is paused, nothing is counted

// _update totals at the previous frame close (per-frame deltas).
LONG64 g_fxLastTicks = 0, g_fxLastOff = 0, g_fxLastMain = 0;
LONG   g_fxLastCalls = 0;

int FxFind(const FxTable& t, const void* sys)
{
	unsigned h = HashPtr(sys, FX_HASH - 1);
	for (int probe = 0; probe < MAX_PROBES_PER_LOOKUP; ++probe)
	{
		const FxSlot& s = t.slot[h];
		if (s.gen != t.gen)
			return -1;
		if (s.sys == sys)
			return (int)h;
		h = (h + 1) & (FX_HASH - 1);
	}
	return -1;
}

int FxInsert(FxTable& t, const void* sys)
{
	unsigned h = HashPtr(sys, FX_HASH - 1);
	for (int probe = 0; probe < MAX_PROBES_PER_LOOKUP; ++probe)
	{
		FxSlot& s = t.slot[h];
		if (s.gen != t.gen)
		{
			s.sys = sys;
			s.gen = t.gen;
			return (int)h;
		}
		h = (h + 1) & (FX_HASH - 1);
	}
	return -1;
}

// ParticleUniverse's own visibility test (ParticleSystem::_update +0x3A):
// the system was queued for drawing in this frame or the previous one.
inline bool FxOnScreen(const char* root, const void* sys)
{
	if (!root)
		return false;
	int diff = (int)(KLIB_ROOT_FRAME(root) -
	                 *(const unsigned*)((const char*)sys + PS_LAST_VISIBLE));
	return diff >= 0 && diff <= 1;
}

// Template id of a system, by its ParticleUniverse template name. Main thread
// only, on first sight of a system.
int FxTemplateId(uintptr_t sys)
{
	char name[NAME_LEN];
	CopyOgreString((const void*)(sys + PS_TEMPLATE), name, NAME_LEN);
	for (char* ch = name; *ch; ++ch)
	{
		if (*ch == ' ' || *ch == '\t')
			*ch = '_';
	}
	if (!name[0])
		strcpy_s(name, "(unnamed)");
	LONG n = g_fxN;
	for (LONG i = 1; i < n; ++i)
	{
		if (strcmp(g_fxE[i].name, name) == 0)
			return (int)i;
	}
	if (n >= MAX_FX_TPL)
		return 0;
	NameEntry& e = g_fxE[n];
	memset(&e, 0, sizeof(e));
	strcpy_s(e.name, name);
	_WriteBarrier();
	InterlockedExchange(&g_fxN, n + 1);
	return (int)n;
}

// Template ids of systems updated on the main thread (zone effects, 0x40BA60),
// which the census doesn't see. Main thread only; emptied when half full.
const int      FX_MAIN_HASH = 1024;
const void*    g_fxMainKey[FX_MAIN_HASH];
unsigned short g_fxMainVal[FX_MAIN_HASH];
int            g_fxMainKeys = 0;

int FxMainTemplate(const void* sys)
{
	unsigned h = HashPtr(sys, FX_MAIN_HASH - 1);
	for (int probe = 0; probe < MAX_PROBES_PER_LOOKUP; ++probe)
	{
		const void* k = g_fxMainKey[h];
		if (k == sys)
			return g_fxMainVal[h];
		if (!k)
		{
			if (g_fxMainKeys >= FX_MAIN_HASH / 2)
			{
				memset(g_fxMainKey, 0, sizeof(g_fxMainKey));
				g_fxMainKeys = 0;
				h = HashPtr(sys, FX_MAIN_HASH - 1);
			}
			int id = FxTemplateId((uintptr_t)sys);
			g_fxMainKey[h] = sys;
			g_fxMainVal[h] = (unsigned short)id;
			++g_fxMainKeys;
			return id;
		}
		h = (h + 1) & (FX_MAIN_HASH - 1);
	}
	return 0;
}

// Walks EffectsManager's active list at the `particles` site, right before the
// particle job: per template, systems alive / on screen / stopping and their
// live particles, and entries into / exits from the list since the previous
// census. A system still held by the same effect and not younger is the same
// activation; a pooled system handed to a new activation counts as an exit
// plus an entry.
void FxCensus()
{
	unsigned long long t0 = __rdtsc();
	CurFrame& c = g_cur;
	// Paused frames are left out, as everywhere else in the audit; the map and
	// the entry/exit baseline stay as they were before the pause.
	LONG paused = (c.flags & F_PAUSED) ? 1 : 0;
	InterlockedExchange(&g_fxPaused, paused);
	if (paused)
		return;
	c.fxCensused = true;

	uintptr_t em = *(const uintptr_t*)(g_base + RVA_EFFECTS_MGR);
	size_t n = PlausiblePtr(em) ? KLIB_EFFECT_COUNT(em) : 0;
	uintptr_t data = n ? KLIB_EFFECT_DATA(em) : 0;
	if (n > 1000000 || (n && !PlausiblePtr(data)))
		n = 0;
	if (!g_root && g_rootSingleton)
		g_root = (const char*)g_rootSingleton();
	const char* root = g_root;

	LONG prevIdx = g_fxCurTab;
	int curIdx = prevIdx < 0 ? 0 : (int)(prevIdx ^ 1);
	FxTable& t = g_fxTab[curIdx];
	const FxTable* p = prevIdx < 0 ? NULL : &g_fxTab[prevIdx];
	++t.gen;
	t.n = 0;

	const uintptr_t* list = (const uintptr_t*)data;
	for (size_t i = 0; i < n; ++i)
	{
		uintptr_t e = list[i];
		if (!PlausiblePtr(e))
			continue;
		++c.fx;
		uintptr_t h = *(const uintptr_t*)(e + FX_HANDLER);
		uintptr_t sys = PlausiblePtr(h) ? *(const uintptr_t*)(h + PSH_SYSTEM) : 0;
		if (!PlausiblePtr(sys) || FxFind(t, (const void*)sys) >= 0)
			continue;   // no particles, or a system already counted this census
		float age = *(const float*)(e + FX_AGE);
		int stopping = *(const int*)(e + FX_STATE) == 1 ? 1 : 0;

		int tpl = -1;
		if (p)
		{
			int ps = FxFind(*p, (const void*)sys);
			if (ps >= 0)
			{
				const FxSlot& old = p->slot[ps];
				if (old.effect == (const void*)e && !(age < old.age))
					tpl = old.tpl;
				else
				{
					++g_fxDel[old.tpl];
					++c.fxDel;
				}
			}
		}
		bool fresh = tpl < 0;
		if (fresh)
			tpl = FxTemplateId(sys);

		int s = t.n < MAX_FX_LIVE ? FxInsert(t, (const void*)sys) : -1;
		if (s >= 0)
		{
			FxSlot& slot = t.slot[s];
			slot.effect = (const void*)e;
			slot.age    = age;
			slot.tpl    = (unsigned short)tpl;
			t.list[t.n++] = s;
			if (fresh)
			{
				++g_fxNew[tpl];
				++c.fxNew;
			}
		}
		else
			g_fxOverflow = 1;   // not tracked: its entry/exit would be miscounted

		bool vis = FxOnScreen(root, (const void*)sys);
		size_t parts = g_puParticles ? g_puParticles((void*)sys) : 0;
		++c.fxSys;
		c.fxVis   += vis ? 1 : 0;
		c.fxStop  += stopping;
		c.fxParts += (int)parts;
		++g_fxAlive[tpl];
		g_fxVisSum[tpl]  += vis ? 1 : 0;
		g_fxStopSum[tpl] += stopping;
		g_fxPartSum[tpl] += (LONG64)parts;
		if (*(const unsigned char*)(sys + PS_NONVIS_SET))
			++g_fxNvtoSum;
	}

	// Systems of the previous census that left the list.
	if (p)
	{
		for (int k = 0; k < p->n; ++k)
		{
			const FxSlot& old = p->slot[p->list[k]];
			if (FxFind(t, old.sys) < 0)
			{
				++g_fxDel[old.tpl];
				++c.fxDel;
			}
		}
	}

	g_fxEffSum += c.fx;
	if (!(c.flags & F_PAUSED) && !IsNan(c.dtMs) && c.dtMs > 0.0f && c.dtMs < 10000.0f)
		g_fxRunMs = g_fxRunMs + c.dtMs;   // raw frame time (mainLoop's argument)
	_WriteBarrier();
	InterlockedExchange(&g_fxCurTab, curIdx);
	InterlockedIncrement(&g_fxFrames);
	c.fxCensusTsc = __rdtsc() - t0;
}

// ParticleUniverse::ParticleSystem::_update(float). Ogre workers (the particle
// job) take the template from the census map; the main thread (zone effects)
// names its systems itself. Split by whether the system is on screen.
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
PuUpdate_t oPuUpdate = NULL;

void hk_PuUpdate(void* sys, float dt)
{
	if (!sys || g_fxPaused)
	{
		oPuUpdate(sys, dt);
		return;
	}
	int off = FxOnScreen(g_root, sys) ? 0 : 1;
	unsigned long long t0 = __rdtsc();
	oPuUpdate(sys, dt);
	unsigned long long d = __rdtsc() - t0;
	int tpl = 0;
	if (IsMain())
	{
		tpl = FxMainTemplate(sys);
		g_fxMainTsc += (LONG64)d;
		++g_fxMainCalls;
	}
	else
	{
		LONG cur = g_fxCurTab;
		if (cur == 0 || cur == 1)
		{
			int s = FxFind(g_fxTab[cur], sys);
			if (s >= 0)
				tpl = g_fxTab[cur].slot[s].tpl;
		}
	}
	if (tpl < 0 || tpl >= MAX_FX_TPL)
		tpl = 0;
	InterlockedExchangeAdd64(&g_fxTsc[off][tpl], (LONG64)d);
	InterlockedIncrement(&g_fxCalls[off][tpl]);
}

// This frame's _update totals: the per-template counters summed, less the
// previous close (the job and the zone-effect updates finish inside a frame).
void FxFrameTotals(FrameRec& r)
{
	LONG64 ticks = 0, off = 0;
	LONG calls = 0;
	LONG n = g_fxN;
	for (LONG id = 0; id < n && id < MAX_FX_TPL; ++id)
	{
		LONG64 o = g_fxTsc[1][id];
		ticks += g_fxTsc[0][id] + o;
		off   += o;
		calls += g_fxCalls[0][id] + g_fxCalls[1][id];
	}
	LONG64 main = g_fxMainTsc;
	double perMs = g_tscPerMs;
	if (perMs > 0.0)
	{
		double dMain = (double)(main - g_fxLastMain);
		r.m[M_FX_UPD]     = (float)(((double)(ticks - g_fxLastTicks) - dMain) / perMs);
		r.m[M_FX_UPDOFF]  = (float)((double)(off - g_fxLastOff) / perMs);
		r.m[M_FX_UPDMAIN] = (float)(dMain / perMs);
	}
	r.c[C_FXCALLS] = (int)(calls - g_fxLastCalls);
	g_fxLastTicks = ticks;
	g_fxLastOff   = off;
	g_fxLastMain  = main;
	g_fxLastCalls = calls;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
