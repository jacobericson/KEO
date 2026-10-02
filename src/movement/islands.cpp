// islands.cpp - Shared island state, readiness counters and the main-thread tick.
// Components are built and published by island_components.cpp; island_hooks.cpp
// owns both any-thread hooks, and island_stuck.cpp supplies router diagnostics.
// The seven inline accessors live in islands_inline.h through the private header.
// Shared mutable state and the four adapter definitions have one owner here.
//
// Readiness counters run on any thread. The tick, reset and span diagnostics
// run on the main thread, as do the overlay accessibility/generation adapters.
// The zone-index adapter also serves the edge-ring hook on any caller thread.
// No mod lock is acquired by these adapters or the inline accessors.
//
// See islands.h for the public threading contract.

#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "movement/island_overlay_internal.h"
#include "movement/formation.h"      // FormationCohesionSample (coh= on the diag line)
#include "pathfind/pathfinding.h"    // PlayerFarArrivals (farArrive= on the diag line)
#include "movement/island_span_policy.h"
#include "movement/island_edge_legs.h"
#include "movement/island_edge_ring.h"
#include "movement/order_outcome.h"
#include <iomanip>



// =========================================================================
// Per-thread readiness-gate counters (any thread)
// =========================================================================

namespace islands_detail {

const int    READINESS_TID_SLOTS = 8;
const double DIAG_INTERVAL_SEC   = 5.0;

// Any IslandCountReadiness caller claims tid by CAS before setting isMain
// and incrementing the atomic counts. The main AppendReadinessTids reads
// each field separately: a newly visible tid can precede its other values.
// Mixed values are tolerated diagnostics; slots and overflow never reset.
struct ReadinessTid {
	volatile LONG tid;
	volatile LONG isMain;
	volatile long calls;
	volatile long notReady;
	volatile long notReadySec;
};

static ReadinessTid  g_readTids[READINESS_TID_SLOTS];
static volatile long g_readTidOverflow = 0;

} // namespace
using namespace islands_detail;

void IslandCountReadiness(bool notReady, bool sectionsPending)
{
	LONG tid = (LONG)GetCurrentThreadId();
	for (int i = 0; i < READINESS_TID_SLOTS; ++i)
	{
		LONG prev = InterlockedCompareExchange(&g_readTids[i].tid, tid, 0);
		if (prev == 0)
		{
			// Claimed this slot. Record whether it is the game main thread.
			InterlockedExchange(&g_readTids[i].isMain, IsMainThread() ? 1 : 0);
			prev = tid;
		}
		if (prev == tid)
		{
			InterlockedIncrement(&g_readTids[i].calls);
			if (notReady)
			{
				InterlockedIncrement(&g_readTids[i].notReady);
				if (sectionsPending)
					InterlockedIncrement(&g_readTids[i].notReadySec);
			}
			return;
		}
	}
	InterlockedIncrement(&g_readTidOverflow);
}

static void AppendReadinessTids(std::ostringstream& ss)
{
	ss << " tids=[";
	bool first = true;
	for (int i = 0; i < READINESS_TID_SLOTS; ++i)
	{
		LONG tid = InterlockedCompareExchange(&g_readTids[i].tid, 0, 0);
		if (tid == 0) continue;
		if (!first) ss << " ";
		first = false;
		ss << tid
		   << (InterlockedCompareExchange(&g_readTids[i].isMain, 0, 0) ? "M:" : ":")
		   << InterlockedCompareExchange(&g_readTids[i].calls, 0, 0) << "/"
		   << InterlockedCompareExchange(&g_readTids[i].notReady, 0, 0) << "/"
		   << InterlockedCompareExchange(&g_readTids[i].notReadySec, 0, 0);
	}
	long ovf = InterlockedCompareExchange(&g_readTidOverflow, 0, 0);
	if (ovf > 0)
		ss << " +" << ovf << " unslotted";
	ss << "]";
}

