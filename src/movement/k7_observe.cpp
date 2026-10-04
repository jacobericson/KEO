// k7_observe.cpp - main-thread K7 order, signature and hold observation.
// Reads the current task and deque state before the reissue decision.
#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "zone/preload/preload.h"
#include "movement/formation.h"
#include "movement/formation_gather_policy.h"
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
// -------------------------------------------------------------------------
// K7: deleted-order form (main thread only).
//
// The engine deletes a player's move order when it treats a partial path's
// end as arrival (CharMovement's isDestinationReached shortcut: hc136 == 1
// with movingToEdge clear) or when the path fails (path state 3):
// AITaskSytem::bodyTaskComplete 0x50CD40 pops the order and the task goes
// from 29 to -1 with an empty order deque, short of the destination. The
// stopped park form in island_orders.cpp needs a live task-29 order. The edge
// form sees movingToEdge cleared after deletion, so neither form sees this state.
// The stop key leaves the same "task -1, deque empty" state, which is why
// the form is interlocked with the three PlayerInterface cancel hooks.
//
// Layout sources: the typed IDB.
// -------------------------------------------------------------------------

const size_t K7_VT_IS_UNCONCIOUS       = 0x30;  // vtable slot 6 = Character::isUnconcious 0x5C9380 in the
                                                // Character, CharacterHuman and CharacterAnimal vtables

// OrdersReceiver::orders.list: a VS2010 std::deque<Tasker*> (typed IDB
// std::_Deque_val<Tasker*>: _Map +0x08, _Mapsize +0x10, _Myoff +0x18,
// _Mysize +0x20), two Tasker* per block. ActionDeque::passOnCurrentTask
// 0x518210 reads the front as _Map[(_Myoff >> 1) mod _Mapsize][_Myoff & 1].
const size_t K7_OFF_TS_ORDERS_LIST     = 0x40;
const size_t K7_DQ_MAP                 = 0x08;
const size_t K7_DQ_MAPSIZE             = 0x10;
const size_t K7_DQ_MYOFF               = 0x18;
const size_t K7_DQ_MYSIZE              = 0x20;
static_assert(K7_OFF_TS_ORDERS_LIST == KLIB_OFF_OrdersReceiver_orders + KLIB_OFF_ActionDeque_list,
              "K7_OFF_TS_ORDERS_LIST composed parity");
static_assert(K7_DQ_MYSIZE == KLIB_OFF_TaskDeque__Mysize, "K7_DQ_MYSIZE parity");
static_assert(K7_OFF_TS_ORDERS_LIST + K7_DQ_MYSIZE == PT_OFF_TS_DEQUE_SIZE, "K7 deque size parity");
KLIB_ASSERT_OFFSET(TaskDeque__Map, K7_DQ_MAP);
KLIB_ASSERT_OFFSET(TaskDeque__Mapsize, K7_DQ_MAPSIZE);
KLIB_ASSERT_OFFSET(TaskDeque__Myoff, K7_DQ_MYOFF);

// K7_SIG_WINDOW (end signature at most this long before the deletion) is
// shared with k7_swap_policy.h's K7ClassifySwap, which uses the same window
// to decide whether a died-first swap and a later deletion are one episode.
const long long K7_DEQUE_SANE  = 1000;    // order deque sizes outside [0, 1000] are rejected
const double    K7_HOLD_MARGIN = 0.5;     // swap must trail the signature onset by this long
const double    K7_HOLD_MAX    = 60.0;    // maximum hold duration before it expires (xe)

// K7 interlock: the deleted-order form (and every K7 drop) runs only
// with islandDeletedReissue on AND all three cancel hooks installed; a
// cancel it cannot see would read as an engine deletion. Quiet: the cancel
// detours call it too.
bool K7FormOn()
{
	return movement::g_movementCfg.islandDeletedReissueEnabled && IslandCancelHooksLive();
}

