// nm_buildlock.cpp — the collision-build lock around the two section builders
// (buildCollision 0x3CB700, buildCollisionInterior 0x3CBAB0).
//
// Wide mode: buildCollisionCS around each whole builder (and, from
// nm_job_pipeline.cpp, around partialGeneration).
// Narrow mode (the default): the game's own build mutex, taken only around the
// parts of buildCollision that run outside it in the game's code.

#include "navmesh/jobs/nm_buildlock.h"

#include "navmesh/cache/nm_cache_core.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "navmesh/jobs/buildlock_audit.h"
#include "navmesh/jobs/nm_buildlock_stall.h"
#include <intrin.h>
#include <sstream>
#include <iomanip>
#pragma intrinsic(_ReturnAddress)

// --------------------------------------------------------------------
// buildCollisionCS
// --------------------------------------------------------------------
//
// Serializes the three regions that build section collision:
//   - buildCollision (0x3CB700), hooked
//   - buildCollisionInterior (0x3CBAB0), hooked
//   - the partialGeneration call in ProcessNavMeshJob's MISS tail
// The hooks cover the game's own calls from dispatchJob_orig and the mod's
// fn_buildCollision calls alike, since they patch the function entry.
//
// Lock order is processJobCS -> buildCollisionCS, never the reverse.
// buildCollisionCS is a leaf: nothing of ours runs inside it, the hook bodies
// only forward to the original, and it is never held while waiting on
// processJobCS or nmCacheCS.
//
// g_inBuild is bumped BEFORE the lock, so bcOverlap= counts build regions in
// flight > 1 rather than lock waits. That is the necessary condition for the
// unguarded tail races, not proof one happened: two threads can both be in a
// build region and still be serialized by the game's own build mutex inside
// buildSectionCollision.
static volatile long g_inBuild = 0;

static inline void NoteBcMax(volatile long* slot, long us)
{
	for (;;)
	{
		long prev = InterlockedCompareExchange(slot, 0, 0);
		if (us <= prev) break;
		if (InterlockedCompareExchange(slot, us, prev) == prev) break;
	}
}

// RAII, because buildCollision and partialGeneration carry C++ unwind state
// (both decompile with "Hidden C++ exception states"). A plain Enter / call /
// Leave would skip the Leave if anything unwound through this frame, and a
// buildCollisionCS left held hangs every NavMesh thread. The destructor runs on
// that path.
//
// Timing lives here so the wait and the hold are measured across one object:
//   wait = time blocked in EnterCriticalSection
//   hold = time from acquiring to releasing
// Totals are 64-bit microseconds (a 32-bit microsecond total overflows after
// about six hours of 100 ms builds); the maxima are 32-bit, which is 35 minutes
// of single hold and cannot realistically overflow.
BuildCollisionScope::BuildCollisionScope()
{
	// Bumped before the lock, so the count reflects regions in flight
	// rather than lock waits.
	if (InterlockedIncrement(&g_inBuild) > 1)
		InterlockedIncrement(&navmesh::g_nmCache.g_buildOverlapSeen);

	LARGE_INTEGER w0;
	QueryPerformanceCounter(&w0);
	EnterCriticalSection(&buildCollisionCS);
	QueryPerformanceCounter(&holdStart);

	long waitUs = (long)(QPCToMs(w0, holdStart) * 1000.0);
	InterlockedIncrement(&navmesh::g_nmCache.nmBcCount);
	InterlockedExchangeAdd64(&navmesh::g_nmCache.nmBcWaitTotalUs, (LONGLONG)waitUs);
	NoteBcMax(&navmesh::g_nmCache.nmBcWaitMaxUs, waitUs);
}

BuildCollisionScope::~BuildCollisionScope()
{
	LARGE_INTEGER h1;
	QueryPerformanceCounter(&h1);
	long holdUs = (long)(QPCToMs(holdStart, h1) * 1000.0);
	InterlockedExchangeAdd64(&navmesh::g_nmCache.nmBcHoldTotalUs, (LONGLONG)holdUs);
	NoteBcMax(&navmesh::g_nmCache.nmBcHoldMaxUs, holdUs);

	LeaveCriticalSection(&buildCollisionCS);
	InterlockedDecrement(&g_inBuild);
}

