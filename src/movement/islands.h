// islands.h — island routing hooks. The component overlay was built to route
// around zones the mod held outside the game's own tracking sets; an adopted
// cell now enters Set A and, through the game's own machinery, Set B, so the
// overlay's component state is permanently empty and its answer never
// applies. The hooks stay live for the far-span rule (isInIsland) and the
// edge-ring filter (getIsland).
// Depends on: config.h
//
// Threading contract:
//   - Builder (IslandTick, IslandRequestRebuild, IslandNoteOrder,
//     IslandDescribeStuck, IslandEmulateCrossing): MAIN THREAD ONLY.
//   - Hooks (hook_isInIsland, hook_getIsland): any thread that calls
//     CharMovement::setDestination or the smell picker (main, AI, render back
//     thread). They take no locks, allocate nothing, log nothing; they read the
//     published snapshot through a seqlock and bump Interlocked counters.
//   - IslandCountReadiness: any thread (hook_isContentPending).
//
// What the overlay does:
//   - readiness-gate side effects on the main thread only + per-thread counters;
//   - builder, snapshot, load reset, emulation, PLAYER STUCK fields;
//   - both hooks live (subject to the islandFix INI key);
//   - parked-squad re-issue (a) + tier-ordered registration (b);
//   - deleted-order re-issue (K7): a player move order the engine
//       deleted short of its destination (task 29 -> -1, empty order deque,
//       a partial-path-end or path-failure signature) is re-issued, solo,
//       under the existing cooldown and MAX_REISSUES cap; the PlayerInterface
//       cancel hooks (stop key, clearing job, nearest-character task) and
//       game-initiated clears / appended orders drop tracking instead.
//       Needs islandDeletedReissue and all three cancel hooks.

#ifndef KEO_ISLANDS_H
#define KEO_ISLANDS_H

#include "base/config.h"


// Tracker cooldown (seconds): no character gets two tracker move orders
// within this window, whichever path sent the first one -- solo
// (island_reissue.cpp ReissueOrder) or group (formation_query.cpp FormationReissueTravel,
// whose own per-group check uses it too).
const double REISSUE_COOLDOWN = 2.0;


// =========================================================================
// Readiness-gate per-thread counters (always compiled)
// =========================================================================

// Called from hook_isContentPending on whichever thread the game uses.
// notReady = original returned 0; sectionsPending = notReady with sections > 0.
void IslandCountReadiness(bool notReady, bool sectionsPending);

// Main thread, every frame from hook_updateCameraZone (before the stats reporters).
void IslandTick(void* zoneMgr, double now);


// =========================================================================
// Hooks (installed by hook_manifest.cpp when preloadEnabled; both or neither go live)
// =========================================================================

bool  hook_isInIsland(void* zoneA, void* zoneB);
void* hook_getIsland(void* zoneMgr, void* zone, void* lektorOut);

// hook_manifest.cpp: called once after both hooks installed successfully.
void IslandSetHooksInstalled(bool installed);

// True when the overlay answers the hooks (installed && islandFix).
bool IslandHooksLive();


// =========================================================================
// Rebuild requests (main thread)
// =========================================================================

// zone_life.cpp: called after an unload changes what the overlay should see
// (forces a rebuild this frame). The mod-component eligibility test this
// fed (IslandMarkModZone, a zone the mod published as accessible outside
// Set A/B) is gone: the mod never writes +177 itself any more, so Rebuild's
// component roots, which seed only from that eligibility test, now seed zero
// components every time -- see the comment on Rebuild() in island_components.cpp.
void IslandRequestRebuild();

// preload_saveload.cpp (ClearPreloadState): full reset of the snapshot and tracker.
void IslandReset();


// =========================================================================
// Router emulation + diagnostics (main thread)
// =========================================================================

// Emulate CharMovement::computeProjectedDest for a character standing in
// charZone, along the ray from (destX,destZ) toward (posX,posZ), using the
// island list the router receives in the CURRENT configuration (vanilla when
// the hooks pass through, overlay when live). Returns the RAW first crossing
// (before the 300-unit navmesh snap). false = no island zone crossed.
bool IslandEmulateCrossing(void* zoneMgr, void* charZone,
                           float destX, float destZ, float posX, float posZ,
                           float* outX, float* outZ);

