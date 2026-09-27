// islands_reissue.cpp - main-thread reissue tracker state and adapters.
//
// Owns the single definitions of the IslandOrder table, pending checks, K7
// counters, nearest-task snapshot and pause clocks. Orders, sends, K7
// observation, cancel hooks and diagnostics live in adjacent movement files.
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


namespace islands_reissue_detail {

const int    MAX_ISLAND_ORDERS   = 64;
const double ORDER_POLL_INTERVAL = 0.25;
const float  PARK_WP_DIST        = 20.0f;   // pathDestination within this of pos
const float  PARK_MIN_DEST_DIST  = 100.0f;  // farther than this from the order destination
const float  MISSED_ADVANCE_DIST = 350.0f;  // 300-unit snap + arrival tolerance
const float  GROWTH_THRESHOLD_SQ = 40.0f;   // the router's own threshold (squared)
const float  UNPARK_DIST         = 50.0f;
const int    MAX_REISSUES        = 8;
// REISSUE_COOLDOWN (2.0 s) lives in islands.h: formation.cpp's group check
// uses the same constant.
const double RETRY_DELAY         = 1.5;
// (c) The stopped form of the park test in island_orders.cpp must hold
// continuously for this long before it counts as parked -- a single bad poll
// (mid-frame state change, a still-settling order) must not park a character
// about to move again. The edge form keeps its existing (unhysteresised)
// behaviour.
const double STOPPED_HYSTERESIS  = 3.0;


IslandOrder   g_orders[MAX_ISLAND_ORDERS];
int           g_orderCount = 0;
double        g_lastOrderPoll = 0.0;
volatile long g_reissues = 0;

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

// Which cancel hooks hook_manifest.cpp installed (IslandSetCancelHooksInstalled).
bool g_cancelStopInstalled = false;
bool g_cancelJobInstalled  = false;
bool g_cancelTaskInstalled = false;

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
// One pending check per character at most (a newer order resolves the old
// one). Checks come from solo IslandOrders (MAX_ISLAND_ORDERS = 64) AND from
// every member of a group dispatch (MAX_FORMATION_GROUPS 8 x
// MAX_FORMATION_MEMBERS 30), which are not IslandOrders. The worst case is
// therefore 64 + 240 = 304 distinct characters with a check pending inside
// the same 1 s window, which 256 does NOT cover. The table is left at 256
// (squads that large parking together are not expected): past 256, a new
// check resolves the oldest one early with its current state instead of
// dropping a line, counted as reissueCheckEarly= and visible as a short dt=.
const int    MAX_REISSUE_CHECKS     = 256;
ReissueCheck    g_reissueChecks[MAX_REISSUE_CHECKS];
// Islands: summary-line counters (cumulative for the session, main thread).
long g_reissuePostSent     = 0;
long g_reissuePostLast     = 0;
long g_reissuePostOther    = 0;
long g_reissueCheckDropped = 0;
long g_reissueCheckEarly   = 0;   // resolved before the delay because the table was full
K7NearSnap g_k7NearSnap[MAX_ISLAND_ORDERS];
int        g_k7NearSnapCount = 0;
} // namespace islands_reissue_detail
using namespace islands_reissue_detail;

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

namespace islands_reissue_detail {
bool   g_k7WasPaused    = false;
double g_k7PauseStarted = 0.0;
}
using namespace islands_reissue_detail;
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

