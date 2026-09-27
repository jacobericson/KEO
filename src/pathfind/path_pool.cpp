// path_pool.cpp - shared path pass layout, thread latches and main-thread cadence.
//
// Keeps request offsets, path-thread timing state and PathPoolTickMain.
// The pass-through hooks, counters, NPC wait walk and window printers
// live in the module's other translation units.
//
// Off-main collectors stay lock-free and allocation-free; the main-thread
// tick invokes the diagnostics and printers.

#include "pathfind/path_pool_internal.h"
#include "pathfind/path_pool.h"
#include "pathfind/gate_pass.h"
#include "zone/transition.h"   // isTransitionActive
#include "zone/grid.h"         // WorldToZoneGrid
#include "fixes/streaming/navmesh_update_guard.h"
#include "pathfind/astar_cost_policy.h"  // AstarCallerClass constants for the PathBusy split
#include "pathfind/astar_cost.h"         // AstarCostTick, printed on the same window cadence
#include <intrin.h>        // _ReadWriteBarrier
#include <cmath>


namespace path_pool_detail {
// =========================================================================
// Shared layout constants
// =========================================================================


const size_t REQ_STAMP_OFFSET    = 0x00;   // never written by the game
const size_t REQ_PRIORITY_OFFSET = 0x2C;   // 10 NPC, 20 player, 45 mod tier
const size_t REQ_STATUS_OFFSET   = 0x90;   // -10 sentinel
const size_t REQ_START_OFFSET    = 0x40;   // start position (3 floats)
const size_t REQ_GOAL_OFFSET     = 0x50;   // goal position, offset-free once resolved

const int REQ_STATUS_SENTINEL = -10;


} // namespace path_pool_detail

// =========================================================================
// Thread identity + section manager latch
// =========================================================================

volatile DWORD g_pathThreadId = 0;
namespace path_pool_detail {

// Latched by hook_contentStream on every pass so the dequeueWork/enqueue
// filters always compare against the SectionManager currently in use (a new
// game or a save load can replace it). Single
// writer (the path thread, from inside contentStream); read on the path
// thread (same call stack) and, in principle, could be read elsewhere, so
// published through InterlockedExchangePointer for a clean 8-byte publish.
void* volatile g_sectionMgrPtr = NULL;

// Set for the duration of hook_contentStream so hook_gatesUpdateCodes (its
// sole caller, IDA-verified) can confirm it is really running inside a pass.
__declspec(thread) int t_inContentStreamPass = 0;

// Set for the duration of hook_gatesUpdateCodes; PathPoolNoteSearch reads it
// to attribute path-thread searches to the gate counters instead of the
// queue counters.
__declspec(thread) int t_inGatePass = 0;

// QPC at the start of the pass currently in progress on the path thread.
// Single-threaded (only ever written/read by the path thread, from nested
// calls within the same contentStream invocation), so a plain global is
// safe -- no other thread touches it.
LARGE_INTEGER g_passStartTicks = { 0 };

// "svc" must not include the pass preamble
// (origin shift, drainResults, work items, section adds, the gate pass,
// the request drain), or PathSlow ranks whole passes -- a gate pass can run
// up to 929 ms -- instead of searches. The tightest lower bound the four
// hooks can give for "serve one request began" is the LATEST of: pass
// start, the gate pass's own end (if one ran this pass) and the last
// dequeueWork return this pass (request drain, audit row 6, immediately
// precedes serve, row 7). Both reset to the pass start at the top of every
// pass; hook_gatesUpdateCodes and hook_dequeueWork push them later only if
// they actually run. Plain globals: path-thread-only, same reasoning as
// g_passStartTicks above.
LARGE_INTEGER g_gatePassEndTicks  = { 0 };
LARGE_INTEGER g_lastDequeueTicks  = { 0 };

// Set (not logged) by hook_gatesUpdateCodes on the path thread when it fires
// outside a contentStream pass; PathPoolTickMain reports it once from the
// main thread (no LogMsg from the path thread here).
volatile LONG g_gateOutsideStreamWarned = 0;

// Latched by PathPoolNoteSearch on the path thread for the last non-gate
// search this pass; hook_enqueueThreadSafe reads it for the PathSlow
// iterations field. Path-thread-only, like the ticks above.
//
// Reset to -1 at the top of every pass (with g_gatePassEndTicks/
// g_lastDequeueTicks), not left holding a stale value across passes.
// On the path thread outside gate passes the only
// caller of findPathFull is findPathFallback (serve step 7); a completion
// that ends at step 3 (status 1, no start face), step 4 (direct csFindPath
// success, status 0) or step 5 (status 2, no goal face) runs no search at
// all this pass, so without the reset PathSlow's iter= would silently show
// another, possibly many-passes-old, request's iteration count. -1 prints
// as "-" (no search ran for this completion).
LONG g_lastPathIterations = -1;

// "direct=" needs to know whether ANY path-thread, non-gate search ran
// during the pass that produced a given completion -- a status-0 completion
// with none is the csFindPath direct path (RunPathRequest step 4), not the
// fallback/full-search path (step 7). Reset alongside g_lastPathIterations;
// incremented by PathPoolNoteSearch.
LONG g_passSearchCount = 0;


// Nanosecond resolution for the per-iteration cost: microseconds
// put nearly every per-iteration cost in bucket 0/1 (the histogram's log2
// buckets are in whole units), so AstarCost's p50/p90 always read ~1.0/1.0.
// Nanoseconds spread real per-iteration costs (tens to low thousands of ns)
// across enough buckets to be informative.
LONGLONG TicksToNs(LONGLONG ticks)
{
	if (qpcFrequency.QuadPart <= 0 || ticks <= 0)
		return 0;
	return (ticks * 1000000000LL) / qpcFrequency.QuadPart;
}


} // namespace path_pool_detail