// PollOrders' copy: logs the reason once (DEV) when the form is off.
bool K7FormOnLogged()
{
	bool on = K7FormOn();
	static bool logged = false;
	if (!on && !logged)
	{
		logged = true;
		std::ostringstream ss;
		ss << "Islands: deleted-order re-issue off (";
		if (!movement::g_movementCfg.islandDeletedReissueEnabled)
			ss << "islandDeletedReissue=false";
		else
			ss << "cancel hooks not all installed: stop=" << (g_cancelStopInstalled ? 1 : 0)
			   << " job=" << (g_cancelJobInstalled ? 1 : 0)
			   << " task=" << (g_cancelTaskInstalled ? 1 : 0);
		ss << ")";
		LogDebug(ss.str());
	}
	return on;
}
} // namespace order_tracker_detail
namespace order_tracker_detail {


bool K7ReadOrders(uintptr_t character, K7OrderState* st)
{
	st->ok = false;
	st->size = 0;
	st->head = 0;
	st->headType = -1;
	st->curType = -1;
	if (!character) return false;
	uintptr_t ai = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_ai, PT_OFF_CHAR_AI));
	if (!ai) return false;
	uintptr_t ts = *(uintptr_t*)(KLIB_MEMBER(3, ai, AI_taskSystemAI, PT_OFF_AI_TASKSYS));
	if (!ts) return false;
	long long size = *(long long*)(KLIB_MEMBER(3, ts, OrdersReceiver_orders_list__Mysize, PT_OFF_TS_DEQUE_SIZE));
	if (size < 0 || size > K7_DEQUE_SANE) return false;
	st->size = size;
	if (size > 0)
	{
		uintptr_t dq = ts + K7_OFF_TS_ORDERS_LIST;
		uintptr_t map = *(uintptr_t*)(KLIB_MEMBER(3, dq, TaskDeque__Map, K7_DQ_MAP));
		unsigned long long mapSize = *(unsigned long long*)(KLIB_MEMBER(3, dq, TaskDeque__Mapsize, K7_DQ_MAPSIZE));
		unsigned long long off     = *(unsigned long long*)(KLIB_MEMBER(3, dq, TaskDeque__Myoff, K7_DQ_MYOFF));
		if (map && mapSize > 0 && mapSize < (1ull << 20))
		{
			unsigned long long block = off >> 1;
			if (block >= mapSize) block -= mapSize;
			if (block < mapSize)
			{
				uintptr_t blk = *(uintptr_t*)(map + 8 * (uintptr_t)block);
				if (blk) st->head = *(uintptr_t*)(blk + 8 * (uintptr_t)(off & 1));
			}
		}
		if (st->head)
		{
			uintptr_t td = *(uintptr_t*)(KLIB_MEMBER(3, st->head, Tasker_taskData, PT_OFF_TASKER_DATA));
			if (td) st->headType = *(int*)(KLIB_MEMBER(3, td, TaskData_key, PT_OFF_TASKDATA_TYPE));
		}
	}
	st->curType = ReadCharOrderType(character);
	st->ok = true;
	return true;
}

// Character::isUnconcious (vtable slot 6): medical.unconcious || dead ||
// isRagdoll() || _isBeingCarried || getProneState() >= PS_PLAYING_DEAD.
bool K7IsUnconcious(uintptr_t character)
{
	uintptr_t vt = *(uintptr_t*)character;
	if (!vt) return false;
	typedef bool (*isUnconcious_t)(uintptr_t);
	isUnconcious_t fn = (isUnconcious_t)(*(uintptr_t*)(vt + K7_VT_IS_UNCONCIOUS));
	return fn ? fn(character) : false;
}

