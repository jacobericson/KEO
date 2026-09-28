// path_pool.cpp - path-thread identity and main-thread diagnostic cadence.
//
// Keeps the path-thread identity, deferred gate warning and PathPoolTickMain.
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




// =========================================================================
// Thread identity + section manager latch
// =========================================================================

// hook_contentStream atomically relatches the path thread id each pass;
// search classifiers read the aligned scalar on any calling thread. Initial
// zero is replaced by the next pass, with no separate reset. It cannot tear
// and is not a coherent pair with the separately published manager pointer.
volatile DWORD g_pathThreadId = 0;
namespace path_pool_detail {

// Set (not logged) by hook_gatesUpdateCodes on the path thread when it fires
// outside a contentStream pass; PathPoolTickMain reports it once from the
// main thread (no LogMsg from the path thread here).
volatile LONG g_gateOutsideStreamWarned = 0;

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

	if (pathfind::g_pathfindCfg.npcWaitDiagEnabled && (now - lastNpcWaitTime >= 1.0))
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
	if (pathfind::g_pathfindCfg.gatePassDiagEnabled)
		PrintGateRateLine(gws);
	PrintPathSlowLine(servedThisWindow);
	PrintAstarCostLine();
	PrintPathBusyLine(gws);
	AstarCostTick();
	if (pathfind::g_pathfindCfg.npcWaitDiagEnabled)
		PrintNpcPathWaitLine(windowSec);
}