// Extra fields for the PLAYER STUCK diagnostic line (player_task_snap.cpp).
struct IslandStuckInfo {
	float wpX, wpZ;          // CharMovement::pathDestination (+0xE8)
	int   movingToEdge;      // +0x370
	int   edgeCounter;       // +0x368
	int   selfComp;          // liveComp of the character's zone (-1 none)
	bool  haveCrossing;      // emulation found a crossing
	float xd;                // |pos - raw crossing| when haveCrossing
	int   nextGX, nextGY;    // zone just beyond the crossing (or grid walk fallback)
	int   nextComp;          // liveComp(next)
	int   nextLabel;         // next zone +0x20
	int   nextLoading;       // next zone +176
	int   nextAccess;        // next zone +177
	// The two inputs the engine's own same-island test reads, for the
	// character's own zone and for the zone holding the order's destination.
	// setDestination only routes to an island edge when the test says the two
	// zones differ; a positive answer sends a direct path to the destination
	// however far away it is. haveSelf/haveDest say whether the zone behind
	// each label could be resolved at all, so an unread label is never
	// reported as the label value 0.
	bool  haveSelf, haveDest;
	int   selfLabel, destLabel;
	int   destGX, destGY;
	int   cellSpan;          // Chebyshev cell distance, -1 when either is unresolved
	int   sameIsland;        // vanilla answer: 1, 0, or -1 unknown
};
// The same-island test's two inputs and the cell span between them, for an
// arbitrary position/destination pair. Separate from IslandDescribeStuck so the
// per-frame arrival latch can record them at the moment the engine declares
// arrival: the labels are reassigned from scratch on every island
// recalculation and zeroed when a cell deactivates, so a value read a second
// later is not the value the routing branch acted on.
bool IslandSampleLabels(void* zoneMgr, float posX, float posZ,
                        float destX, float destZ,
                        int* outSelfLabel, int* outDestLabel, int* outSpan);

bool IslandDescribeStuck(void* zoneMgr, uintptr_t charMov,
                         float posX, float posZ, float destX, float destZ,
                         IslandStuckInfo* out);

// Any thread: the far-span rule answers the hook (both hooks installed and
// islandFarSpan > 0). Fixed at install.
bool IslandFarSpanArmed();


// Reasons an order from outside the tracker overtook a pending re-issue check
// inside its 1 s window. The resolved "Island reissue result:" line then
// carries click=1, because a post=other result may be that order, not the
// re-issue.
enum IslandOvertakeReason
{
	ISLAND_OVERTAKEN_CLICK = 1    // player move order (IslandNoteOrder)
};

// Flags `character`'s pending re-issue check, if it has one (no early
// resolve, no dereference of `character`). Main thread only.
void IslandFlagReissueOvertaken(uintptr_t character, int reason);

// order_hook.cpp: called for every selected character of a task-29 (move) order.
void IslandNoteOrder(uintptr_t character, const float* location);

// order_hook.cpp: called for every selected character of a NON-move (task != 29)
// player order. Drops that character's IslandOrder if it has one -- a later
// non-move order (attack, job, pick-up, talk) ends the relevance of an old
// tracked move order the same way arrival or a new move order does.
void IslandDropOrder(uintptr_t character);


// =========================================================================
// Shared re-issue helpers (main thread only)
//
// Used by both island_reissue.cpp (ReissueCharacter, per-character re-issue) and
// formation_query.cpp (FormationReissueTravel, per-member re-issue). Declared here
// instead of duplicated per file.
// =========================================================================

// (b) Direction-aware nudge, replacing the fixed one-sided +3 on x.
// CharMovement::setDestination_Vec3 drops a new order within 2 units of the
// last requested destination (+0xDC) while routing to an island edge. When
// (*x,*z) is within 8 units of (lastX,lastZ), push it to
// lastDest + 8*unit(sent - lastDest); when the two coincide (length < 0.01),
// push +8 on x only (z left at lastZ). No-op when already >= 8 units away.
inline void IslandNudgeAwayFromLastDest(float lastX, float lastZ, float* x, float* z)
{
	const float NUDGE_DIST    = 8.0f;
	const float COINCIDE_DIST = 0.01f;
	float dx = *x - lastX, dz = *z - lastZ;
	float len = sqrtf(dx * dx + dz * dz);
	if (len >= NUDGE_DIST) return;
	if (len < COINCIDE_DIST)
	{
		*x = lastX + NUDGE_DIST;
		*z = lastZ;
		return;
	}
	float scale = NUDGE_DIST / len;
	*x = lastX + dx * scale;
	*z = lastZ + dz * scale;
}

