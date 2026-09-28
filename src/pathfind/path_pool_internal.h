// path_pool_internal.h - private path pass state, histograms and window interfaces.
// Private to the path_pool*.cpp units and npc_wait_diag.cpp; off-main recorders take no lock.

#ifndef KENSHI_ZONE_OPT_PATH_POOL_INTERNAL_H
#define KENSHI_ZONE_OPT_PATH_POOL_INTERNAL_H

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
static const size_t PATHQ_INPUT_OFFSET  = 0xB8;   // mgr+0xB8: input queue (submit)
static const size_t PATHQ_RESULT_OFFSET = 0xF8;   // mgr+0xF8: result queue (contentStream)
static const size_t PATHQ_DEPTH_OFFSET  = 0x1C0;  // mgr+0x1C0: sorted array count
// =========================================================================
// Fixed log-spaced histogram (lock-free): p50/p90/p99/max from bucket counts
// =========================================================================
//
// Bucket b covers [2^b, 2^(b+1)) microseconds (bucket 0 covers [0,2)). 40
// buckets covers up to ~18 minutes, far beyond any single search or request.
// Percentiles are reported as the bucket's lower bound -- a log2-resolution
// approximation, adequate for an "is it queue latency or search cost"
// diagnostic, not exact statistics.

static const int PP_HIST_BUCKETS = 40;

struct PPHist
{
	volatile LONG     buckets[PP_HIST_BUCKETS];
	volatile LONG     maxUs;
	volatile LONG     count;
	volatile LONGLONG sumUs;
};
// --- PathSlow: top 5 by service time (single published buffer + seqlock) ---

struct PPSlowEntry
{
	LONGLONG svcUs;
	LONG     status;
	LONG     priority;
	LONG     iterations;   // latched from the pass's last PathPoolNoteSearch call
	// Havok units (world units x 0.1: audit's "+0x50 goal x 0.1" scale),
	// read straight from req+0x40/+0x50 -- not converted to world units.
	float    startX, startZ, goalX, goalZ;
};

static const int PP_SLOW_N = 5;
// =========================================================================
// PathPoolNoteSearch state: per-thread-class stats + boost counters
// =========================================================================

enum PPThreadClass { PP_CLASS_PATH = 0, PP_CLASS_NAVMESH = 1, PP_CLASS_MAIN = 2, PP_CLASS_OTHER = 3, PP_CLASS_COUNT = 4 };

struct PPClassStats
{
	volatile LONG     count;
	volatile LONGLONG totalTicks;       // used for a mean latency
	volatile LONGLONG totalIterations;
	PPHist            latencyUs;        // per-class search wall time (the latency histogram)
	PPHist            iterNsHist;       // per-iteration cost in ns; only path-thread's is printed (AstarCost)
};


struct GateWindowStats
{
	LONG     n;
	LONGLONG totalUs;
	LONG     maxUs;
	LONG     inTransition;
	LONG     iterLimit;
	LONG     stateFull;
	LONG     searches;
};


// Path-thread gate flag set/cleared by the hook_gatesUpdateCodes scope.
// The iteration/count latches reset at hook_contentStream pass top and
// advance in PathPoolNoteSearch during that same pass.
extern __declspec(thread) int t_inGatePass;
extern volatile LONG g_gateOutsideStreamWarned;
extern LONG g_lastPathIterations;
extern LONG g_passSearchCount;
LONGLONG TicksToNs(LONGLONG ticks);
void PPHistAdd(PPHist* h, LONGLONG us);
void PPSlowRequestReset();
void PPSlowConsider(LONGLONG svcUs, LONG status, LONG priority, LONG iterations, float sx, float sz, float gx, float gz);
int PPSlowSnapshot(PPSlowEntry* out);
LONGLONG PPHistPercentileUs(const PPHist* h, double frac);
void PPHistReset(PPHist* h);
// Written by the path-thread pass-through hooks in path_pool_hooks.cpp,
// with Interlocked updates. Main-thread reporters in path_pool_report.cpp
// read and reset each member or histogram bucket independently. Each atomic
// update publishes itself; a diagnostic window can straddle a pass and mix
// member epochs. Torn sets are tolerated; the whole struct is never copied.
struct PathPassWindow
{
	volatile LONG g_passCount;
	volatile LONGLONG g_passTotalUs;
	// Independent pass-duration total: PathBusy resets separately from PathQueue.
	volatile LONGLONG g_busyPassTotalUs;
	volatile LONG g_depthMax;
	volatile LONGLONG g_depthSum;
	volatile LONG g_depthSamples;
	volatile LONG g_arrivedCount;
	volatile LONG g_servedCount;
	PPHist g_waitHist;
	// Service starts after the latest pass-start, gate-end or dequeue latch.
	PPHist g_svcHist;
	volatile LONGLONG g_svcTotalUs;
	volatile LONG g_priNpcCount;
	volatile LONG g_priPlayerCount;
	volatile LONG g_priTierCount;
	// Raw completion status 0 found, 1 no start, 2 no goal, 3 disconnected,
	// 4 fallback failed. Cluster bypass leaves status 3 at zero; not reachability.
	volatile LONG g_reqStatusCount[5];
	// Status-0 completions with no path-thread search during this pass.
	volatile LONG g_directCount;
	volatile LONG g_gatePassCount;
	volatile LONGLONG g_gatePassTotalUs;
	volatile LONG g_gatePassMaxUs;
	volatile LONG g_gatePassInTransition;
};
extern PathPassWindow g_ppWindow;

// PathPoolNoteSearch in path_pool_hist.cpp atomically writes these fields.
// Gate, busy and outcome members are path-thread-only; class stats and boost
// tallies have any-search-thread writers. Main reporters read/reset members
// independently per window; each atomic is its publication. Mixed values
// are tolerated diagnostics, never a copied coherent set.
struct PathSearchWindow
{
	volatile LONGLONG g_busyCharCause3Ticks;
	volatile LONGLONG g_busyCharOtherTicks;
	volatile LONG g_gateSearchCount;
	volatile LONG g_gateIterLimit;
	volatile LONG g_gateStateFull;
	PPClassStats g_classStats[PP_CLASS_COUNT];
	// Non-gate path outcomes; cause 1 iter limit, 2 open full, 3 state full.
	volatile LONG g_pathSearchOk;
	volatile LONG g_pathSearchFail;
	volatile LONG g_pathTermIterLimit;
	volatile LONG g_pathTermOpenSetFull;
	volatile LONG g_pathTermStateFull;
	volatile LONG g_pathTermOther;
	// Boost outcomes: high-iteration success, other success, failure.
	volatile LONG g_boostByTag[3][2];
	volatile LONG g_boostByReq[3][3];
	volatile LONG g_boostDisagree;
};
extern PathSearchWindow g_ppSearch;

void RunNpcWaitDiagnostic(double now);
GateWindowStats SnapshotAndResetGateStats();
void PrintPathQueueLine(double windowSec, const GateWindowStats& gws, LONG* servedOut);
void PrintPathBusyLine(const GateWindowStats& gws);
void PrintGateRateLine(const GateWindowStats& gws);
void PrintPathSlowLine(LONG servedThisWindow);
void PrintAstarCostLine();
void PrintNpcPathWaitLine(double windowSec);
} // namespace path_pool_detail



#endif // KENSHI_ZONE_OPT_PATH_POOL_INTERNAL_H