// The character's zone and the next zone toward the destination are both
// accessible (+177). The next zone is the first zone other than the
// character's own on the straight line to the destination, sampled every
// quarter zone up to two zones out; none = the destination is in this zone.
bool K7ZonesAccessible(uintptr_t zm, float posX, float posZ, float destX, float destZ)
{
	int gx, gy;
	if (!WorldToZoneGrid(posX, posZ, &gx, &gy)) return false;
	uintptr_t z = (uintptr_t)GetZoneEntry((void*)zm, gx, gy);
	if (!z || !IslandOverlayZoneAccessible(z)) return false;

	float sx = fabsf(zoneStepX), sz = fabsf(zoneStepZ);
	float stepLen = (sx < sz ? sx : sz) * 0.25f;
	float dx = destX - posX, dz = destZ - posZ;
	float dist = sqrtf(dx * dx + dz * dz);
	if (dist < 1.0f || stepLen < 1.0f) return true;
	float reach = 2.0f * (sx > sz ? sx : sz);
	if (reach > dist) reach = dist;
	int steps = (int)(reach / stepLen) + 1;
	for (int i = 1; i <= steps; ++i)
	{
		float s = stepLen * (float)i;
		if (s > reach) s = reach;
		int nx, ny;
		if (!WorldToZoneGrid(posX + dx / dist * s, posZ + dz / dist * s, &nx, &ny)) return false;
		if (nx == gx && ny == gy) continue;
		uintptr_t nz = (uintptr_t)GetZoneEntry((void*)zm, nx, ny);
		return nz && IslandOverlayZoneAccessible(nz);
	}
	return true;
}

