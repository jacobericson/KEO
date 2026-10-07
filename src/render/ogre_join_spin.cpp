// ogre_join_spin.cpp - The Ogre barrier join spin: the entry detour on OgreMain's exported
// Barrier::sync, its install step, and the main-thread tick that hands it the key as a budget in
// counter ticks and writes the OgreSpin: line. Off, the detour is one volatile read and a forward;
// on, it spins on the main thread only, reading the barrier's counts, and never locks, allocates
// or logs.
#include "render/ogre_join_spin.h"

#ifdef KEO_DEBUG

#include "render/ogre_worker_policy.h"
#include "render/module_hooks.h"
#include "base/core.h"
#include "fixes/fixes_config.h"
#include <windows.h>
#include <sstream>
#include <string>

static void (*s_orig)(void*) = NULL;

// Published by the tick, read by the detour: 0 is off.
static volatile LONGLONG s_spinTicks = 0;

// Only the main thread reaches these: any other caller is passed through before them.
static LONG     s_calls = 0;
static LONG     s_ready = 0;
static LONG     s_spun = 0;
static LONG     s_hits = 0;
static LONG     s_timeouts = 0;
static LONGLONG s_spunTicks = 0;

static volatile LONG s_offMain = 0;

// Main thread only.
static int         s_seenUs = 0;
static LONGLONG    s_freq = 0;
static bool        s_installed = false;
static bool        s_shared = false;
static const char* s_installWhy = "not run";
static double      s_lastBeat = 0;

static const ModuleSite s_site =
{
	"Barrier::sync", "OgreMain_x64.dll", "?sync@Barrier@Ogre@@QEAAXXZ", OGRE_BARRIER_SYNC_RVA,
	{ 0x48,0x83,0xEC,0x28,0x48,0x8B,0x41,0x08,0x4C,0x8B,0xC9,0x48,0x89,0x44,0x24,0x30 }
};

// The thread that forks Ogre's workers is the only caller of the export; the workers inline
// their own syncs. The spin only reads the arrival count, and main still arrives exactly once,
// through the original's own increment: while main has not arrived nobody can reset the count,
// so a count that reaches parties - 1 makes main the last arriver, which releases the others
// and returns without a wait. A spin that runs out calls the original with the count as it is.
static void hook_BarrierSync(void* b)
{
	const LONGLONG budget = s_spinTicks;
	if (!budget)
	{
		s_orig(b);
		return;
	}
	if (!IsMainThread())
	{
		InterlockedIncrement(&s_offMain);
		s_orig(b);
		return;
	}

	const char* bar = (const char*)b;
	const unsigned long long parties = *(const unsigned long long*)(bar + OGRE_BARRIER_PARTIES);
	const volatile LONG* arrived = (const volatile LONG*)(bar + OGRE_BARRIER_ARRIVED);
	++s_calls;
	if (!OgreSpinWanted(*arrived, parties))
	{
		++s_ready;
		s_orig(b);
		return;
	}

	++s_spun;
	LARGE_INTEGER t0, t;
	QueryPerformanceCounter(&t0);
	t = t0;
	for (unsigned i = 1; ; ++i)
	{
		YieldProcessor();
		if (!OgreSpinWanted(*arrived, parties))
		{
			++s_hits;
			break;
		}
		if ((i & 31) == 0)
		{
			QueryPerformanceCounter(&t);
			if (t.QuadPart - t0.QuadPart >= budget)
			{
				++s_timeouts;
				break;
			}
		}
	}
	QueryPerformanceCounter(&t);
	s_spunTicks += t.QuadPart - t0.QuadPart;
	s_orig(b);
}

static const char* OgreSpinInstall()
{
	HMODULE ogre = GetModuleHandleA(s_site.module);
	if (!ogre)
		return "module";
	DWORD stamp = 0, size = 0;
	if (!ReadModuleImageId(ogre, &stamp, &size) || !OgreMainIdentityOk(stamp, size))
		return "build";
	if (ResolveModuleSite(s_site) != (void*)((uintptr_t)ogre + OGRE_BARRIER_SYNC_RVA))
		return "export";
	if (!InstallModuleHook(s_site, (void*)hook_BarrierSync, (void**)&s_orig, &s_shared))
		return "hook";
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_freq = f.QuadPart;
	return NULL;
}

void InstallOgreJoinSpin(int* installed, int*)
{
	(void)installed;
	const char* why = OgreSpinInstall();
	s_installWhy = why;
	s_installed = why == NULL;
	if (why)
		ErrorLog(std::string("OgreSpin: install=refused(") + why + ")");
	else
		LogMsg(std::string("OgreSpin: install=ok shared=") + (s_shared ? "1" : "0"));
}

static void OgreSpinHeartbeat(double now)
{
	const long long spunUs = s_freq > 0 ? (long long)(s_spunTicks * 1000000 / s_freq) : 0;
	std::ostringstream ss;
	ss << "OgreSpin: mode=" << s_seenUs;
	if (s_installWhy)
		ss << " install=refused(" << s_installWhy << ")";
	else
		ss << " install=ok";
	ss << " calls=" << s_calls << " ready=" << s_ready << " spun=" << s_spun << " hits=" << s_hits
	   << " timeouts=" << s_timeouts << " spunUs=" << spunUs
	   << " offMain=" << InterlockedCompareExchange(&s_offMain, 0, 0);
	LogMsg(ss.str());
	s_lastBeat = now;
}

void OgreJoinSpinTick(double now)
{
	const int us = fixes::g_fixesCfg.cfg_ogreJoinSpinUs;
	if (us != s_seenUs)
	{
		InterlockedExchange64(&s_spinTicks, s_installed ? OgreSpinTicks(us, s_freq) : 0);
		s_seenUs = us;
		OgreSpinHeartbeat(now);
	}
	else if (now - s_lastBeat >= 60.0)
	{
		OgreSpinHeartbeat(now);
	}
}

#else  // !KEO_DEBUG

void InstallOgreJoinSpin(int* installed, int*) { (void)installed; }
void OgreJoinSpinTick(double now) { (void)now; }

#endif // KEO_DEBUG