namespace islands_detail {

const double ELIGIBILITY_INTERVAL = 0.25;


// Hook counters
volatile long g_isInCalls      = 0;
volatile long g_isInFlips      = 0;   // answer differs from vanilla (would-flip when passing through)
volatile long g_isInSeqFail    = 0;
// Census of the answers the engine's routing branch acts on. A positive answer
// spanning several cells is the reading that matters: it tells setDestination
// to send one direct path instead of routing to an island edge. The two
// bucket counters say which label pair produced it, and g_isInSpanUnk counts
// the answers no cell span could be computed for, so a quiet census is never
// confused with an unmeasured one.
volatile long g_isInFarTrue    = 0;   // positive answers spanning >= ISIN_FAR_CELLS cells
volatile long g_isInTrueZero   = 0;   // of those, both labels 0 (neither cell labelled)
volatile long g_isInTrueLabel  = 0;   // of those, equal non-zero labels (both in one component)
volatile long g_isInMaxSpan    = 0;   // worst cell span seen on a positive answer
volatile long g_isInSpanUnk    = 0;   // positive answers with no resolvable cell span
// The far-span rule (cfg_islandFarSpan). Every call lands in exactly one of
// vanFalse / vanTrue, and flipped is a subset of vanTrue, so the three close
// against g_isInCalls. g_farSpanArmed is set once at install: both hooks in and
// a threshold above 0.
volatile long g_isInVanTrue    = 0;
volatile long g_isInVanFalse   = 0;
volatile long g_isInRuleFlip   = 0;   // vanilla true answered false by the rule
volatile long g_isInRuleUnk    = 0;   // labelled vanilla true the rule could not measure
volatile long g_isInFlipSpan[ISLAND_SPAN_BUCKETS] = { 0 };
volatile long g_getIslCalls    = 0;
volatile long g_getIslAppended = 0;   // members appended (would-append when passing through)
volatile long g_getIslFallback = 0;   // seqlock failure / oversize -> original's answer
volatile long g_hooksInstalled = 0;
static bool          g_wasLoading       = false;
} // namespace
using namespace islands_detail;


// Private adapters keep global linkage for the tracker and edge-ring filter.
// Accessibility and generation/signature queries run on the main thread.
// IslandZoneIndexOf also serves the edge-ring filter on any caller thread.
// They acquire no mod locks; callers provide object lifetime.
bool         IslandOverlayZoneAccessible(uintptr_t z) { return ZoneAccess(z); }
unsigned int IslandOverlayGen()                       { return g_snapGen; }
unsigned int IslandOverlaySetBSig()                   { return g_setBSig; }
int          IslandZoneIndexOf(uintptr_t zm, uintptr_t z) { return ZoneIndexOf(zm, z); }


// =========================================================================
// Rebuild requests (main thread)
// =========================================================================

void IslandRequestRebuild()
{
	g_rebuildRequested = true;
}

void IslandReset()
{
	ResetBuilder();
	g_builderZm = 0;
	// Pending re-issue checks hold raw Character pointers; a save load frees
	// every character, so the table goes with the rest of the state
	// (islands_reissue.cpp).
	IslandReissueResetChecks();
	// Order records hold raw Character pointers too.
	OrderOutcomeReset();
}


// =========================================================================
// IslandTick (main thread, every frame)
//
// The parked-squad re-issue tracker and K7 (deleted-order re-issue, cancel
// hooks) live in islands_reissue.cpp and the island_*/k7_* units beside it;
// the calls below reach them through islands_internal.h.
// =========================================================================

namespace islands_detail { static unsigned int g_lastSig = 0; }
using namespace islands_detail;