// The real signatures, from the decompiles. buildCollision takes four
// arguments; a3 is unused in its body and a4 reaches stitch_buildCollision
// (0x3C5C10), which overwrites it before use — but the hook forwards all four
// so the trampoline call is faithful whatever the caller passed.
typedef __int64 (*buildCollisionFull_t)(void* thisNMG, void* job, __int64 a3, double a4);
typedef __int64 (*buildCollisionInterior_t)(void* thisNMG, void* job);

static buildCollisionFull_t     orig_buildCollisionHook  = NULL;
static buildCollisionInterior_t orig_buildInteriorHook   = NULL;

typedef int (*stitchUnloadedZone_t)(void* nmg, void* inst, unsigned gx, unsigned gy, char stitch);
typedef int (*stitchWithInteriors_t)(void* nmg, void* inst, const void* list);
static stitchUnloadedZone_t  orig_stitchUnloadedZone  = NULL;
static stitchWithInteriors_t orig_stitchWithInteriors = NULL;
static nmgLockZone_t         orig_lockZoneDiag        = NULL;

static volatile LONG g_narrow = 0;             // decided once at install; never changes after

// The build mutex is held by this thread only between a builder's entry and its
// first stitchUnloadedZone (R1), and between the last neighbour's return and the
// tail stitch's return (TAIL). It is never held across a call into
// stitchUnloadedZone, which takes the same non-recursive mutex itself.
// t_blDepth > 0 while a narrow builder scope is open on this thread: the mutex
// is only ever taken inside one, whose destructor releases it.
enum { BL_NONE = 0, BL_R1 = 1, BL_TAIL = 2 };
static __declspec(thread) int      t_bl       = BL_NONE;
static __declspec(thread) void*    t_blNmg    = NULL;
static __declspec(thread) LONGLONG t_blHeldAt = 0;
static __declspec(thread) int      t_blDepth  = 0;

static volatile LONG     nmBlWaitN = 0, nmBlFail = 0, nmBlRecur = 0;
static volatile LONGLONG nmBlWaitUs = 0, nmBlHoldUs = 0;
static volatile long     nmBlWaitMaxUs = 0, nmBlHoldMaxUs = 0;
static volatile LONG     nmBcTotalN = 0;
static volatile LONGLONG nmBcTotalUs = 0;
static volatile long     nmBcTotalMaxUs = 0;
static volatile LONG     nmBscN = 0;
static volatile LONGLONG nmBscUs = 0;
static volatile long     nmBscMaxUs = 0;
static volatile LONG     nmBlTryN = 0, nmBlTryFail = 0;
static volatile LONG     nmBlStallLatch = 0, nmBlStallSleeps = 0, nmBlStallSkips = 0;
// One run per asking loop. Two loops refusing at once are two jams, not one,
// and a grant on one of them says nothing about the other: sharing the run
// would let an unrelated grant clear a genuine jam, and let two trickles add
// up into a bound neither of them reached.
static volatile LONG     g_stallLatched[BL_SITE_COUNT] = { 0 };
static volatile LONG     g_stallRun[BL_SITE_COUNT] = { 0 };
static volatile LONGLONG g_stallRunStart[BL_SITE_COUNT] = { 0 };
static volatile LONG     nmStitchConnected = 0, nmStitchWalked = 0, nmStitchAsym = 0;
static volatile LONG     g_bscOtherLogged = 0, g_swiOtherLogged = 0;

// Who holds the game's build mutex through a narrow builder scope, for the
// crash record. A thread that faults while holding it never runs its release,
// so bcHold= cannot report that hold and the record is the only place the
// ownership survives.
volatile LONG g_buildLockOwnerTid   = 0;
volatile LONG g_buildLockOwnerState = 0;   // BL_R1 or BL_TAIL


static const int BL_ZERO_ZONE[2] = { 0, 0 };   // lockZone ignores the zone


static uintptr_t ReturnRva(void* ra) { return (uintptr_t)ra - gameBase; }

// First call from an unrecognised site, logged once per hook. A caller outside
// the game's builders is also what another plugin's detour in front of ours
// looks like; TAIL is then never taken and the tail runs as in the game.
static void LogOtherCallerOnce(volatile LONG* flag, const char* hook, uintptr_t rva)
{
	if (InterlockedCompareExchange(flag, 1, 0) != 0)
		return;
	char line[128];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"Collision build lock: %s called from unrecognised rva 0x%llx", hook, (unsigned long long)rva);
	LogMsgDeferrable(line);
}