// Every frame (IslandTick, before PollOrders): end signatures are
// transient -- CharMovement::halt 0x65F1E0 and HavokCharacter::clearPath run
// as the order ends -- so they are sampled per frame, not per 0.25 s poll.
// Only for orders whose task 29 has been seen (the previous order's path
// state must not count). Live-list test before any dereference. `paused`
// (GameWorld::paused, IslandReissuePollTick's own read) blocks a new arm
// outright -- nothing is simulating, so an end signature observed this frame
// proves nothing about whether the character has genuinely stopped.
void K7SampleSignatures(double now, bool paused)
{
	if (g_orderCount == 0) return;
	uintptr_t* stuff;
	unsigned int count;
	if (!TrackerPlayerList(&stuff, &count)) return;
	for (int i = 0; i < g_orderCount; ++i)
	{
		IslandOrder& o = g_orders[i];
		if (!o.active || !o.k7Seen29) continue;
		if (!TrackerListHas(stuff, count, o.character)) continue;
		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, o.character, Character_movement, OFF_CHAR_MOVEMENT));
		if (!cm) continue;
		uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
		if (!hc) continue;
		int hc136 = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
		int ps    = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));
		bool edge = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE)) != 0;
		float posX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
		float posZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
		bool reached = hc136 == 1 && !edge
		            && Dist2(posX, posZ, o.destX, o.destZ) > PARK_MIN_DEST_DIST * PARK_MIN_DEST_DIST;
		bool failed = ps == 3;
		bool sigNow = reached || failed;

		// The engine resumed the walk itself (back to FOLLOWING_PATH, no
		// path failure) -- nothing left to recover, and leaving the wait armed
		// would let a later, unrelated signature fire into a character that is
		// moving normally again.
		if (o.k7ArrivalWaitSince > 0.0 && hc136 == 2 /*FOLLOWING_PATH*/ && ps != 3)
		{
			o.k7ArrivalWaitSince = 0.0;
			o.k7ArrivalWouldFireTime = 0.0;
			g_k7ArrivalResumed++;
		}

		// Arm on the *rising edge* of the signature only (never while a
		// formation is still gathering, whose own gather-arrival move ends
		// with exactly this signature) -- not on every frame it continues to
		// hold, and not on a later, unrelated signature reusing an already-
		// spent arm. Independent of whether the order ever reaches the
		// "deleted" state below: a live continuation of task 29 (the state-6
		// re-request) keeps this armed. Covers a signature whose destination
		// cell is not-in-world right now, or one that read not-in-world within
		// the last K7_ARRIVAL_RECENT_TRANSITION seconds (k7DestLastNotIn,
		// updated once per poll in island_orders.cpp's PollOrders) -- the shape where the
		// leg was requested just before the cell streamed in and the
		// character stopped just after.
		//
		// The cheap tests (edge, gathering, distance) run first, every frame,
		// for every tracked entry; ClassifyZoneReadiness -- a try-shared
		// +0x200 scan -- only runs on a qualifying rising edge, at most once
		// per signature, not once per frame for every unarmed entry.
		if (o.k7ArrivalWaitSince <= 0.0 && sigNow && !o.k7ArrivalPrevSig)
		{
			int fslot = FormationSlotForCharacter(o.character);
			bool gathering = fslot >= 0 && FormationSkipWhileGathering(formationGroups[fslot].gathered, FormationMemberAlone(o.character));
			float dDestSq = Dist2(posX, posZ, o.destX, o.destZ);
			if (!gathering && dDestSq > K7_ARRIVAL_MIN_DIST_SQ)
			{
				int dgx = 0, dgy = 0;
				int cls = ZR_UNKNOWN;
				if (WorldToZoneGrid(o.destX, o.destZ, &dgx, &dgy))
				{
					int destCell[2] = { dgx, dgy };
					uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
					cls = ClassifyZoneReadiness(sectionMgr, destCell, /*splitMap*/false);
				}
				double secsSinceNotIn = (o.k7DestLastNotIn > 0.0) ? (now - o.k7DestLastNotIn) : -1.0;
				// K7ArrivalArmEdge re-checks the edge/gathering/distance terms
				// it was just given (harmless -- they were already true here),
				// keeping one tested predicate for both call sites.
				if (K7ArrivalArmEdge(o.k7ArrivalPrevSig, sigNow, gathering, paused, dDestSq, cls, secsSinceNotIn))
				{
					o.k7ArrivalWaitSince = now;
					o.k7ArrivalGX = dgx;
					o.k7ArrivalGY = dgy;
					g_k7ArrivalArmed++;
					if (!o.k7ArrivalArmLogged)
					{
						o.k7ArrivalArmLogged = true;
						int cgx = 0, cgy = 0;
						WorldToZoneGrid(posX, posZ, &cgx, &cgy);
						int span = IslandCellSpan(cgx, cgy, dgx, dgy);
						bool recentTransition = cls == K7_ARRIVAL_ZR_BUILDINGS_PENDING;
						std::ostringstream aa;
						aa << std::fixed << std::setprecision(0);
						aa << "K7 arrival armed: char@" << std::hex << (o.character & 0xFFFF) << std::dec
						   << " cell=(" << cgx << "," << cgy << ")"
						   << " destCell=(" << dgx << "," << dgy << ")"
						   << " dDest=" << sqrtf(dDestSq)
						   << " span=" << span
						   << " sig=" << (reached ? "reached" : "failed")
						   << " why=" << (recentTransition ? "recent" : "notIn");
						LogMsg(aa.str());
					}
				}
			}
		}
		o.k7ArrivalPrevSig = sigNow;   // every frame, outside the guard above

		if (reached) o.k7ReachedTime = now;   // a partial path's end counted as arrival
		if (failed)  o.k7FailedTime = now;    // path failed
		// The onset latch. Only K7Observe's task-29 poll clears it
		// (task29Poll=false here); this per-frame sample only arms it, and
		// never on the "last frame the signature held" value (reached/failed
		// above), which would always read "just now" at the swap poll.
		o.k7SigOnset = K7SigOnsetStep(o.k7SigOnset, reached || failed, false, now);
	}
}