// IslandSpan: the far-span rule's state, its call identity and the edge legs
// of the orders it moves. Every build and outside every feature gate, so a
// preload=false control and a PROD session print it too; a field nothing
// measures prints ?.
static void LogIslandSpan(double now)
{
	static double lastEmit = -1.0e9;
	static long   lastCalls = -1;
	static long   lastIslCalls = -1;
#ifdef KEO_DEBUG
	const double interval = DIAG_INTERVAL_SEC;
#else
	const double interval = 60.0;
#endif
	if (now - lastEmit < interval)
		return;

	std::ostringstream ss;
	ss << "IslandSpan:";
	long calls = InterlockedCompareExchange(&g_isInCalls, 0, 0);
	long islCalls = InterlockedCompareExchange(&g_getIslCalls, 0, 0);
	// The order-outcome totals move on their own schedule (a player order can land
	// between isInIsland/getIsland calls, or not at all if nothing routes
	// through them this interval), so a peek at whether they changed joins
	// the gate below; the real append happens once the decision is made.
	std::ostringstream orderPeek;
	bool ordersChanged = OrderOutcomeAppendSpanTotals(orderPeek);
	// Gated on isInIsland calls alone, the ring filter's own counters (all on
	// getIsland, a different hook) could go quiet forever whenever isInIsland
	// happens to hold steady even though getIsland keeps moving. Gate on
	// either counter, or the order totals, moving instead.
	if (calls == lastCalls && islCalls == lastIslCalls && !ordersChanged)
		return;
	lastEmit = now;
	lastCalls = calls;
	lastIslCalls = islCalls;

	bool installed = InterlockedCompareExchange(&g_hooksInstalled, 0, 0) != 0;
	long flipped = InterlockedCompareExchange(&g_isInRuleFlip, 0, 0);
	const char* state;
	if (!installed)                    state = "not-installed";
	else if (movement::g_movementCfg.cfg_islandFarSpan <= 0)   state = "off";
	else if (calls == 0)               state = "armed-idle";
	else if (flipped == 0)             state = "armed-quiet";
	else                               state = "armed-active";
	ss << " rule=" << state << " farSpan=" << movement::g_movementCfg.cfg_islandFarSpan;
	if (!installed)
	{
		ss << " calls=? vanT=? vanF=? flipped=?";
	}
	else
	{
		ss << " calls=" << calls
		   << " vanT=" << InterlockedCompareExchange(&g_isInVanTrue, 0, 0)
		   << " vanF=" << InterlockedCompareExchange(&g_isInVanFalse, 0, 0);
		if (IslandFarSpanArmed())
		{
			ss << " flipped=" << flipped << "(";
			for (int i = 0; i < ISLAND_SPAN_BUCKETS; ++i)
			{
				if (i) ss << "/";
				ss << "s" << (i + 1) << (i == ISLAND_SPAN_BUCKETS - 1 ? "+" : "") << "="
				   << InterlockedCompareExchange(&g_isInFlipSpan[i], 0, 0);
			}
			ss << ") ruleUnk=" << InterlockedCompareExchange(&g_isInRuleUnk, 0, 0);
		}
		else
		{
			ss << " flipped=off";
		}
	}
	IslandEdgeRingAppendSummary(ss);
	EdgeLegsAppendSummary(ss);
	IslandReissueAppendSpanDiag(ss);
	OrderOutcomeAppendSpanTotals(ss);
	LogMsg(ss.str());
}