// (a) Discriminator line. Fields read before fn_moveOrder (playerMoveOrderDefault,
// vtable+0x318) is called: +0xDC (last dest), +0xE8 (pathDestination), +0x370
// (movingToEdge), +0x368 (edge counter), +0xC4 (position), the HavokCharacter
// path state (+0x90) and arrival code (+136), and the character's current
// order type (0 = no cached order / not a move order). Safe with a null
// CharMovement or HavokCharacter (fields read as 0, orderType as -1); the
// caller still gets a trace with valid=false.
struct IslandReissueTrace
{
	bool  valid;
	float lastX, lastZ;   // +0xDC before the call
	float wpX, wpZ;       // +0xE8 before the call
	float posX, posZ;     // +0xC4 before the call (for moved= at resolve time)
	int   edge;           // +0x370 before the call
	int   edgeCtr;        // +0x368 before the call
	int   hcPathState;    // HavokCharacter+0x90 before the call
	int   hcArrival;      // HavokCharacter+136 before the call
	int   orderType;      // current order type before the call (-1 = none/no chain)
};

void IslandCaptureReissueTrace(uintptr_t character, IslandReissueTrace* out);

// Single owner of the post=sent|last|other classification. It runs only at
// resolve time (see IslandRecordReissueCheck below), for both the per-member
// lines and the group summary count.
enum IslandReissuePost { ISLAND_POST_SENT, ISLAND_POST_LAST, ISLAND_POST_OTHER };
IslandReissuePost IslandClassifyReissuePost(uintptr_t character, float sentX, float sentZ,
                                            const IslandReissueTrace& pre);

// Deferred (a) discriminator.
//
// playerMoveOrderDefault hands the order to the AI task system, which applies
// it asynchronously, so +0xDC read in the same tick as the call always still
// held the previous destination. The caller now records a pending check
// right after fn_moveOrder; islands.cpp resolves it from IslandTick at the
// first tick at least REISSUE_RESULT_DELAY (1.0 s) after `now`,
// re-validating the character against the player squad list first (dropped
// silently and counted as reissueCheckDropped= when it is gone). The
// resolved line is:
//   Island reissue result: <label> post=sent|last|other d=<|last-sent|>
//     order=<type> edge=<a>/<b>-><a>/<b> wp=(x,z)->(x,z)
//     ps=<a>-><b> hc136=<a>-><b> dt=<ms> moved=<units> [click=1]
// where every -> pair is pre-order -> resolve time, and click=1 marks a
// player move order from outside the tracker inside the window
// (IslandFlagReissueOvertaken). `label` identifies the character
// ("char@<hex>", "group N member K char@<hex>"); it is copied.
//
// `dispatch` groups the members of one FormationReissueTravel call in summary
// mode (group above 6 members): -1 = none (every result prints its own line).
void IslandRecordReissueCheck(uintptr_t character, const char* label,
                              float sentX, float sentZ,
                              const IslandReissueTrace& pre, double now, int dispatch);

// Summary-mode dispatch (FormationReissueTravel, groups above 6 members):
// per-member lines only for non-sent results, plus one
//   Island reissue result: group <slot> summary <sent>/<total> sent
// line once every member recorded under the dispatch has resolved or been
// dropped (dropped members count toward total, not sent).
// Begin returns -1 when summaryMode is false or the dispatch table is full
// (then every member prints its own line). End closes the dispatch to new
// members; it must be called once for every Begin, even when nothing was sent.
int  IslandBeginReissueDispatch(int slot, bool summaryMode);
void IslandEndReissueDispatch(int dispatch);

// True if `character` was re-issued by the tracker (ReissueOrder, solo or
// as part of a formation group) within the last REISSUE_COOLDOWN seconds.
// FormationReissueTravel skips sending a member a second move order inside
// one cooldown window (e.g. it was already re-issued solo this cycle via
// the (c) per-member path).
bool IslandRecentlyReissued(uintptr_t character, double now);

// Group-then-solo cross-tick residual: stamps `character`'s own
// IslandOrder.lastReissueTime (the same field IslandRecentlyReissued reads)
// to `now`, WITHOUT touching reissueCount or parked. FormationReissueTravel
// calls this for every member it actually sends fn_moveOrder to
// (representative included), so a member whose order is swallowed by the
// group blast still carries a fresh cooldown timestamp even
// though its own park state was never touched -- closing the gap where a
// first-ever solo park-transition for that member would otherwise call
// ReissueOrder immediately, which the cooldown guard should, but previously
// could not, block (lastReissueTime was still 0.0). No-op if `character` has
// no active IslandOrder entry.
void IslandMarkReissued(uintptr_t character, double now);


