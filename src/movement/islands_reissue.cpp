// islands_reissue.cpp - main-thread reissue tracker state and adapters.
//
// Owns the IslandOrder table, K7 counters, main-thread adapters and diagnostic
// appenders. Order polling, deferred checks, cancel snapshots and pause clocks
// live in the adjacent files that use them.
// The tracker reads only these values from the component overlay (islands.cpp): the
// rebuild generation and Set B signature (IslandOverlayGen/IslandOverlaySetBSig, for
// staleness detection) and a zone's accessible flag (IslandOverlayZoneAccessible);
// islands_internal.h declares those and the adapter calls.
//
// See islands.h for the threading contract and what the overlay does.

#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "zone/preload/preload.h"
#include "movement/formation.h"
#include "pathfind/player_task_policy.h"   // PT_OFF_* task-system offsets (for K7)
#include "movement/k7_swap_policy.h"       // K7ClassifySwap / K7SigOnsetStep
#include "movement/k7_arrival_policy.h"    // K7ArrivalShouldArm / K7ArrivalPoll
#include "zone/readiness/zone_readiness_classify.h"  // ClassifyZoneReadiness, ZR_*
#include "movement/order_outcome.h"            // OrderOutcomeNoteReissueSent
#include "movement/island_span_policy.h"       // IslandCellSpan (K7 arrival arm line)
#include "zone/zone_pause.h"               // ZonePauseIsPaused (pause gate)
#include <intrin.h>
#include <cstring>
#include "movement/islands_reissue_internal.h"
#include "movement/islands_reissue_counters.h"