// PollOrders, per tracked character (after the arrival test): samples the
// order state and applies the permanent drops. Returns true when the entry
// must be dropped (counted in gameDrop= / apdDrop=). *deleted = the
// deletion state (task 29 seen, now task -1 with an empty deque) holds.
// One line per permanent drop, logged in every variant: a drop is rare (at
// most a handful a session) and it costs the character every later re-issue,
// so the reason and the state it was read from are worth naming. curType and
// headType are -1 when the drop happened before the order state was read.
static void K7LogDrop(const IslandOrder& o, uintptr_t cm, const char* why,
                      int curType, int headType, long long dq)
{
	float posX = 0.0f, posZ = 0.0f;
	if (cm)
	{
		posX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
		posZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	}
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(0);
	ss << "K7 drop: char@" << std::hex << (o.character & 0xFFFF) << std::dec
	   << " why=" << why
	   << " cur=" << curType
	   << " head=" << headType
	   << " dq=" << dq
	   << " pos=(" << posX << "," << posZ << ")"
	   << " dDest=" << sqrtf(Dist2(posX, posZ, o.destX, o.destZ))
	   << " n=" << o.reissueCount;
	LogMsg(ss.str());
}

// Logged once per hold episode (not every poll it continues to
// hold), same char@/cur/pos shape as K7LogDrop. via names which timestamp
// armed the hold's deletion latch: "sig" (the signature onset itself, no
// deletion armed yet) or "del" (an already-latched deletion).
static void K7LogHold(const IslandOrder& o, uintptr_t cm, int curType, const char* via,
                      double sigAgo, bool observeOnly)
{
	float posX = 0.0f, posZ = 0.0f;
	if (cm)
	{
		posX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
		posZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	}
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "K7 hold" << (observeOnly ? " (observe)" : "") << ": char@"
	   << std::hex << (o.character & 0xFFFF) << std::dec
	   << " cur=" << curType
	   << " sigAgo=" << sigAgo
	   << " via=" << via
	   << " dDest=" << sqrtf(Dist2(posX, posZ, o.destX, o.destZ));
	LogMsg(ss.str());
}

