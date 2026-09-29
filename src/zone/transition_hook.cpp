// transition_hook.cpp - Transition bracket, priority and completion work.
// Main and contentStream threads; dismissal stamps are published with Interlocked.
// Completion and target reporting run on main; queue prioritization takes the queue lock alone.

#include "fixes/world/destroy_list_defer.h"
#include "zone/preload/preload.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "zone/transition_hook.h"
#include "zone/camera_zone_hook.h"
#include "zone/hooks_internal.h"
#include "zone/preload/coverage_stats.h"
#include "movement/tracking.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "pathfind/astar_cost.h"
#include "pathfind/gate_pass.h"

// Transition state (defined here, declared in transition.h)
bool           isTransitionActive   = false;
int            deferredFrameCount   = 0;
LARGE_INTEGER  transitionStartTime;
// Any dismissal caller fills the end stamp, then atomically raises pending.
// Main TransitionCompleteIfPending claims that flag before reporting and
// resetting the bracket. Flag ordering publishes earlier stores, not an
// immutable copy: another dismissal can restamp it while pending. Mixed
// diagnostic stamps are tolerated; the claim clears pending each completion.
volatile LONG  transitionEndPending  = 0;
LARGE_INTEGER  transitionEndQpc;
static DWORD          transitionEndTid      = 0;

