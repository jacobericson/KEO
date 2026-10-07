// scene_fork_skip.cpp - sceneForkSkip: the scene manager's three worker forks
// skipped when their lists give every worker nothing to do (scene_levers.h).
#include "render/scene_levers.h"

#ifdef KEO_DEBUG

#include "render/scene_lever_policy.h"
#include "render/module_hooks.h"
#include "fixes/fixes_config.h"
#include "fixes/near_page.h"
#include "base/core.h"
#include <windows.h>
#include <sstream>
#include <string>
#include <stdio.h>
#include <string.h>

static const char* const kOgreModule = "OgreMain_x64.dll";
static const char* const kSyncExport = "?sync@Barrier@Ogre@@QEAAXXZ";
static const char* const kUpdateVbExport =
	"?updateVertexBuffer@InstanceBatchHW@Ogre@@IEAA_KPEAVCamera@2@PEBV32@@Z";
static const char* const kBaseCullExport =
	"?instanceBatchCullFrustumThreaded@MovableObject@Ogre@@UEAAXPEBVFrustum@2@PEBVCamera@2@I@Z";
static const uintptr_t RVA_BARRIER_SYNC   = 0x3DFF40;
static const uintptr_t RVA_UPDATE_VB      = 0x130490;
static const uintptr_t RVA_BASE_CULL      = 0x274C0;

static const size_t kStubPageBytes = 4096;
static const size_t kStubSpacing   = 32;

// One redirected call to Barrier::sync: its offset in OgreMain, the bytes from
// the instruction that proves the scene manager's register to the call's end,
// and that register.
struct ForkSite
{
	size_t        rva;
	size_t        windowRva;
	unsigned char window[25];
	size_t        windowLen;
	ForkReg       reg;
};

static const ForkSite s_sites[FORK_SITES] =
{
	{ 0x2BD391, 0x2BD38A, { 0x48,0x8B,0x8E,0x20,0x4B,0x00,0x00,0xE8,0xAA,0x2B,0x12,0x00 }, 12, FORK_REG_RSI },
	{ 0x2BD39D, 0x2BD396, { 0x48,0x8B,0x8E,0x20,0x4B,0x00,0x00,0xE8,0x9E,0x2B,0x12,0x00 }, 12, FORK_REG_RSI },
	{ 0x2C29B8, 0x2C29A4, { 0x49,0x8B,0x8C,0x24,0x20,0x4B,0x00,0x00,0x41,0xC7,0x84,0x24,0x18,0x4B,0x00,0x00,
	                        0x01,0x00,0x00,0x00,0xE8,0x83,0xD5,0x11,0x00 }, 25, FORK_REG_R12 },
	{ 0x2C29C5, 0x2C29BD, { 0x49,0x8B,0x8C,0x24,0x20,0x4B,0x00,0x00,0xE8,0x76,0xD5,0x11,0x00 }, 13, FORK_REG_R12 },
	{ 0x2CAFD5, 0x2CAFC1, { 0x48,0x8B,0xF9,0xC7,0x81,0x18,0x4B,0x00,0x00,0x07,0x00,0x00,0x00,0x48,0x8B,0x89,
	                        0x20,0x4B,0x00,0x00,0xE8,0x66,0x4F,0x11,0x00 }, 25, FORK_REG_RDI },
	{ 0x2CAFE1, 0x2CAFDA, { 0x48,0x8B,0x8F,0x20,0x4B,0x00,0x00,0xE8,0x5A,0x4F,0x11,0x00 }, 12, FORK_REG_RDI },
};

typedef void (*BarrierSync_t)(void*);

// Written once by the install, before any site points at a stub.
static BarrierSync_t s_sync = NULL;     // the export's live entry
static uintptr_t s_baseCull = 0;        // MovableObject's empty threaded cull
static unsigned char* s_page = NULL;
static const char* s_install = "not run";
static char s_siteWhy[16];               // "site <n>", when a site refuses

// Main thread only: every site sits in a function only the main thread runs.
static ForkPairState s_pair[FORK_PAIRS];
static LONG s_fires[FORK_PAIRS];
static LONG s_skips[FORK_PAIRS];
static LONG s_odd = 0;
static volatile LONG s_offMain = 0;