// =========================================================================
// K7: PlayerInterface cancel hooks (main thread only)
// =========================================================================
//
// Pass-through detours on the player's own ways to end a move order
// (bindings.h RVA_STOP_CHARACTERS_MOVEMENT / RVA_ADD_JOB_SELECTED /
// RVA_ADD_TASK_NEAREST). Each calls the original with every argument
// forwarded unchanged, then the matching IslandNoteCancel*. Main thread
// only; no logging, no allocation, no locks. Installed by hook_manifest.cpp.
void hook_stopCharactersMovement(void* thisPI);
void hook_addJobSelected(void* thisPI, int task, void* subject, bool shift,
                         bool add, const float* location);
void hook_addTaskNearest(void* thisPI, void* dest, int task, void* subject, bool shift,
                         const float* location, bool noAnimals);

// Called after the original returns. Each drops the tracked IslandOrder of
// the characters the player's action cancelled, counted as
// cancelDrop=s<n>/j<n>/t<n> on the Islands: diag line; all three do nothing
// while the deleted-order form is off (see IslandCancelHooksLive).
//   Stop: every selected character (PlayerInterface::selectedCharacters).
//   Job:  every selected character when `add` is 0 (Character::addJob's
//         addDontClear: only then are the orders cleared); nothing otherwise.
//   NearestTask: every tracked character whose order state changed across
//         the original, against the snapshot hook_addTaskNearest takes
//         before it (the one ordered character is not known in advance).
void IslandNoteCancelStop(void* playerInterface);
void IslandNoteCancelJob(void* playerInterface, bool add);
void IslandNoteCancelNearestTask(void* playerInterface);

// hook_manifest.cpp: which of the three installed. The deleted-order re-issue (and
// every K7 drop) is off unless all three did (IslandCancelHooksLive) and
// the islandDeletedReissue INI key is on, since a cancel it cannot see
// would read as an engine deletion; PollOrders logs the reason once (DEV).
void IslandSetCancelHooksInstalled(bool stop, bool job, bool task);
bool IslandCancelHooksLive();

// With the deleted-order form on, true while `character`'s current
// task is neither the move (29) nor none (-1): another task preempts it with
// the move still queued. No tracker form re-issues to it meanwhile; the
// formation group re-issue skips such a member. Main thread; `character`
// must be live (the caller's own player-list test).
bool IslandK7Preempted(uintptr_t character);

// PLAYER STUCK decode of the deleted-order form (player_task_snap.cpp k7=):
//   off  the form is off (islandDeletedReissue or a cancel hook missing)
//   -    no tracked move order for the character
//   new  tracked, task 29 not seen yet
//   trk  tracked, the move ran (task 29 seen), not deleted
//   arr  an arrival wait is armed, waiting on the destination cell (only
//        while k7ArrivalTrigger can actually send -- with it false this
//        reads the same as a build without the arrival mechanism)
//   pre  preempted: another task runs, the move still queued
//   del  the deleted state holds (task -1, empty deque after task 29)
//   held k7PostDeathHold: a died-first combat swap is being held
// Checked in held/del/arr/pre/trk order, so an armed arrival wait never masks
// a held or deleted state that order_outcome.cpp's stops= attribution
// already relies on. Main thread; compares pointers only.
const char* IslandK7StuckForm(uintptr_t character);

// Character::isUnconcious (vtable slot 6), exposed for order_outcome.cpp's
// ko= exclusion: medical.unconcious || dead || isRagdoll() ||
// _isBeingCarried || getProneState() >= PS_PLAYING_DEAD. Main thread.
bool IslandK7IsUnconcious(uintptr_t character);

// "reached"/"failed"/"-": the deleted-order form's own end signature for
// `character` (order_outcome.cpp's stops= guess). Main thread.
const char* IslandK7StopSig(uintptr_t character);

// HavokCharacter::characterState (the arrival flag PLAYER STUCK's hc= prints)
// for `character`'s CharMovement, or false when the chain (movement or
// HavokCharacter) is unreadable -- `*outHc136` is then left untouched. Main
// thread.
bool IslandReadHc136(uintptr_t character, int* outHc136);


#endif // KEO_ISLANDS_H