static void BlAcquire(void* nmg, int state)
{
	LONGLONG w0 = QpcNow();
	bool ok = fn_nmgLockZone(nmg, BL_ZERO_ZONE, true);
	LONGLONG w1 = QpcNow();
	if (!ok) { InterlockedIncrement(&nmBlFail); return; }
	long us = (long)QpcToUs(w1 - w0);
	InterlockedIncrement(&nmBlWaitN);
	InterlockedExchangeAdd64(&nmBlWaitUs, us);
	NoteBcMax(&nmBlWaitMaxUs, us);
	t_bl = state; t_blNmg = nmg; t_blHeldAt = w1;
	InterlockedExchange(&g_buildLockOwnerState, state);
	InterlockedExchange(&g_buildLockOwnerTid, (LONG)GetCurrentThreadId());
}

static void BlRelease()
{
	if (t_bl == BL_NONE) return;
	long us = (long)QpcToUs(QpcNow() - t_blHeldAt);
	InterlockedExchangeAdd64(&nmBlHoldUs, us);
	NoteBcMax(&nmBlHoldMaxUs, us);
	t_bl = BL_NONE;
	InterlockedExchange(&g_buildLockOwnerTid, 0);
	InterlockedExchange(&g_buildLockOwnerState, BL_NONE);
	fn_nmgUnlockZone(t_blNmg, BL_ZERO_ZONE);
}

// Builder wall time, the same measure in both modes (the wide mode's includes
// the buildCollisionCS wait).
struct BuilderWallTimer
{
	LONGLONG t0;
	BuilderWallTimer() : t0(QpcNow()) {}
	~BuilderWallTimer()
	{
		long us = (long)QpcToUs(QpcNow() - t0);
		InterlockedIncrement(&nmBcTotalN);
		InterlockedExchangeAdd64(&nmBcTotalUs, us);
		NoteBcMax(&nmBcTotalMaxUs, us);
	}
private:
	BuilderWallTimer(const BuilderWallTimer&);
	BuilderWallTimer& operator=(const BuilderWallTimer&);
};

// Overlap and the release of a TAIL still held when the tail stitch never ran
// (the section lookup returned NULL) or anything unwound. The wall timer is a
// member, so it closes after the release.
struct NarrowBuilderScope
{
	BuilderWallTimer wall;
	NarrowBuilderScope()
	{
		if (InterlockedIncrement(&g_inBuild) > 1)
			InterlockedIncrement(&navmesh::g_nmCache.g_buildOverlapSeen);
		if (t_bl != BL_NONE) { InterlockedIncrement(&nmBlRecur); BlRelease(); }
		++t_blDepth;
	}
	~NarrowBuilderScope()
	{
		BlRelease();
		--t_blDepth;
		InterlockedDecrement(&g_inBuild);
	}
private:
	NarrowBuilderScope(const NarrowBuilderScope&);
	NarrowBuilderScope& operator=(const NarrowBuilderScope&);
};

bool BuildLockNarrowActive() { return InterlockedCompareExchange(&g_narrow, 0, 0) != 0; }

static __int64 hook_buildCollision(void* thisNMG, void* job, __int64 a3, double a4)
{
	if (BuildLockNarrowActive())
	{
		NarrowBuilderScope scope;
		// The type-4 clears of the job's existing instance arrays.
		if (*(void**)KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 0x50))
			BlAcquire(thisNMG, BL_R1);
		return orig_buildCollisionHook(thisNMG, job, a3, a4);
	}
	BuilderWallTimer wall;
	BuildCollisionScope guard;
	return orig_buildCollisionHook(thisNMG, job, a3, a4);
}

static __int64 hook_buildCollisionInterior(void* thisNMG, void* job)
{
	if (BuildLockNarrowActive())
	{
		NarrowBuilderScope scope;             // no region of its own: counting only
		return orig_buildInteriorHook(thisNMG, job);
	}
	BuilderWallTimer wall;
	BuildCollisionScope guard;
	return orig_buildInteriorHook(thisNMG, job);
}