namespace order_tracker_detail {



IslandOrder   g_orders[MAX_ISLAND_ORDERS];
int           g_orderCount = 0;

// K7 counters for the Islands: diag line (main thread only; the
// cancel detours run on the main thread too).
long g_k7DelPark     = 0;   // delPark=:    deletion episodes whose full predicate held
long g_k7DelReissue  = 0;   // delReissue=: deleted-order re-issues sent
long g_k7CancelStop  = 0;   // cancelDrop=s: entries dropped by the stop key
long g_k7CancelJob   = 0;   // cancelDrop=j: entries dropped by a clearing job order
long g_k7CancelTask  = 0;   // cancelDrop=t: entries dropped by a nearest-character task
long g_k7GameDrop    = 0;   // gameDrop=:   total of the three reasons below
long g_k7ApdDrop     = 0;   // apdDrop=:    an order appended behind the tracked one

// gameDropWhy=u/h/x: the three permanent drops K7Observe applies, each
// counted separately. One total cannot say which rule removed a character
// from every later re-issue, and the three imply different answers: u is
// the game clearing the order for a reason vanilla also honours, while h
// and x are read from the order deque and the current task and so can
// misfire on a transient the 1 Hz task sampler never prints.
long g_k7DropUnconcious = 0;   // u: isUnconcious turned true while tracked
long g_k7DropOrderHead  = 0;   // h: a non-move order at the deque head
long g_k7DropTaskSwap   = 0;   // x: another task after the order ran, deque empty

// The "x" swap, split by K7ClassifySwap's verdict. xo/xe are never counted
// while gameDropWhy's plain x is (each swap counts in exactly one of the
// three): xo is a died-first swap into a task off the combat whitelist, xe a
// held swap whose fight ran past k7PostDeathHold's 60 s cap. Both count under
// k7PostDeathHold=observe too, where nothing is actually held (still counted
// as x there, since the send path is unchanged); see g_k7HoldWould for what
// observe adds.
long g_k7DropTaskSwapNonCombat = 0;   // xo
long g_k7DropTaskSwapExpired   = 0;   // xe

// k7Hold=s<held>/r<resumedSends>: episodes K7ClassifySwap called HOLD while
// k7PostDeathHold=true (a hold actually started), and how many of those were
// later re-issued (as opposed to expiring or swapping to a non-combat task).
// g_k7HoldWould counts every poll K7ClassifySwap said HOLD regardless of
// mode -- the observe-mode proof that a real run would have held here too.
long g_k7HoldStarted     = 0;   // k7Hold=s
long g_k7HoldResumedSend = 0;   // k7Hold=r
long g_k7HoldWould       = 0;   // counted in every mode, including off

// K7TryDeletedReissue's per-poll refusal gates (delRefuse=), one counter per
// early return so a session can say which gate is holding a specific
// character back instead of only "it did not send".
long g_k7RefuseHyst    = 0;   // h: -1 not held long enough
long g_k7RefuseSig     = 0;   // g: no end signature inside the window
long g_k7RefuseDest29  = 0;   // d: last task-29 destination does not match
long g_k7RefuseDestNow = 0;   // n: +0xDC now does not match either
long g_k7RefuseCarried = 0;   // c: _isBeingCarried
long g_k7RefuseInSomething = 0;   // s: Character::inSomething
long g_k7RefuseHit     = 0;   // a: under melee attack now
long g_k7RefuseEnemies = 0;   // e: AI sensory numEnemies != 0
long g_k7RefuseThreats = 0;   // t: AI sensory threats != 0
long g_k7RefuseHold    = 0;   // o: CharStats holdPositionMode
long g_k7RefuseNear    = 0;   // f: already within PARK_MIN_DEST_DIST of dest
long g_k7RefuseZones   = 0;   // z: K7ZonesAccessible false
long g_k7RefuseDestReady = 0; // r: destination not ready
long g_k7RefuseCooldown  = 0; // k: tracker cooldown (IslandRecentlyReissued)
long g_k7RefuseBudget    = 0; // b: MAX_REISSUES spent
long g_k7DestReadyTimeout = 0; // destination-readiness waits force-allowed after 15 s

// k7Arrive=w<armed>/s<sent>/x<expired>/v<fired-before-deleted-latch>/
// r<cleared-without-a-send>/open<currently-armed>. w (K7ArrivalArmEdge) and x
// (the 15 s cap with no arrival, or a fire condition some other gate keeps
// refusing) count regardless of k7ArrivalTrigger -- an instrument is never
// gated behind its lever. s only counts an actual send (k7ArrivalTrigger=true
// and every K7ArrivalFireGate check passed); with the key false, a
// would-have-sent episode is never counted here at all -- read the "K7
// arrival (observe)" line instead. v is the subset of s whose entry had not
// yet reached the "deleted" state at send time. It is NOT proof of the
// live-path shape (a state-6 re-request that never clears the order out of
// the deque): a plain not-in-world wait (the exterior-behind-interiors shape)
// that fires before the next poll latches k7DeletedSince also reads v. r is
// every wait ended other than by an arrival-form send: the engine resumed
// the walk on its own, its formation started gathering, the entry arrived,
// dropped out of the squad, was cleared by K7Observe, was overwritten by a
// new order for the same character, was dropped by a selection-cancel or a
// nearest-task cancel, was cleared by a save load or a new ZoneManager
// (ResetOrders), or -- also not an arrival-form send -- was cleared by a
// deleted-form send instead (K7TryDeletedReissue's own success point). open
// is a live count of waits still armed right now, so
// w == s + x + r + open always holds -- an entry cannot vanish from w without
// landing in exactly one of the other three. The "Island reissue (arrival)"
// log line is the actual per-episode record, and "K7 arrival armed" the
// per-episode arm.
long g_k7ArrivalArmed   = 0;   // w
long g_k7ArrivalSent    = 0;   // s
long g_k7ArrivalExpired = 0;   // x
long g_k7ArrivalLiveSent = 0;  // v
long g_k7ArrivalResumed = 0;   // r

// Live count of orders whose arrival wait is armed right now (open= above).
long K7ArrivalOpenWaits()
{
	long n = 0;
	for (int i = 0; i < g_orderCount; ++i)
		if (g_orders[i].active && g_orders[i].k7ArrivalWaitSince > 0.0) ++n;
	return n;
}


void ResetOrders()
{
	for (int i = 0; i < MAX_ISLAND_ORDERS; ++i)
	{
		if (g_orders[i].active && g_orders[i].k7ArrivalWaitSince > 0.0)
			g_k7ArrivalResumed++;
		g_orders[i].active = false;
	}
	g_orderCount = 0;
}
} // namespace order_tracker_detail
using namespace order_tracker_detail;

// =========================================================================
// Adapter (main thread only) -- islands.cpp's IslandTick / IslandReset
// call these. Shared helpers are declared in islands_reissue_internal.h;
// their definitions are with the tracker or its responsibility files.
// =========================================================================