namespace hooks_detail
{ // namespace hooks_detail

bool prioritizedThisTransition = false;

// Bracket generation. Every transition start increments it; the dismissal
// stamps the generation its bracket belongs to. The deferred completion runs
// its body only while the two still agree, so a bracket that started before
// the main thread got round to the previous completion cannot be torn down
// by it.
static volatile LONG transitionGen    = 0;
static LONG          transitionEndGen = 0;


// =========================================================================
// Transition-bracket instrumentation
// =========================================================================

// Target 3x3 and preload counts at transition start. Written by
// CaptureTransitionTarget and CaptureTransitionStart on the thread that opens
// the bracket (normally the main thread), read by LogTransitionTarget and
// LogTransitionStart on the main thread. When the start ran off the main
// thread, g_tgtLogPending and then g_startLogPending are raised after their
// values are written, and FlushPendingTransitionLines claims them on the
// next main-thread hook_updateCameraZone. Each pending flag orders an
// off-main capture before the main reporter, which consumes it before
// logging. The next bracket replaces the values; there is no sequence-checked
// copy, so overlapping captures can mix a diagnostic target or count. Main
// captures log directly and clear their pending flag.
static char          g_tgtLetters[10];
static int           g_tgtX          = -1;
static int           g_tgtY          = -1;
static volatile LONG g_tgtLogPending = 0;
static int           g_startGameOwned  = 0;
static int           g_startPreloaded  = 0;
static volatile LONG g_startLogPending = 0;

// Main-thread hook_updateCameraZone frames per zone-manager state while a
// bracket is open. Main thread only. g_bracketFramesGen is the transitionGen
// the counts belong to (0 = none yet): a new bracket is noticed by its
// generation and the counts restart, so the start branch, which can run off
// the main thread, never has to touch them.
const int            BRACKET_STATES    = 6;       // ZoneManager states 0..5
static int           g_bracketFrames[BRACKET_STATES];
static LONG          g_bracketFramesGen = 0;

// numPreloaded clamped to the table's capacity. A bracket start off the main
// thread reads it while the main thread may be changing it.
static int ClampedPreloadCount()
{
	int n = numPreloaded;
	if (n > MAX_PRELOADED) n = MAX_PRELOADED;
	if (n < 0) n = 0;
	return n;
}

// One zone of the target 3x3. Allocation-free and lock-free: it may run on
// whichever thread opened the bracket. Reads the preload arrays read-only
// (no lock: this is a diagnostic snapshot) and the zone's +176/+177 bytes,
// which live in the ZoneManager's static array.
//   U unloaded, P ours in flight (loading, or tracked and not yet registered),
//   R ours registered, A accessible (+177; game-owned, whether the game
//   already had it when this slot was recorded or has it now), G game-loaded
//   (+176, not ours), '-' off the grid.
static char TargetZoneLetter(void* zm, int gx, int gy)
{
	if (gx < 0 || gx > ZONE_GRID_MAX || gy < 0 || gy > ZONE_GRID_MAX)
		return '-';

	int n = ClampedPreloadCount();
	for (int i = 0; i < n; ++i)
	{
		if (preloadedZones[i].gridX != gx || preloadedZones[i].gridY != gy)
			continue;
		if (preloadedZones[i].gameOwned)  return 'A';
		if (preloadedZones[i].registered) return 'R';
		return 'P';
	}

	void* ze = GetZoneEntry(zm, gx, gy);
	if (!ze)                   return '-';
	if (IsZoneAccessible(ze))  return 'A';
	if (IsZoneLoading(ze))     return 'G';
	return 'U';
}

// Fills g_tgtLetters for the 3x3 around the transition target: ZoneManager's
// current zone. Rows are gy-1..gy+1, columns gx-1..gx+1, so index 4 is the target.
// Any thread; returns false when there is no target to report.
static bool CaptureTransitionTarget()
{
	void* zm = g_cachedZoneMgr;
	if (!zm)
		return false;
	void* tz = *(void**)(KLIB_MEMBER(2, (uintptr_t)zm, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE));
	if (!tz)
		return false;
	int tx = GetZoneGridX(tz);
	int ty = GetZoneGridY(tz);
	if (tx < 0 || tx > ZONE_GRID_MAX || ty < 0 || ty > ZONE_GRID_MAX)
		return false;

	int k = 0;
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			g_tgtLetters[k++] = TargetZoneLetter(zm, tx + dx, ty + dy);
	g_tgtLetters[9] = '\0';
	g_tgtX = tx;
	g_tgtY = ty;
	return true;
}

// Main thread only (CRT string work).
static void LogTransitionTarget()
{
	std::ostringstream ss;
	ss << "Transition target: (" << g_tgtX << "," << g_tgtY << ") 3x3="
	   << g_tgtLetters << " focus=" << g_tgtLetters[4];
	LogMsg(ss.str());
}

// Counts the tracked slots the game already owned when recorded. Any thread;
// allocation-free and lock-free, reading the preload table like
// TargetZoneLetter. Returns false when no slot is tracked.
static bool CaptureTransitionStart()
{
	int n = ClampedPreloadCount();
	if (n <= 0)
		return false;
	int gameOwnedCount = 0;
	for (int i = 0; i < n; ++i)
	{
		if (preloadedZones[i].gameOwned)
			gameOwnedCount++;
	}
	g_startGameOwned = gameOwnedCount;
	g_startPreloaded = n;
	return true;
}

// Main thread only (CRT string work).
static void LogTransitionStart()
{
	std::ostringstream ss;
	ss << "Transition start: " << g_startGameOwned << "/"
	   << g_startPreloaded << " game-owned";
	LogMsg(ss.str());
}

// Main thread, every hook_updateCameraZone frame. The bracket start raises
// the target flag before the start flag, so the flags are claimed in the
// reverse order: a claimed start flag means its target flag is already
// visible, and the target line never prints after its own start line.
void FlushPendingTransitionLines()
{
	bool start  = InterlockedCompareExchange(&g_startLogPending, 0, 1) == 1;
	bool target = InterlockedCompareExchange(&g_tgtLogPending, 0, 1) == 1;
	if (target)
		LogTransitionTarget();
	if (start)
		LogTransitionStart();
}

} // namespace hooks_detail
using namespace ::hooks_detail;

LONG TransitionGeneration()
{
	return InterlockedCompareExchange(&transitionGen, 0, 0);
}

namespace hooks_detail
{ // namespace hooks_detail

// Main thread, every hook_updateCameraZone frame.
void CountBracketFrame(void* zm)
{
	if (!isTransitionActive || !zm)
		return;
	LONG gen = InterlockedCompareExchange(&transitionGen, 0, 0);
	if (gen != g_bracketFramesGen)
	{
		for (int s = 0; s < BRACKET_STATES; ++s)
			g_bracketFrames[s] = 0;
		g_bracketFramesGen = gen;
	}
	int state = GetZoneState(zm);
	if (state >= 0 && state < BRACKET_STATES)
		g_bracketFrames[state]++;
}

} // namespace hooks_detail
using namespace ::hooks_detail;

static int             g_workerSavedPriority[NAVMESH_WORKER_COUNT] = {};

void CallPrioritizeNavMeshQueue()
{
	static SchedContext ctx;   // main thread only; kept off the stack (~1.3 KB)
	BuildSchedContext(&ctx);
	PrioritizeNavMeshQueue(ctx.camX, ctx.camY, ctx.movers, ctx.moverCount,
	                       ctx.zones, ctx.zoneCount);
}