bool K7Observe(IslandOrder& o, uintptr_t cm, double now, bool* deleted)
{
	*deleted = false;
	o.k7Preempted = false;

	// Game-initiated clears (ragdoll, carry, KO -> GET_UP): isUnconcious
	// turning true while tracked. Matches vanilla: no re-issue after recovery.
	if (K7IsUnconcious(o.character))
	{
		g_k7GameDrop++; g_k7DropUnconcious++;
		K7LogDrop(o, cm, "u", -1, -1, -1);
		return true;
	}

	K7OrderState st;
	if (!K7ReadOrders(o.character, &st)) return false;   // no information this poll

	// A non-29 order in the deque (GET_UP_STAND_UP after a KO, a job, ...).
	// The deque is written synchronously on the main thread
	// (playerMoveOrderDefault -> Character::addOrder), so the tracked order
	// is already the head at the first poll.
	if (st.size >= 1 && st.headType >= 0 && st.headType != ORDER_TYPE_MOVE)
	{
		g_k7GameDrop++; g_k7DropOrderHead++;
		K7LogDrop(o, cm, "h", st.curType, st.headType, st.size);
		return true;
	}

	// An order appended behind the tracked one (the shift-drag waypoint:
	// Character::updateLastTask -> OrdersReceiver::updateLastOrder 0x507F00
	// appends; there is no in-place retarget). Never re-issue the old one.
	if (st.size > 1) { g_k7ApdDrop++; return true; }

	if (st.curType == ORDER_TYPE_MOVE)
	{
		o.k7Seen29 = true;
		o.k7HaveDest29 = true;
		o.k7Dest29X = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		o.k7Dest29Z = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		// The engine resumed a move itself: clear the swap classifier's
		// per-episode state and any hold, exactly as a task-29 poll clears
		// k7SigOnset (K7SigOnsetStep).
		o.k7Last29Time = now;
		o.k7SigOnset = K7SigOnsetStep(o.k7SigOnset, false, true, now);
		o.k7SwapSeenTime = 0.0;
		o.k7PostDeathHold = false;
	}
	else if (st.curType != -1 && o.k7Seen29 && st.size == 0)
	{
		// Another task after the order ran, and the order is gone from the
		// deque (a job, pickup, combat or dialog that replaced it). Before
		// task 29 is seen the current action is still whatever preceded the
		// order: the AI thread switches it asynchronously.
		//
		// The order's own end (a partial path's arrival, or a path
		// failure -- k7SigOnset) can have come BEFORE this swap, which is not
		// today's "another task pushed the order out" story: the move was
		// already over, and the AI's own switch into the new task is not a
		// clear the mod should treat the same way. K7ClassifySwap tells the
		// two apart; only a genuine drop (K7_SWAP_DROP) keeps today's exact
		// behaviour and reason "x".
		if (o.k7SwapSeenTime <= 0.0) o.k7SwapSeenTime = now;
		K7SwapVerdict verdict = K7ClassifySwap(o.k7Last29Time, o.k7SigOnset, o.k7DeletedSince,
		                                       o.k7SwapSeenTime, now, st.curType,
		                                       K7_HOLD_MARGIN, K7_HOLD_MAX);
		if (verdict == K7_SWAP_HOLD) g_k7HoldWould++;   // counts in every mode, including off

		if (movement::g_movementCfg.cfg_k7PostDeathHold != K7_HOLD_OFF && verdict == K7_SWAP_HOLD)
		{
			bool observeOnly = (movement::g_movementCfg.cfg_k7PostDeathHold == K7_HOLD_OBSERVE);
			if (!o.k7PostDeathHold)
			{
				g_k7HoldStarted++;
				const char* via = (o.k7DeletedSince > 0.0) ? "del" : "sig";
				double sigAgo = now - ((o.k7DeletedSince > 0.0) ? o.k7DeletedSince : o.k7SigOnset);
				K7LogHold(o, cm, st.curType, via, sigAgo, observeOnly);
			}
			if (!observeOnly)
			{
				o.k7PostDeathHold = true;
				if (o.k7DeletedSince <= 0.0) o.k7DeletedSince = o.k7SigOnset;
				o.k7Preempted = true;
				return false;   // held: send nothing this poll, stay tracked
			}
			// observe: falls through and drops exactly as k7PostDeathHold=off would
		}

		const char* why = "x";
		if (verdict == K7_SWAP_DROP_NONCOMBAT)     { why = "xo"; g_k7DropTaskSwapNonCombat++; }
		else if (verdict == K7_SWAP_EXPIRED)       { why = "xe"; g_k7DropTaskSwapExpired++; }
		else                                       { g_k7DropTaskSwap++; }
		g_k7GameDrop++;
		K7LogDrop(o, cm, why, st.curType, st.headType, st.size);
		return true;
	}

	// A current task other than 29 while the move is still queued (the
	// only deque left here is empty or a task-29 head) is a preemption the
	// move resumes after, not a clear: STAND_STILL 62 (a freeze posted by an
	// approacher, TTL 3 s), SELF_PRESERVATION 32, a forced stumble 147, and
	// the like. The entry stays; PollOrders sends nothing while it lasts.
	if (st.curType != ORDER_TYPE_MOVE && st.curType != -1)
		o.k7Preempted = true;

	if (o.k7Seen29 && st.curType == -1 && st.size == 0)
	{
		if (o.k7DeletedSince <= 0.0) o.k7DeletedSince = now;
		*deleted = true;
	}
	else
	{
		// Never clobber a hold's deletion latch while curType != -1
		// (the swap branch above already returned for every such poll; this
		// path is unreachable while a hold is active). Kept as a defensive
		// no-op guard, not a behaviour change: k7PostDeathHold is only ever
		// true right after that branch returns.
		if (!o.k7PostDeathHold)
		{
			o.k7DeletedSince = 0.0;
			o.k7Counted = false;
		}
	}
	return false;
}
} // namespace order_tracker_detail
using namespace order_tracker_detail;