// IslandTick's save-load / new-ZoneManager branch: both tracker tables.
void IslandReissueReset()
{
	ResetOrders();
	ResetReissueChecks();
}

// IslandReset() (preload_saveload.cpp's full reset): the pending-check table
// only; it does not call ResetOrders(). ResetBuilder() is the builder-side
// counterpart in islands.cpp.
void IslandReissueResetChecks()
{
	ResetReissueChecks();
}

namespace order_tracker_detail {
}
using namespace order_tracker_detail;
// IslandTick, every frame: resolve due checks, sample K7's end-of-frame
// signatures, then poll every tracked order.
// While paused, only the clock rebase in k7_reissue.cpp runs -- no check resolves, no
// signature is sampled, no order is polled, so no stall opens, no wait arms
// and no re-issue sends until the game actually resumes.
void IslandReissuePollTick(uintptr_t zm, double now)
{
	bool paused = ZonePauseIsPaused();
	K7RebasePausedClocks(paused, now);
	if (paused) return;

	ResolveDueReissueChecks(now);
	if (K7FormOn())
		K7SampleSignatures(now, paused);
	PollOrders(zm, now);
}


// Main-thread reissue and K7 diagnostic appenders.
// Field and line order are the shipped log format for both output variants; do not reorder.
// IslandTick's KEO_DEBUG "Islands:" line: the reissue/K7 fields.
void IslandReissueAppendDiag(std::ostringstream& ss)
{
	ss << " reissue=" << InterlockedCompareExchange(&g_reissues, 0, 0)
	   << " reissuePost=s" << g_reissuePostSent << "/l" << g_reissuePostLast
	   << "/o" << g_reissuePostOther
	   << " reissueCheckDropped=" << g_reissueCheckDropped
	   << " reissueCheckEarly=" << g_reissueCheckEarly;
	// K7 (all 0 while the deleted-order form is off).
	ss << " delPark=" << g_k7DelPark
	   << " delReissue=" << g_k7DelReissue
	   << " cancelDrop=s" << g_k7CancelStop << "/j" << g_k7CancelJob << "/t" << g_k7CancelTask
	   << " gameDrop=" << g_k7GameDrop
	   << " gameDropWhy=u" << g_k7DropUnconcious
	   << "/h" << g_k7DropOrderHead
	   << "/x" << g_k7DropTaskSwap
	   << "/xo" << g_k7DropTaskSwapNonCombat
	   << "/xe" << g_k7DropTaskSwapExpired
	   << " apdDrop=" << g_k7ApdDrop;
	IslandReissueAppendSpanDiag(ss);
}

// Printed on the PROD-visible IslandSpan: line (islands.cpp) every mode, and
// folded into the DEV Islands: line above (which is compiled out entirely in
// PROD).
void IslandReissueAppendSpanDiag(std::ostringstream& ss)
{
	ss << " delRefuse=h" << g_k7RefuseHyst
	   << "/g" << g_k7RefuseSig
	   << "/d" << g_k7RefuseDest29
	   << "/n" << g_k7RefuseDestNow
	   << "/c" << g_k7RefuseCarried
	   << "/s" << g_k7RefuseInSomething
	   << "/a" << g_k7RefuseHit
	   << "/e" << g_k7RefuseEnemies
	   << "/t" << g_k7RefuseThreats
	   << "/o" << g_k7RefuseHold
	   << "/f" << g_k7RefuseNear
	   << "/z" << g_k7RefuseZones
	   << "/r" << g_k7RefuseDestReady
	   << "/k" << g_k7RefuseCooldown
	   << "/b" << g_k7RefuseBudget
	   << " k7Hold=s" << g_k7HoldStarted << "/r" << g_k7HoldResumedSend
	   << " k7HoldWould=" << g_k7HoldWould
	   << " k7DestReadyTimeout=" << g_k7DestReadyTimeout
	   << " k7Arrive=w" << g_k7ArrivalArmed
	   << "/s" << g_k7ArrivalSent
	   << "/x" << g_k7ArrivalExpired
	   << "/v" << g_k7ArrivalLiveSent
	   << "/r" << g_k7ArrivalResumed
	   << "/open" << K7ArrivalOpenWaits();
}