static int hook_stitchUnloadedZone(void* nmg, void* inst, unsigned gx, unsigned gy, char stitch)
{
	if (t_bl == BL_R1)
		BlRelease();
	else if (t_bl == BL_TAIL)
	{
		InterlockedIncrement(&nmBlRecur);
		BlRelease();
	}
	LONGLONG t0 = QpcNow();
	int r = orig_stitchUnloadedZone(nmg, inst, gx, gy, stitch);
	long us = (long)QpcToUs(QpcNow() - t0);
	InterlockedIncrement(&nmBscN);
	InterlockedExchangeAdd64(&nmBscUs, us);
	NoteBcMax(&nmBscMaxUs, us);

	uintptr_t rva = ReturnRva(_ReturnAddress());
	if (rva == RVA_BC_RET_LAST_NEIGHBOUR)
	{
		if (BuildLockNarrowActive() && t_blDepth > 0)
			BlAcquire(nmg, BL_TAIL);             // the section lookup and the tail stitch
	}
	else if (!(rva >= RVA_BUILD_COLLISION_IMPL && rva < RVA_BUILD_COLLISION_IMPL_END)
	      && !(rva >= RVA_BUILD_COLLISION_INTERIOR_IMPL && rva < RVA_BUILD_COLLISION_INTERIOR_IMPL_END))
		LogOtherCallerOnce(&g_bscOtherLogged, "stitchUnloadedZone", rva);   // t_bl is NONE here: released at entry
	return r;
}

static int hook_stitchWithInteriors(void* nmg, void* inst, const void* list)
{
	int connected = orig_stitchWithInteriors(nmg, inst, list);
	uintptr_t rva = ReturnRva(_ReturnAddress());
	if (rva == RVA_BC_RET_TAIL_STITCH)
	{
		int walked = list ? *(const int*)((const char*)list + 8) : 0;
		InterlockedExchangeAdd(&nmStitchConnected, connected);
		InterlockedExchangeAdd(&nmStitchWalked, walked);
		if (t_bl == BL_TAIL)
		{
#ifdef ZONEOPT_DEBUG
			static const BuildLockLayout lay = { 0x08, 0x3C, 0x40, 0x48, 0x38, 0x04 };
			if (list && walked > 0)
			{
				const void* const* data = *(const void* const* const*)((const char*)list + 16);
				int bad = StreamingPairMismatches(inst, data, walked, lay);
				if (bad) InterlockedExchangeAdd(&nmStitchAsym, bad);
			}
#endif
			BlRelease();                         // after the audit, which reads under the mutex
		}
	}
	else if (!(rva >= RVA_NMG_STITCH_UNLOADED_ZONE && rva < RVA_NMG_STITCH_UNLOADED_ZONE_END))
	{
		// A detour in front of this hook hides the tail's return address. The
		// tail stitch is over either way; release before the log, which takes a
		// mod lock that must never nest inside the build mutex.
		if (t_bl == BL_TAIL)
			BlRelease();
		LogOtherCallerOnce(&g_swiOtherLogged, "stitchWithInteriors", rva);
	}
	return connected;
}

// Which loop asked, from the return address. The four non-blocking call sites
// are fixed in the binary; anything else is a caller in front of ours.
static BuildLockStallSite StallSiteForReturn(uintptr_t rva)
{
	if (rva == RVA_LOCKZONE_RET_UPDATE_ADD) return BL_SITE_UPDATE_ADD;
	if (rva == RVA_LOCKZONE_RET_GEN_SAVE)   return BL_SITE_GEN_SAVE;
	if (rva == RVA_LOCKZONE_RET_UNLOAD)     return BL_SITE_UNLOAD;
	if (rva == RVA_LOCKZONE_RET_CREATE)     return BL_SITE_CREATE;
	return BL_SITE_UNKNOWN;
}

// Called for a refused non-blocking acquisition past the bound. The wait is
// only ever taken where the asking loop holds nothing (nm_buildlock_stall.h
// names the sites); elsewhere the jam is reported and the refusal returns at
// once, because the caller's own lock is what a navigability query needs. Once
// the navmesh system is being torn down the refusals no longer matter and the
// wait would only lengthen the quit.
static void StallThrottle(const BuildLockStallDecision& dec, BuildLockStallSite site,
                          long run, double secs)
{
	if (dec.action == BL_STALL_LATCH)
	{
		InterlockedIncrement(&nmBlStallLatch);
		char line[192];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"Collision build lock: %ld refusals in %.1fs with no grant from the %s loop — "
			"the holder is not releasing; %s", run, secs, BuildLockStallSiteName(site),
			dec.sleep ? "throttling the retry" : "this loop holds a lock, so the retry is not slowed");
		LogMsgDeferrable(line);
	}

	if (dec.sleep && InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) == 0)
	{
		InterlockedIncrement(&nmBlStallSleeps);
		Sleep(BUILD_LOCK_STALL_SLEEP_MS);
	}
	else
		InterlockedIncrement(&nmBlStallSkips);
}