using namespace path_pool_detail;

// =========================================================================
// PathPoolTickMain (main thread; called every frame from hook_updateCameraZone)
// =========================================================================

void PathPoolTickMain(double now)
{
	static double lastWindowTime = -1.0e9;
	static double lastNpcWaitTime = -1.0e9;
	static bool   gateOutsideStreamReported = false;

	// hook_gatesUpdateCodes only sets the flag (path thread); the actual
	// LogMsg happens here, on the main thread, once.
	if (!gateOutsideStreamReported
		&& InterlockedCompareExchange(&g_gateOutsideStreamWarned, 0, 0) != 0)
	{
		gateOutsideStreamReported = true;
		LogMsg("PathPool: Gates__updateCodes fired outside a contentStream pass "
		       "(unexpected -- contentStream is documented as its sole caller)");
	}

#ifdef ZONEOPT_DEBUG
	const double windowInterval = 10.0;
#else
	const double windowInterval = 30.0;
#endif

	if (npcWaitDiagEnabled && (now - lastNpcWaitTime >= 1.0))
	{
		lastNpcWaitTime = now;
		RunNpcWaitDiagnostic(now);
	}

	if (now - lastWindowTime < windowInterval)
		return;
	// The first window runs from plugin start (ElapsedSec's zero, when every
	// counter was zero) to now, not a nominal windowInterval: the rates below
	// (busy%, preamble ms/s, reissue /10s) divide by it.
	double windowSec = (lastWindowTime > -1.0e8) ? (now - lastWindowTime)
	                 : ((now > 0.0) ? now : windowInterval);
	lastWindowTime = now;

	// Snapshot-and-reset the gate counters exactly once, so PathQueue's
	// gate= mirror and GateRate: (when printed) agree.
	GateWindowStats gws = SnapshotAndResetGateStats();

	LONG servedThisWindow = 0;
	PrintPathQueueLine(windowSec, gws, &servedThisWindow);
	if (gatePassDiagEnabled)
		PrintGateRateLine(gws);
	PrintPathSlowLine(servedThisWindow);
	PrintAstarCostLine();
	PrintPathBusyLine(gws);
	AstarCostTick();
	if (npcWaitDiagEnabled)
		PrintNpcPathWaitLine(windowSec);
}