// Published by the tick; the entry reads it at a fire only.
static volatile LONG s_mode = 0;

// Main-thread tick state.
static int s_seenMode = 0;
static double s_lastBeat = 0.0;
static const double kBeatSeconds = 60.0;

// The import-thunk test's reads. A slot may name any module's code, so a
// fault answers false and the fork runs.
static bool CullReadBytes(uintptr_t addr, unsigned char* out, size_t len)
{
	bool ok = true;
	GuardEnter();
	__try { memcpy(out, (const void*)addr, len); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static bool CullReadPtr(uintptr_t addr, uintptr_t* out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const uintptr_t*)addr; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static const CullReaders s_cullReaders = { &CullReadBytes, &CullReadPtr };
// Main thread only, like the fire that reads it.
static CullThunkCache s_cullThunks;

static bool PairIdle(int pair, const unsigned char* sm)
{
	switch (pair)
	{
	case FORK_R8: return ForkR8Idle(sm, s_baseCull, &s_cullThunks, &s_cullReaders);
	case FORK_R1: return ForkR1Idle(sm);
	case FORK_R7: return ForkR7Idle(sm);
	default: return false;
	}
}

// Called by a stub that a call at an Ogre site jumped to; returns straight to
// that site. A wait never reads the switch: it skips only the barrier its own
// fire skipped.
static void ForkSyncEntry(void* barrier, unsigned char* sm, int site)
{
	const int pair = site / 2;
	if (site & 1)
	{
		if (ForkWait(&s_pair[pair], IsMainThread()) == FORK_SKIP)
			return;
		s_sync(barrier);
		return;
	}

	const bool on = s_mode != 0;
	const bool onMain = IsMainThread();
	if (!onMain)
		InterlockedIncrement(&s_offMain);
	const bool idle = on && onMain && s_pair[pair].live && !s_pair[pair].pending && PairIdle(pair, sm);
	const ForkAction a = ForkFire(&s_pair[pair], on, onMain, idle);
	if (on && onMain)
		++s_fires[pair];
	if (a == FORK_SKIP)
	{
		++s_skips[pair];
		return;
	}
	if (a == FORK_CALL_ODD)
		++s_odd;
	s_sync(barrier);
}

const char* OgreSceneBuildRefusal(HMODULE ogre)
{
	if (!ogre)
		return "no module";
	unsigned long stamp = 0, size = 0;
	if (!OgrePeStampAndSize((const unsigned char*)ogre, 0x1000, &stamp, &size)
	    || stamp != OGRE_SCENE_BUILD_STAMP || size != OGRE_SCENE_BUILD_SIZE)
		return "build mismatch";
	const uintptr_t base = (uintptr_t)ogre;
	if ((uintptr_t)GetProcAddress(ogre, kSyncExport) != base + RVA_BARRIER_SYNC
	    || (uintptr_t)GetProcAddress(ogre, kUpdateVbExport) != base + RVA_UPDATE_VB)
		return "export mismatch";
	return NULL;
}

// The four rel32 bytes of the call at site, rewritten from the main thread
// while no frame runs.
static bool WriteRel32(unsigned char* site, int32_t rel)
{
	DWORD old = 0;
	if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old))
		return false;
	memcpy(site + 1, &rel, sizeof(rel));
	DWORD ignored = 0;
	VirtualProtect(site + 1, 4, old, &ignored);
	FlushInstructionCache(GetCurrentProcess(), site, 5);
	return true;
}

static void LogRefusal(const char* why)
{
	s_install = why;
	LogMsg(std::string("ForkSkip: install=refused(") + why + ")");
}

