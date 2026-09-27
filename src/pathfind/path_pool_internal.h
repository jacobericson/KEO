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
// Sets t_inContentStreamPass or t_inGatePass for the lifetime of a hook call
// and clears it on every exit, including a C++ exception unwinding out of the
// original (a flag left at 1 would attribute every later path-thread search to
// the gate counters, or hide the "outside a pass" warning for good). Used only
// in the two hooks, which have no __try, so a destructor is allowed there.
struct PPTlsFlagScope
{
	int* flag;
	explicit PPTlsFlagScope(int* f) : flag(f) { *flag = 1; }
	~PPTlsFlagScope() { *flag = 0; }
private:
	PPTlsFlagScope(const PPTlsFlagScope&);
	PPTlsFlagScope& operator=(const PPTlsFlagScope&);
};

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
// --- PathSlow: top 5 by service time (lock-free seqlock double buffer) ---

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
struct NpcWaitWalkResult
{
	int waitingTotal, waiting4, waiting5;
	int failed3FarDest;
	int stoppedFarDestNoMove;
	int navWaitZoneNotReady;
	double longestWaitSec;
	int haveTop;
	float topPosX, topPosZ;
	int topGx, topGy;
	int topState;
	double topDestDist;
	int topZoneReady;      // -1 unknown, 0/1
	int playerWaitingTotal, playerWaiting4, playerWaiting5;
	// 1 when the walk faulted part-way (a node or character freed mid-walk):
	// the counts above cover only the characters seen before the fault, and
	// the NpcPathWait line says so (partial=1) instead of passing them off as
	// the whole list.
	int faulted;
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


extern const size_t REQ_STAMP_OFFSET;
extern const size_t REQ_PRIORITY_OFFSET;
extern const size_t REQ_STATUS_OFFSET;
extern const size_t REQ_START_OFFSET;
extern const size_t REQ_GOAL_OFFSET;
extern const int REQ_STATUS_SENTINEL;
extern void* volatile g_sectionMgrPtr;
extern __declspec(thread) int t_inContentStreamPass;
extern __declspec(thread) int t_inGatePass;
extern LARGE_INTEGER g_passStartTicks;
extern LARGE_INTEGER g_gatePassEndTicks;
extern LARGE_INTEGER g_lastDequeueTicks;
extern volatile LONG g_gateOutsideStreamWarned;
extern LONG g_lastPathIterations;
extern LONG g_passSearchCount;
LONGLONG TicksToNs(LONGLONG ticks);
void PPHistAdd(PPHist* h, LONGLONG us);
LONGLONG PPHistPercentileUs(const PPHist* h, double frac);
void PPHistReset(PPHist* h);
extern volatile LONG g_passCount;
extern volatile LONGLONG g_passTotalUs;
extern volatile LONGLONG g_busyPassTotalUs;
extern volatile LONGLONG g_busyCharCause3Ticks;
extern volatile LONGLONG g_busyCharOtherTicks;
extern volatile LONG g_depthMax;
extern volatile LONGLONG g_depthSum;
extern volatile LONG g_depthSamples;
extern volatile LONG g_arrivedCount;
extern volatile LONG g_servedCount;
extern PPHist g_waitHist;
extern PPHist g_svcHist;
extern volatile LONGLONG g_svcTotalUs;
extern volatile LONG g_priNpcCount;
extern volatile LONG g_priPlayerCount;
extern volatile LONG g_priTierCount;
extern volatile LONG g_reqStatusCount[5];
extern volatile LONG g_directCount;
extern volatile LONG g_gatePassCount;
extern volatile LONGLONG g_gatePassTotalUs;
extern volatile LONG g_gatePassMaxUs;
extern volatile LONG g_gatePassInTransition;
void RunNpcWaitDiagnostic(double now);
GateWindowStats SnapshotAndResetGateStats();
void PrintPathQueueLine(double windowSec, const GateWindowStats& gws, LONG* servedOut);
void PrintPathBusyLine(const GateWindowStats& gws);
void PrintGateRateLine(const GateWindowStats& gws);
void PrintPathSlowLine(LONG servedThisWindow);
void PrintAstarCostLine();
void PrintNpcPathWaitLine(double windowSec);
} // namespace path_pool_detail

#include "pathfind/path_pool_search_state.h"

#endif // KENSHI_ZONE_OPT_PATH_POOL_INTERNAL_H