void IslandTick(void* zoneMgr, double now)
{
	static double lastDiag = 0.0;

	LogIslandSpan(now);
	OrderOutcomePoll(now);
	uintptr_t zm = (uintptr_t)zoneMgr;

	if (zm)
	{
		bool loading = *(unsigned char*)(KLIB_MEMBER(2, zm, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;
		if (zm != g_builderZm || (loading && !g_wasLoading))
		{
			// Save load (ZM+8) or a new ZoneManager: drop marks, snapshot and tracker.
			ResetBuilder();
			IslandReissueReset();
			LogMsg(loading ? "Islands: save load detected, overlay reset"
			               : "Islands: zone manager bound, overlay reset");
			g_builderZm = zm;
		}
		g_wasLoading = loading;

		if (!loading)
		{
			WalkSetB(zm);
			bool sigChanged = !g_haveSig || g_setBSig != g_lastSig;
			bool eligibilityDue = (g_lastEligibility < 0.0)
			                   || (now - g_lastEligibility >= ELIGIBILITY_INTERVAL);
			if (sigChanged || g_rebuildRequested || eligibilityDue)
			{
				Rebuild(zm, now);
				g_lastSig = g_setBSig;
				g_haveSig = true;
				g_rebuildRequested = false;
			}
			// Resolve the pending re-issue checks whose 1 s delay has
			// elapsed, sample K7's per-frame signatures, then poll every
			// tracked order.
			//
			// The only part of this tick that issues orders, and so the only
			// part gated on preload: the rest above observes and is wanted in
			// a preload=false control, while re-issuing there would make that
			// control carry a behaviour it is supposed to be without.
			if (zone::g_zoneCfg.preloadEnabled)
				IslandReissuePollTick(zm, now);
		}
	}

	if (now - lastDiag >= DIAG_INTERVAL_SEC)
	{
		lastDiag = now;
#ifdef KEO_DEBUG
		std::ostringstream ss;
		ss << "Islands:";
		ss << " comps=" << g_curCompCount
		   << " mod=" << g_curModZones
		   << " setBacc=" << g_setBAccessible
		   << " unexpl=" << ((zm && zm == g_builderZm) ? CountUnexplained(zm) : 0)
		   << " snapGen=" << g_snapGen
		   << " live=" << (IslandHooksLive() ? 1 : 0)
		   << " isIn(flip)=" << InterlockedCompareExchange(&g_isInCalls, 0, 0)
		   << "(" << InterlockedCompareExchange(&g_isInFlips, 0, 0) << ")"
		   << " getIsl(app/fallback)=" << InterlockedCompareExchange(&g_getIslCalls, 0, 0)
		   << "(" << InterlockedCompareExchange(&g_getIslAppended, 0, 0)
		   << "/" << InterlockedCompareExchange(&g_getIslFallback, 0, 0) << ")";
		long seqFail = InterlockedCompareExchange(&g_isInSeqFail, 0, 0);
		if (seqFail) ss << " seqFail=" << seqFail;
		// Positive same-island answers that span cells, split by the label
		// pair behind them, with the worst span any positive answer carried.
		// far>0 means the engine was told to send a direct path across
		// several cells instead of routing to an island edge; z counts the
		// pairs where neither cell is labelled and l the pairs sharing one
		// label. maxSpan is reported for every positive answer, so far=0 with
		// maxSpan=1 says short-range answers only, not "nothing measured".
		ss << " isInTrue=far" << InterlockedCompareExchange(&g_isInFarTrue, 0, 0)
		   << "/z" << InterlockedCompareExchange(&g_isInTrueZero, 0, 0)
		   << "/l" << InterlockedCompareExchange(&g_isInTrueLabel, 0, 0)
		   << " maxSpan=" << InterlockedCompareExchange(&g_isInMaxSpan, 0, 0);
		long spanUnk = InterlockedCompareExchange(&g_isInSpanUnk, 0, 0);
		if (spanUnk) ss << " spanUnk=" << spanUnk;
		IslandReissueAppendDiag(ss);
		// Far arrivals: how often the engine declared a tracked character
		// arrived while it was still far from the destination its order
		// carried, and the worst such distance. A build that cannot sample
		// this says n/a rather than 0, so "none happened" and "nothing
		// measured it" are never the same reading.
		ss << std::fixed << std::setprecision(0);
		ss << " farArrive=" << PlayerFarArrivals()
		   << "/" << PlayerFarArriveMaxDist();
		// Squad cohesion: active formation groups, their live members, and
		// the largest max-pairwise spread of any one group with the id of the
		// group it belongs to. g0 says no group is travelling, which is why
		// the group count is printed next to the spread: max0 on its own
		// cannot tell "no group" from "a group standing on one spot". off
		// means group cohesion is not built or not enabled, so nothing can
		// form a group at all.
		if (movement::g_movementCfg.groupCohesionEnabled)
		{
			int cohGroups = 0, cohLive = 0, cohWorstId = -1;
			float cohWorst = 0.0f;
			FormationCohesionSample(&cohGroups, &cohLive, &cohWorst, &cohWorstId);
			ss << " coh=g" << cohGroups
			   << "/m" << cohLive
			   << "/max" << cohWorst
			   << "/in" << cohWorstId;
		}
		else
		{
			ss << " coh=off";
		}
		AppendReadinessTids(ss);
		LogDebug(ss.str());
#endif // KEO_DEBUG
	}
}