namespace hooks_detail
{ // namespace hooks_detail

static void BoostWorkerThreads()
{
	// Live count, not capacity: slots above it were never created.
	for (int i = 0; i < navmesh::g_navmeshCfg.g_navMeshWorkerCount && i < NAVMESH_WORKER_COUNT; ++i)
	{
		HANDLE h = WorkerSlotHandle(i);
		if (h)
		{
			g_workerSavedPriority[i] = GetThreadPriority(h);
			SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
		}
	}
}

static void RestoreWorkerThreads()
{
	for (int i = 0; i < navmesh::g_navmeshCfg.g_navMeshWorkerCount && i < NAVMESH_WORKER_COUNT; ++i)
	{
		HANDLE h = WorkerSlotHandle(i);
		if (h)
			SetThreadPriority(h, g_workerSavedPriority[i]);
		g_workerSavedPriority[i] = THREAD_PRIORITY_NORMAL;
	}
}

} // namespace hooks_detail
using namespace ::hooks_detail;


// =========================================================================
// Hook 1: showLoadingMessage -- transition start/end bracket
// =========================================================================

void hook_showLoadingMessage(void* thisPtr, bool on)
{
	// A previous bracket's dismissal may still be waiting for the main thread.
	// Retire it here, before the new bracket is set up, or the start
	// below would be swallowed by the still-true isTransitionActive and the
	// deferred completion would later clear the new bracket's state.
	if (on && InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0)
	{
		if (IsMainThread())
		{
			// In order and complete: stats line, thread restore, preload reset.
			TransitionCompleteIfPending();
		}
		else
		{
			// Off the main thread, only allocation-free Win32 work is allowed.
			// Undo the priority boost (so the BoostNavMeshThread below does not
			// latch HIGHEST as the saved priority) and open the bracket. The
			// pending flag stays raised: the main thread will still report the
			// arrival, see the newer generation, and skip its body.
			RestoreNavMeshThread();
			RestoreWorkerThreads();
			isTransitionActive        = false;
			prioritizedThisTransition = false;
		}
	}

	if (on && !isTransitionActive)
	{
		InterlockedIncrement(&transitionGen);
		isTransitionActive = true;
		deferredFrameCount = 0;
		QueryPerformanceCounter(&transitionStartTime);
		BoostNavMeshThread();
		BoostWorkerThreads();

		// The target 3x3 and the preload counts as the bracket opens. The
		// captures are allocation-free. Their lines are logged here on the main
		// thread; off it (CRT string work is not allowed there) they wait for
		// the next main-thread hook_updateCameraZone. The target flag must be
		// raised before the start flag: FlushPendingTransitionLines relies on it.
		if (CaptureTransitionTarget())
		{
			if (IsMainThread())
			{
				InterlockedExchange(&g_tgtLogPending, 0);
				LogTransitionTarget();
			}
			else
			{
				InterlockedExchange(&g_tgtLogPending, 1);
			}
		}

		if (CaptureTransitionStart())
		{
			if (IsMainThread())
			{
				InterlockedExchange(&g_startLogPending, 0);
				LogTransitionStart();
			}
			else
			{
				InterlockedExchange(&g_startLogPending, 1);
			}
		}
	}
	else if (!on && isTransitionActive)
	{
		// The dismissal can arrive on the contentStream (path) thread, which
		// runs concurrently with the main thread. Nothing here may touch the
		// preload arrays, the thread-priority state or the logger -- LogMsg
		// builds a std::string and an ostringstream, which is CRT work this
		// thread must not do. Stamp the end time, the calling thread and the
		// bracket generation, then raise the flag; the next main-thread
		// updateCameraZone reports the arrival and runs the completion body.
		QueryPerformanceCounter(&transitionEndQpc);
		transitionEndTid = GetCurrentThreadId();
		transitionEndGen = InterlockedCompareExchange(&transitionGen, 0, 0);
		InterlockedExchange(&transitionEndPending, 1);
		GatePassNoteDismissal(transitionEndQpc.QuadPart, transitionEndGen, IsMainThread());
	}

	game::g_hookOrig.orig_showLoadingMessage(thisPtr, on);
}