void InstallSceneForkSkip(int* installed, int*)
{
	(void)installed;
	HMODULE ogre = GetModuleHandleA(kOgreModule);
	const char* why = OgreSceneBuildRefusal(ogre);
	if (why)
	{
		LogRefusal(why);
		return;
	}
	const uintptr_t base = (uintptr_t)ogre;
	s_sync = (BarrierSync_t)GetProcAddress(ogre, kSyncExport);
	s_baseCull = (uintptr_t)GetProcAddress(ogre, kBaseCullExport);
	if (s_baseCull != base + RVA_BASE_CULL)
	{
		s_baseCull = 0;
		LogRefusal("cull export");
		return;
	}

	for (int i = 0; i < FORK_SITES; ++i)
	{
		const ForkSite& f = s_sites[i];
		if (!ForkSiteMatches((const unsigned char*)(base + f.windowRva), f.window, f.windowLen,
		                     base + f.rva, (unsigned long long)(uintptr_t)s_sync))
		{
			_snprintf_s(s_siteWhy, sizeof(s_siteWhy), _TRUNCATE, "site %d", i);
			LogRefusal(s_siteWhy);
			return;
		}
	}

	unsigned char* page = AllocateNearPages(base, kStubPageBytes);
	if (!page)
	{
		LogRefusal("no near page");
		return;
	}
	int32_t rel[FORK_SITES];
	for (int i = 0; i < FORK_SITES; ++i)
	{
		unsigned char* stub = page + kStubSpacing * i;
		const size_t n = BuildForkStub(stub, kStubSpacing, s_sites[i].reg, i,
		                               (unsigned long long)(uintptr_t)&ForkSyncEntry);
		const char* stubWhy = NULL;
		if (n != FORK_STUB_BYTES)
			stubWhy = "stub";
		else if (!Rel32To(base + s_sites[i].rva + 5, (unsigned long long)(uintptr_t)stub, &rel[i]))
			stubWhy = "reach";
		if (stubWhy)
		{
			VirtualFree(page, 0, MEM_RELEASE);
			LogRefusal(stubWhy);
			return;
		}
	}
	DWORD oldPage = 0;
	if (!VirtualProtect(page, kStubPageBytes, PAGE_EXECUTE_READ, &oldPage))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		LogRefusal("protect");
		return;
	}
	FlushInstructionCache(GetCurrentProcess(), page, kStubPageBytes);
	s_page = page;

	// Wait first: until its fire is redirected a wait stub only ever forwards.
	int live = 0;
	for (int p = 0; p < FORK_PAIRS; ++p)
	{
		const int fire = p * 2, wait = p * 2 + 1;
		if (!WriteRel32((unsigned char*)(base + s_sites[wait].rva), rel[wait]))
			continue;
		if (!WriteRel32((unsigned char*)(base + s_sites[fire].rva), rel[fire]))
			continue;
		s_pair[p].live = true;
		++live;
	}

	std::ostringstream line;
	if (live == FORK_PAIRS)
	{
		s_install = "ok";
		line << "ForkSkip: install=ok pairs=" << live << " page=0x" << std::hex << (uintptr_t)page;
	}
	else
	{
		s_install = "protect";
		line << "ForkSkip: install=refused(protect) pairs=" << live << " page=0x" << std::hex << (uintptr_t)page;
	}
	LogMsg(line.str());
}

static bool InstallOk()
{
	return s_install && strcmp(s_install, "ok") == 0;
}

static void EmitHeartbeat()
{
	std::ostringstream line;
	line << "ForkSkip: mode=" << (s_seenMode ? "on" : "off") << " install=";
	if (InstallOk())
		line << "ok";
	else
		line << "refused(" << s_install << ")";
	line << " r8=" << s_skips[FORK_R8] << "/" << s_fires[FORK_R8]
	     << " r1=" << s_skips[FORK_R1] << "/" << s_fires[FORK_R1]
	     << " r7=" << s_skips[FORK_R7] << "/" << s_fires[FORK_R7]
	     << " odd=" << s_odd << " offMain=" << InterlockedCompareExchange(&s_offMain, 0, 0);
	LogMsg(line.str());
}

void SceneForkSkipTick(double now)
{
	const int mode = (fixes::g_fixesCfg.cfg_sceneForkSkip == 1 && InstallOk()) ? 1 : 0;
	bool beat = false;
	if (mode != s_seenMode)
	{
		InterlockedExchange(&s_mode, mode);
		s_seenMode = mode;
		beat = true;
	}
	if (beat || now - s_lastBeat >= kBeatSeconds)
	{
		s_lastBeat = now;
		EmitHeartbeat();
	}
}

#else  // !KEO_DEBUG

void InstallSceneForkSkip(int* installed, int*) { (void)installed; }
void SceneForkSkipTick(double now) { (void)now; }

#endif // KEO_DEBUG