static bool hook_lockZoneDiag(void* nmg, const int* zone, bool wait)
{
	uintptr_t retRva = ReturnRva(_ReturnAddress());
	bool r = orig_lockZoneDiag(nmg, zone, wait);
	if (!wait)
	{
		InterlockedIncrement(&nmBlTryN);
		if (!r) InterlockedIncrement(&nmBlTryFail);

		if (!navmesh::g_navmeshCfg.navmeshStallThrottleEnabled)
			return r;

		BuildLockStallSite site = StallSiteForReturn(retRva);

		BuildLockStallInputs in;
		in.granted    = r;
		in.runLength  = 0;
		in.runSeconds = 0.0;
		in.latched    = InterlockedCompareExchange(&g_stallLatched[site], 0, 0) != 0;
		if (!r)
		{
			LONGLONG now = QpcNow();
			in.runLength = InterlockedIncrement(&g_stallRun[site]);
			if (in.runLength == 1)
				InterlockedExchange64(&g_stallRunStart[site], now);
			LONGLONG start = InterlockedCompareExchange64(&g_stallRunStart[site], 0, 0);
			if (start)
				in.runSeconds = (double)(now - start) / (double)qpcFrequency.QuadPart;
		}

		BuildLockStallDecision dec = BuildLockStallEvaluate(in, site);
		if (dec.action == BL_STALL_RESET)
		{
			InterlockedExchange(&g_stallRun[site], 0);
			InterlockedExchange64(&g_stallRunStart[site], 0);
			InterlockedExchange(&g_stallLatched[site], 0);
		}
		else if (dec.action != BL_STALL_PASS)
		{
			if (dec.action == BL_STALL_LATCH)
				InterlockedExchange(&g_stallLatched[site], 1);
			StallThrottle(dec, site, in.runLength, in.runSeconds);
		}
	}
	return r;
}

void InstallBuildLockHooks()
{
	bool bc = HookInstallRow(HOOK_BUILD_COLLISION_IMPL, (void*)hook_buildCollision,
	                         (void**)&orig_buildCollisionHook, NULL, true) == NULL;
	LogMsgDeferrable(bc ? "buildCollision hook: installed" : "buildCollision hook: install FAILED");
	bool bi = HookInstallRow(HOOK_BUILD_COLLISION_INTERIOR_IMPL, (void*)hook_buildCollisionInterior,
	                         (void**)&orig_buildInteriorHook, NULL, true) == NULL;
	LogMsgDeferrable(bi ? "buildCollisionInterior hook: installed" : "buildCollisionInterior hook: install FAILED");

	// The stitch hooks install in both modes, so bsc= and stitch= compare
	// across the INI A/B. With g_narrow still 0 they never take the mutex.
	bool su = false, sw = false;
	if (bc)
	{
		su = HookInstallRow(HOOK_NMG_STITCH_UNLOADED_ZONE, (void*)hook_stitchUnloadedZone,
		                    (void**)&orig_stitchUnloadedZone, NULL, true) == NULL;
		sw = HookInstallRow(HOOK_NMG_STITCH_WITH_INTERIORS, (void*)hook_stitchWithInteriors,
		                    (void**)&orig_stitchWithInteriors, NULL, true) == NULL;
	}
	// Both stitch hooks must exist before the builder may take the mutex: a
	// half-installed machine could take it with nothing to release it. Set
	// before any builder runs (no worker exists yet) and never changed.
	if (navmesh::g_navmeshCfg.navmeshBuildLockNarrowEnabled && bc && su && sw && fn_nmgLockZone && fn_nmgUnlockZone)
		InterlockedExchange(&g_narrow, 1);

	// fn_nmgLockZone keeps pointing at the entry, so the mod's own blocking
	// calls pass through this counter, which counts only wait == false.
	if (HookInstallRow(HOOK_NMG_LOCK_ZONE, (void*)hook_lockZoneDiag, (void**)&orig_lockZoneDiag,
	                   NULL, true) == NULL)
	{
		LogMsgDeferrable("lockZone counter: installed");
	}
	else
		LogMsgDeferrable("lockZone counter: install FAILED (blTry=off)");

	char line[128];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "Collision build lock: %s (stitchUnloadedZone=%d stitchWithInteriors=%d)",
		BuildLockNarrowActive() ? "narrow" : "wide", su ? 1 : 0, sw ? 1 : 0);
	LogMsgDeferrable(line);
}