// Deferred transition completion. Main thread only; called from the top of
// hook_updateCameraZone. Claims transitionEndPending, then runs the body that
// used to sit in the dismissal branch of hook_showLoadingMessage.
void TransitionCompleteIfPending()
{
	if (InterlockedCompareExchange(&transitionEndPending, 0, 1) != 1)
		return;

	// Close the bracket before doing anything that can take time. While
	// isTransitionActive is still true, a path-thread showLoadingMessage(false)
	// takes the dismissal branch again, re-stamps the end state and re-raises
	// the flag, and the next frame would report a second "Transition:" line for
	// the same start and call ClearPreloadZones() a second time. The only work
	// ahead of the write is one interlocked read: the generation has to be
	// known first, because on the superseded path the flag belongs to the newer
	// bracket and must not be cleared at all.
	bool superseded = (transitionEndGen != InterlockedCompareExchange(&transitionGen, 0, 0));
	if (!superseded)
	{
		isTransitionActive = false;
		// A completed transition is a bracket that closed without being
		// superseded -- the denominator AstarCap:'s per-100-transitions
		// rate divides by.
		AstarCostOnTransitionClosed();
	}

	// Report the arrival here rather than in the hook: this is the main thread,
	// so the CRT work in LogMsg is safe. main=1 means the dismissal itself was
	// delivered on this thread.
	{
		std::ostringstream ss;
		ss << "Transition end arrived on tid=" << (unsigned long)transitionEndTid
		   << " main=" << ((transitionEndTid == GetCurrentThreadId()) ? 1 : 0);
		LogMsg(ss.str());
	}

	// A newer bracket opened before this completion ran (the off-main-thread
	// path in hook_showLoadingMessage). Its start already restored the thread
	// priorities and re-armed the bracket, and transitionStartTime now belongs
	// to it, so there is nothing left here to finish and nothing safe to reset.
	if (superseded)
	{
		LogMsg("Transition: superseded by a new transition before the "
		       "completion ran; stats dropped");
		return;
	}

	RestoreNavMeshThread();
	RestoreWorkerThreads();
	prioritizedThisTransition = false;

	double totalMs = QPCToMs(transitionStartTime, transitionEndQpc);

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "Transition: " << totalMs << " ms, "
	   << deferredFrameCount << " frames deferred"
	   << (zone::g_zoneCfg.deferralEnabled ? "" : " (deferral OFF)")
	   << (navmesh::g_navmeshCfg.priorityBoostEnabled ? ", priority boosted" : "");
	if (zone::g_zoneCfg.preloadEnabled)
	{
		int registeredCount = 0;
		for (int i = 0; i < numPreloaded; ++i)
		{
			if (preloadedZones[i].registered)
				registeredCount++;
		}
		ss << ", preloaded=" << numPreloaded
		   << ", registered=" << registeredCount
		   << ", charZones=" << charZonesQueued
		   << ", preloadSkipLoaded=" << preloadSkipLoaded
		   << ", regSkip=" << regSkipCount;  // registry guard (zone_life.cpp), session total
		if (zone::g_zoneCfg.movementAwareEnabled)
			ss << ", orders=" << hookOrderCount
			   << ", watched=" << numWatched;
		// Session-cumulative, printed whether or not any of it moved: a zero
		// field says the path was never reached, which is the only way to
		// tell "covering" from "restored but never entered".
		ss << CoverageStatsToken();
	}
	// destroyListOE diagnostic: dlIns=<main>/<other> from the inserter hook, and the
	// off-main-thread call sites the first time any are recorded.
	ss << DestroyListStatsSuffix();
	if (pathfind::g_pathfindCfg.gatePassDiagEnabled)
		ss << GatePassTransitionToken(transitionEndGen, transitionEndQpc.QuadPart);

	// Main-thread frames per zone-manager state for slow transitions.
	// The counts belong to this bracket only if their generation matches the
	// one the dismissal was stamped with.
	if (totalMs > 300.0 && g_bracketFramesGen == transitionEndGen)
	{
		bool first = true;
		for (int s = 0; s < BRACKET_STATES; ++s)
		{
			if (g_bracketFrames[s] == 0)
				continue;
			ss << (first ? " frames=" : ",") << "s" << s << ":" << g_bracketFrames[s];
			first = false;
		}
	}
	LogMsg(ss.str());

	// Reset preload state; preserve watchedChars for in-transit squads.
	// Compact instead of clear, so pending
	// and registered zones are not dropped at transition end, and the per-cell
	// zone-lifecycle record keeps every zone the mod loaded (zone_life.cpp).
	CompactPreloadZones(totalMs / 1000.0);
	charZonesQueued = 0;
	hookOrderCount = 0;
}