static void AppendAvgMax(std::ostringstream& ss, const char* key, LONG n, LONGLONG totalUs, long maxUs)
{
	double avg = n > 0 ? (double)totalUs / (1000.0 * n) : 0.0;
	ss << " " << key << "=" << avg << "/" << (double)maxUs / 1000.0 << "ms";
}

std::string BuildLockStatsSuffix()
{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(2);
	bool narrow = BuildLockNarrowActive();
	ss << " bcMode=" << (narrow ? "narrow" : "wide");
	ss << " bcOverlap=" << InterlockedCompareExchange(&navmesh::g_nmCache.g_buildOverlapSeen, 0, 0);

	if (narrow)
	{
		LONG n = InterlockedCompareExchange(&nmBlWaitN, 0, 0);
		AppendAvgMax(ss, "bcWait", n, InterlockedCompareExchange64(&nmBlWaitUs, 0, 0),
			InterlockedCompareExchange(&nmBlWaitMaxUs, 0, 0));
		AppendAvgMax(ss, "bcHold", n, InterlockedCompareExchange64(&nmBlHoldUs, 0, 0),
			InterlockedCompareExchange(&nmBlHoldMaxUs, 0, 0));
	}
	else
	{
		LONG n = InterlockedCompareExchange(&navmesh::g_nmCache.nmBcCount, 0, 0);
		AppendAvgMax(ss, "bcWait", n, InterlockedCompareExchange64(&navmesh::g_nmCache.nmBcWaitTotalUs, 0, 0),
			InterlockedCompareExchange(&navmesh::g_nmCache.nmBcWaitMaxUs, 0, 0));
		AppendAvgMax(ss, "bcHold", n, InterlockedCompareExchange64(&navmesh::g_nmCache.nmBcHoldTotalUs, 0, 0),
			InterlockedCompareExchange(&navmesh::g_nmCache.nmBcHoldMaxUs, 0, 0));
	}
	AppendAvgMax(ss, "bcTotal", InterlockedCompareExchange(&nmBcTotalN, 0, 0),
		InterlockedCompareExchange64(&nmBcTotalUs, 0, 0), InterlockedCompareExchange(&nmBcTotalMaxUs, 0, 0));

	{
		LONG n = InterlockedCompareExchange(&nmBscN, 0, 0);
		LONGLONG total = InterlockedCompareExchange64(&nmBscUs, 0, 0);
		double avg = n > 0 ? (double)total / (1000.0 * n) : 0.0;
		ss << " bsc=" << n << " " << avg << "/"
		   << (double)InterlockedCompareExchange(&nmBscMaxUs, 0, 0) / 1000.0 << "ms";
	}

	if (HookRowInstalled(HOOK_NMG_LOCK_ZONE))
	{
		ss << " blTry=" << InterlockedCompareExchange(&nmBlTryFail, 0, 0)
		   << "/" << InterlockedCompareExchange(&nmBlTryN, 0, 0);
		// Jams over the bound, then refusals slowed. Printed on every line
		// whatever the counts are: a field that appeared only once it fired
		// could not tell a session that never jammed from a build whose
		// bound does not work. "off" is a third answer, not a zero.
		// jams / waits taken / refusals past the bound that were not slowed,
		// because the loop that asked holds a lock or is not a known one.
		if (navmesh::g_navmeshCfg.navmeshStallThrottleEnabled)
			ss << " blStall=" << InterlockedCompareExchange(&nmBlStallLatch, 0, 0)
			   << "/" << InterlockedCompareExchange(&nmBlStallSleeps, 0, 0)
			   << "/" << InterlockedCompareExchange(&nmBlStallSkips, 0, 0);
		else
			ss << " blStall=off";
	}
	else
		ss << " blTry=off";

	LONG recur = InterlockedCompareExchange(&nmBlRecur, 0, 0);
	if (recur) ss << " blRecur=" << recur;
	LONG fail = InterlockedCompareExchange(&nmBlFail, 0, 0);
	if (fail) ss << " blFail=" << fail;
#ifdef ZONEOPT_DEBUG
	ss << " stitchAsym=" << InterlockedCompareExchange(&nmStitchAsym, 0, 0);
#endif
	ss << " stitch=" << InterlockedCompareExchange(&nmStitchConnected, 0, 0)
	   << "/" << InterlockedCompareExchange(&nmStitchWalked, 0, 0);
	return ss.str();
}

