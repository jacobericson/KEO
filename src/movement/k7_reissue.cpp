// k7_reissue.cpp - main-thread deleted and arrival reissues.
// Both send through ReissueOrder/playerMoveOrderDefault, then preserve pause clocks.
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
// Pinned by klib_member_fields.inc rows, verified against the typed IDB, and
// read through KLIB_MEMBER. Character::inSomething, Character::stats
// and CharStats::_holdPositionMode are shared with formation.cpp and live in game.h
// as OFF_CHAR_IN_SOMETHING / OFF_CHAR_STATS / OFF_STATS_HOLD.
const size_t K7_OFF_CHAR_BEING_CARRIED = 0x3D4; // Character::_isBeingCarried (bool)
KLIB_ASSERT_OFFSET(Character__isBeingCarried, K7_OFF_CHAR_BEING_CARRIED);
const float     K7_DEST_MATCH  = 50.0f;   // +0xDC vs the tracked destination
const double    K7_DEST_WAIT_MAX = 15.0;  // force-allow after this long of consecutive refusals

// The rest of the deletedParked predicate and the action. Called only while
// *deleted held this poll. Returns true when a re-issue was sent.
//
// Every early return below counts its own delRefuse= gate (one
// Interlocked-free long each; main thread only), so a session can say which
// gate held a specific character back instead of only "it did not send this
// poll".
bool K7TryDeletedReissue(IslandOrder& o, uintptr_t zm, uintptr_t cm, float posX, float posZ,
                         int gx, int gy, double now)
{
	// -1 held for STOPPED_HYSTERESIS.
	if (o.k7DeletedSince <= 0.0 || now - o.k7DeletedSince < STOPPED_HYSTERESIS) { g_k7RefuseHyst++; return false; }

	// An end signature within K7_SIG_WINDOW of the deletion (or since it).
	double sigTime = (o.k7ReachedTime > o.k7FailedTime) ? o.k7ReachedTime : o.k7FailedTime;
	if (sigTime <= 0.0 || sigTime < o.k7DeletedSince - K7_SIG_WINDOW) { g_k7RefuseSig++; return false; }
	const char* sig = (o.k7ReachedTime > o.k7FailedTime) ? "reached" : "failed";

	// The movement's destination is still the tracked one (+-50).
	const float matchSq = K7_DEST_MATCH * K7_DEST_MATCH;
	if (!o.k7HaveDest29 || Dist2(o.k7Dest29X, o.k7Dest29Z, o.destX, o.destZ) > matchSq)
	{ g_k7RefuseDest29++; return false; }

	// +0xDC now must match too, or have collapsed onto the position:
	// CharMovement::halt 0x65F1E0 copies pos into +0xDC when the order ends.
	// Anything else is an order issued through a non-hooked path (loot,
	// prospect, building buy, dialog) -- except for a held entry:
	// combat movement writes +0xDC while the fight runs, so this test would
	// refuse a held entry until it expires. The k7Dest29 test above already
	// guards a held entry against a genuinely different destination.
	if (!o.k7PostDeathHold)
	{
		float lastX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		float lastZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		if (Dist2(lastX, lastZ, o.destX, o.destZ) > matchSq && Dist2(lastX, lastZ, posX, posZ) > matchSq)
		{ g_k7RefuseDestNow++; return false; }
	}

	// Character state (isUnconcious was tested by K7Observe this poll: a
	// true answer dropped the entry).
	uintptr_t ch = o.character;
	// _isBeingCarried is also inside isUnconcious (tested earlier by K7Observe);
	// kept as a cheap direct read for the same poll.
	if (*(unsigned char*)(KLIB_MEMBER(3, ch, Character__isBeingCarried, K7_OFF_CHAR_BEING_CARRIED)))
	{ g_k7RefuseCarried++; return false; }
	if (*(int*)(KLIB_MEMBER(3, ch, Character_inSomething, OFF_CHAR_IN_SOMETHING)) != 0)
	{ g_k7RefuseInSomething++; return false; }
	if (*(unsigned char*)(KLIB_MEMBER(3, ch, Character__isLiterallyUnderMeleeAttackRightNowForSure, PT_OFF_CHAR_HIT)))
	{ g_k7RefuseHit++; return false; }
	uintptr_t ai = *(uintptr_t*)(KLIB_MEMBER(3, ch, Character_ai, PT_OFF_CHAR_AI));
	if (!ai) { g_k7RefuseEnemies++; return false; }   // can't confirm "no enemies" without it
	if (*(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_numEnemies, PT_OFF_AI_NUM_ENEMIES)) != 0)
	{ g_k7RefuseEnemies++; return false; }
	if (*(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_threats_count, PT_OFF_AI_THREATS)) != 0)
	{ g_k7RefuseThreats++; return false; }
	uintptr_t stats = *(uintptr_t*)(KLIB_MEMBER(3, ch, Character_stats, OFF_CHAR_STATS));
	if (!stats || *(unsigned char*)(KLIB_MEMBER(3, stats, CharStats__holdPositionMode, OFF_STATS_HOLD)))
	{ g_k7RefuseHold++; return false; }   // hold standing order

	// More than 100 units from the destination; zones accessible.
	if (Dist2(posX, posZ, o.destX, o.destZ) <= PARK_MIN_DEST_DIST * PARK_MIN_DEST_DIST)
	{ g_k7RefuseNear++; return false; }
	if (!K7ZonesAccessible(zm, posX, posZ, o.destX, o.destZ)) { g_k7RefuseZones++; return false; }

	// Backstop: the destination cell's own outdoor navmesh instance must be
	// in the world before the re-issue sends, or it burns one of
	// MAX_REISSUES on a search with nowhere to route. Try-lock only, never
	// state+0x28, never +0x1E0 held with +0x200. After K7_DEST_WAIT_MAX
	// seconds of consecutive refusals, allow anyway so a cell that never
	// gets an instance cannot strand the entry forever.
	// Always classified (delRefuse=r counts a would-refuse poll even with
	// k7DestReadyGate=off); only the actual return is gated on the key.
	{
		int dgx, dgy;
		if (WorldToZoneGrid(o.destX, o.destZ, &dgx, &dgy))
		{
			int destCell[2] = { dgx, dgy };
			// The classifier reads the NavMesh (SectionManager), not the zone manager.
			uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
			int cls = ClassifyZoneReadiness(sectionMgr, destCell, /*splitMap*/false);
			double waited = (o.k7DestWaitSince > 0.0) ? now - o.k7DestWaitSince : 0.0;
			bool allow = K7DestReadyAllows(cls, waited, K7_DEST_WAIT_MAX);
			if (!allow)
			{
				if (o.k7DestWaitSince <= 0.0) o.k7DestWaitSince = now;
				g_k7RefuseDestReady++;
				if (k7DestReadyGateEnabled) return false;
			}
			else
			{
				if (cls != ZR_BUILDINGS_PENDING) g_k7DestReadyTimeout++;   // allowed only by the 15 s cap
				o.k7DestWaitSince = 0.0;
			}
		}
		// WorldToZoneGrid failing on the destination itself is not this
		// gate's question (gridCalibrated is already checked by PollOrders);
		// nothing to classify, so the gate does not apply this poll.
	}

	// deletedParked holds. When the shared budget is already spent (by this
	// form's own last send, which said exhausted=1, or by the park forms),
	// say so once per deletion episode; nothing more is sent.
	if (!o.k7Counted)
	{
		o.k7Counted = true;
		g_k7DelPark++;
		if (o.reissueCount >= MAX_REISSUES)
		{
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(0);
			ss << "Island reissue skipped (deleted): char@" << std::hex << (o.character & 0xFFFF) << std::dec
			   << " pos=(" << posX << "," << posZ << ") sig=" << sig
			   << " dest=(" << o.destX << "," << o.destZ << ")"
			   << " n=" << o.reissueCount << "/" << MAX_REISSUES << " exhausted=1";
			LogMsg(ss.str());
		}
	}

	// Tracker cooldown rule and the shared MAX_REISSUES budget.
	if (IslandRecentlyReissued(o.character, now)) { g_k7RefuseCooldown++; return false; }
	if (o.reissueCount >= MAX_REISSUES) { g_k7RefuseBudget++; return false; }
	if (o.reissueCount == 0) { o.limitGX = gx; o.limitGY = gy; }   // budget-reset anchor

	std::ostringstream f;
	f << std::fixed << std::setprecision(0);
	f << "pos=(" << posX << "," << posZ << ") sig=" << sig << " moved=";
	if (o.k7HaveLastSend)
		f << sqrtf(Dist2(posX, posZ, o.k7LastSendX, o.k7LastSendZ));
	else
		f << "-";
	if (o.k7PostDeathHold) f << " held=1";
	std::string fields = f.str();

	// Always the character path: a formation member is evaluated and
	// re-issued solo, as in item (c).
	if (!ReissueOrder(o, now, "deleted", false, 0.0f, 0.0f, true, fields.c_str()))
		return false;
	IslandMarkReissued(o.character, now);
	g_k7DelReissue++;
	if (o.k7PostDeathHold) g_k7HoldResumedSend++;

	// The observe key: this is the actual K7 send an armed arrival wait was
	// measuring against. Log the would-have-sent line now that the real
	// latency is known; a wait that never reached the fire condition before
	// this send (k7ArrivalWouldFireTime still 0) logs nothing.
	if (o.k7ArrivalWouldFireTime > 0.0)
	{
		std::ostringstream ao;
		ao << std::fixed << std::setprecision(0);
		ao << "K7 arrival (observe): char@" << std::hex << (o.character & 0xFFFF) << std::dec
		   << " cell=(" << o.k7ArrivalGX << "," << o.k7ArrivalGY << ")"
		   << " waitMs=" << ((o.k7ArrivalWouldFireTime - o.k7ArrivalWaitSince) * 1000.0)
		   << " savedMs=" << ((now - o.k7ArrivalWouldFireTime) * 1000.0);
		LogMsg(ao.str());
	}
	// A deleted-form send clears an armed arrival wait too: it is neither an
	// arrival-form send (g_k7ArrivalSent counts only those) nor left open, so
	// it lands in r ("ended other than by an arrival-form send").
	if (o.k7ArrivalWaitSince > 0.0) g_k7ArrivalResumed++;
	o.k7ArrivalWaitSince = 0.0;
	o.k7ArrivalWouldFireTime = 0.0;

	// New episode. k7Seen29 stays set (a re-issued order that the engine
	// completes before the next poll, never seen as task 29, must still be
	// able to re-arm the form, up to the shared cap); the current action is
	// -1 here, so there is no asynchronous switch to wait for. The signature
	// times restart, so the next re-issue needs a signature newer than this
	// send; k7Dest29 keeps the last task-29 destination, which the new order
	// replaces when it is seen as task 29. The hold and its classifier
	// timestamps reset the same way -- a fresh episode has not swapped away
	// from anything yet.
	o.k7HaveLastSend = true;
	o.k7LastSendX = posX;
	o.k7LastSendZ = posZ;
	o.k7ReachedTime = 0.0;
	o.k7FailedTime = 0.0;
	o.k7DeletedSince = 0.0;
	o.k7Counted = false;
	o.k7PostDeathHold = false;
	o.k7SigOnset = 0.0;
	o.k7SwapSeenTime = 0.0;
	o.k7DestWaitSince = 0.0;
	return true;
}

// Fire an armed arrival wait the instant its destination cell is in the
// world, bypassing K7TryDeletedReissue's own STOPPED_HYSTERESIS wait
// (delRefuse=h). The wait armed at K7SampleSignatures' edge test already
// establishes the signature and the distance; this polls readiness, then
// re-checks (through K7ArrivalFireGate) that the character is still actually
// stopped, that its movement destination still matches the tracked order (the
// same +0xDC test K7TryDeletedReissue uses), that the zones are accessible,
// and every character-state gate and budget K7TryDeletedReissue also applies,
// before sending through the same ReissueOrder path. Returns true when a
// re-issue was sent (the caller skips the rest of this poll for the entry).
bool K7TryArrivalReissue(IslandOrder& o, uintptr_t zm, uintptr_t cm, float posX, float posZ,
                                int gx, int gy, double now)
{
	if (o.k7ArrivalWaitSince <= 0.0) return false;

	// k7ArrivalTrigger=false ("observe"): once the would-fire time is
	// latched, the real K7 send (if one ever happens) logs the waitMs/savedMs
	// line at K7TryDeletedReissue's own success point -- nothing more to
	// classify here, so this never re-issues the try-shared readiness read.
	// Still expires at the 15s cap like any other wait, so a latched observe
	// wait that no real send ever follows is bounded and counts x.
	if (!k7ArrivalTriggerEnabled && o.k7ArrivalWouldFireTime > 0.0)
	{
		if (now - o.k7ArrivalWaitSince >= K7_ARRIVAL_MAX_WAIT)
		{
			o.k7ArrivalWaitSince = 0.0;
			o.k7ArrivalWouldFireTime = 0.0;
			g_k7ArrivalExpired++;
		}
		return false;
	}

	int destCell[2] = { o.k7ArrivalGX, o.k7ArrivalGY };
	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	int cls = ClassifyZoneReadiness(sectionMgr, destCell, /*splitMap*/false);
	double waited = now - o.k7ArrivalWaitSince;

	K7ArrivalOutcome outcome = K7ArrivalPoll(cls, waited, K7_ARRIVAL_MAX_WAIT);
	if (outcome == K7_ARRIVAL_WAIT) return false;
	if (outcome == K7_ARRIVAL_EXPIRE)
	{
		o.k7ArrivalWaitSince = 0.0;
		o.k7ArrivalWouldFireTime = 0.0;
		g_k7ArrivalExpired++;
		return false;
	}

	// outcome == K7_ARRIVAL_FIRE: still stopped? HavokCharacter's own
	// arrival/path-failure state, the same reads K7SampleSignatures samples.
	uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
	bool stillStopped = false;
	if (hc)
	{
		int hcState = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
		int ps      = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));
		stillStopped = (hcState == 1 || ps == 3);
	}

	// Destination match (K7TryDeletedReissue's own d/n test, 1057-1075):
	// +0xDC now must match the tracked destination or have collapsed onto the
	// position, skipped while held (combat movement writes +0xDC while the
	// fight runs) -- an arrival wait never sends while held anyway
	// (K7ArrivalFireGate).
	bool destMatch = true;
	if (!o.k7PostDeathHold)
	{
		float lastX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		float lastZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		const float matchSq = K7_DEST_MATCH * K7_DEST_MATCH;
		destMatch = Dist2(lastX, lastZ, o.destX, o.destZ) <= matchSq
		         || Dist2(lastX, lastZ, posX, posZ) <= matchSq;
	}

	bool zonesOk = K7ZonesAccessible(zm, posX, posZ, o.destX, o.destZ);

	// Every character-state gate K7TryDeletedReissue applies before sending
	// (KO already dropped the whole entry in K7Observe before this runs).
	uintptr_t ch = o.character;
	bool charBlocked =
		   (*(unsigned char*)(KLIB_MEMBER(3, ch, Character__isBeingCarried, K7_OFF_CHAR_BEING_CARRIED)) != 0)
		|| (*(int*)(KLIB_MEMBER(3, ch, Character_inSomething, OFF_CHAR_IN_SOMETHING)) != 0)
		|| (*(unsigned char*)(KLIB_MEMBER(3, ch, Character__isLiterallyUnderMeleeAttackRightNowForSure, PT_OFF_CHAR_HIT)) != 0);
	uintptr_t ai = *(uintptr_t*)(KLIB_MEMBER(3, ch, Character_ai, PT_OFF_CHAR_AI));
	if (!ai) charBlocked = true;   // can't confirm "no enemies" without it
	else
	{
		if (*(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_numEnemies, PT_OFF_AI_NUM_ENEMIES)) != 0) charBlocked = true;
		if (*(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_threats_count, PT_OFF_AI_THREATS)) != 0) charBlocked = true;
	}
	uintptr_t stats = *(uintptr_t*)(KLIB_MEMBER(3, ch, Character_stats, OFF_CHAR_STATS));
	if (!stats || *(unsigned char*)(KLIB_MEMBER(3, stats, CharStats__holdPositionMode, OFF_STATS_HOLD)))
		charBlocked = true;

	bool cooldown = IslandRecentlyReissued(o.character, now);

	K7ArrivalAction action = K7ArrivalFireGate(k7ArrivalTriggerEnabled, o.k7PostDeathHold, stillStopped,
	                                           destMatch, zonesOk, charBlocked, cooldown,
	                                           o.reissueCount, MAX_REISSUES);

	if (action == K7_ARR_LATCH_OBSERVE)
	{
		// k7ArrivalTrigger=false: latched only once every refusal gate has
		// already passed (K7ArrivalFireGate checks them before returning
		// LATCH_OBSERVE), so savedMs never overstates a poll `true` would
		// have refused anyway. The real K7 send (if any) logs the
		// waitMs/savedMs line at K7TryDeletedReissue's own success point.
		if (o.k7ArrivalWouldFireTime <= 0.0) o.k7ArrivalWouldFireTime = now;
		return false;
	}

	if (action == K7_ARR_NONE || action == K7_ARR_REFUSE)
	{
		// Held (NONE) or refused by some other gate (REFUSE) must still
		// expire at the 15s cap -- a held entry is already bounded by
		// k7PostDeathHold's own 60s hold cap or a resumed task 29, but
		// expiring the wait itself here too means it never re-classifies (a
		// try-shared read) past 15s regardless of which gate is holding it,
		// and it counts x either way.
		if (waited >= K7_ARRIVAL_MAX_WAIT)
		{
			o.k7ArrivalWaitSince = 0.0;
			o.k7ArrivalWouldFireTime = 0.0;
			g_k7ArrivalExpired++;
		}
		return false;
	}

	// action == K7_ARR_SEND
	bool wasLive = o.k7DeletedSince <= 0.0;   // not yet at the "deleted" state at send time
	if (o.reissueCount == 0) { o.limitGX = gx; o.limitGY = gy; }   // budget-reset anchor

	std::ostringstream f;
	f << std::fixed << std::setprecision(0);
	f << "pos=(" << posX << "," << posZ << ") cell=(" << o.k7ArrivalGX << "," << o.k7ArrivalGY << ")"
	  << " waitMs=" << (waited * 1000.0) << " live=" << (wasLive ? 1 : 0);
	std::string fields = f.str();

	if (!ReissueOrder(o, now, "arrival", false, 0.0f, 0.0f, true, fields.c_str()))
		return false;
	IslandMarkReissued(o.character, now);
	g_k7ArrivalSent++;
	if (wasLive) g_k7ArrivalLiveSent++;

	// End the episode exactly as the deleted path does, so a stale signature
	// or an already-latched deletion cannot pay for a second send for the
	// same stop.
	o.k7HaveLastSend = true;
	o.k7LastSendX = posX;
	o.k7LastSendZ = posZ;
	o.k7ReachedTime = 0.0;
	o.k7FailedTime = 0.0;
	o.k7DeletedSince = 0.0;
	o.k7Counted = false;
	o.k7SigOnset = 0.0;
	o.k7SwapSeenTime = 0.0;
	o.k7DestWaitSince = 0.0;
	o.k7ArrivalWaitSince = 0.0;
	o.k7ArrivalWouldFireTime = 0.0;
	return true;
}

// Same PlayerInterface::selectedCharacters walk as K7DropSelected below, but
// detaches from any formation group instead of touching the order tracker.
// Runs unconditionally from the stop and add==0 job detours (not behind
// K7FormOn): a formation member the player stops or re-tasks off its move
// order must never receive the group's old destination, whether or not the
// deleted-order re-issue form is enabled. A pointer compare and a store per
// selected character; no locks, allocation or logging.
void IslandDetachSelectedFromFormation(uintptr_t pi)
{
	if (!pi) return;
	uintptr_t count = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_count, OFF_PI_SEL_COUNT));
	if (count == 0) return;
	uintptr_t arrayPtr = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_buckets, OFF_PI_SEL_ARRAY));
	uintptr_t index    = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_bucketCount, OFF_PI_SEL_INDEX));
	if (!arrayPtr || index >= 1024) return;

	uintptr_t* node = *(uintptr_t**)(arrayPtr + 8 * index);
	void* sentinel = *(void**)((uintptr_t)GameAddr(RVA_HANDLE_SENTINEL));
	int maxIter = (int)count + 16;
	int iter = 0;
	while (node)
	{
		if (++iter > maxIter) break;
		int nodeType = *(int*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_handle_type, OFF_SEL_NODE_TYPE));
		if (nodeType == 1)
		{
			void* resolved = KlibSelectedCharacter((const void*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_value_base_, OFF_SEL_NODE_HANDLE)));
			uintptr_t character = (uintptr_t)resolved;
			if (character && resolved != sentinel)
			{
				FormationDetachCharacters(&character, 1);
				// Runs for the stop key and a clearing job order whether or not
				// K7 is on, so the order-outcome cancel can't be silently
				// disabled by that key either.
				OrderOutcomeCancel(character, ElapsedSec());
			}
		}
		node = *(uintptr_t**)KLIB_MEMBER(3, node, HandSetNode_next_, 0);
	}
}

// Selected characters of `pi`, the same PlayerInterface::selectedCharacters
// walk as hook_addOrderSelected (handle type 1, OFF_PI_SEL_*): drops each
// one's IslandOrder (pointer comparison against the tracker's own entries).
// Returns the number dropped. No logging, no allocation.
int K7DropSelected(uintptr_t pi)
{
	if (!pi || g_orderCount == 0) return 0;
	uintptr_t count = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_count, OFF_PI_SEL_COUNT));
	if (count == 0) return 0;
	uintptr_t arrayPtr = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_buckets, OFF_PI_SEL_ARRAY));
	uintptr_t index    = *(uintptr_t*)(KLIB_MEMBER(3, pi, PlayerInterface_selected_bucketCount, OFF_PI_SEL_INDEX));
	if (!arrayPtr || index >= 1024) return 0;

	uintptr_t* node = *(uintptr_t**)(arrayPtr + 8 * index);
	void* sentinel = *(void**)((uintptr_t)GameAddr(RVA_HANDLE_SENTINEL));
	int maxIter = (int)count + 16;
	int iter = 0;
	int dropped = 0;
	while (node)
	{
		if (++iter > maxIter) break;
		int nodeType = *(int*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_handle_type, OFF_SEL_NODE_TYPE));
		if (nodeType == 1)
		{
			void* resolved = KlibSelectedCharacter((const void*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_value_base_, OFF_SEL_NODE_HANDLE)));
			uintptr_t character = (uintptr_t)resolved;
			if (character && resolved != sentinel)
			{
				IslandOrder* ord = FindOrderForCharacter(character);
				if (ord)
				{
					if (ord->k7ArrivalWaitSince > 0.0) g_k7ArrivalResumed++;
					ord->active = false; dropped++;
				}
			}
		}
		node = *(uintptr_t**)KLIB_MEMBER(3, node, HandSetNode_next_, 0);
	}
	return dropped;
}

void K7SnapshotTracked()
{
	g_k7NearSnapCount = 0;
	if (!K7FormOn() || g_orderCount == 0) return;
	uintptr_t* stuff;
	unsigned int count;
	if (!TrackerPlayerList(&stuff, &count)) return;
	for (int i = 0; i < g_orderCount && g_k7NearSnapCount < MAX_ISLAND_ORDERS; ++i)
	{
		if (!g_orders[i].active) continue;
		uintptr_t ch = g_orders[i].character;
		if (!TrackerListHas(stuff, count, ch)) continue;
		K7NearSnap& s = g_k7NearSnap[g_k7NearSnapCount];
		if (!K7ReadOrders(ch, &s.st)) continue;
		s.character = ch;
		g_k7NearSnapCount++;
	}
}
} // namespace islands_reissue_detail
using namespace islands_reissue_detail;
bool IslandK7Preempted(uintptr_t character)
{
	if (!character || !K7FormOn()) return false;
	int t = ReadCharOrderType(character);
	return t != ORDER_TYPE_MOVE && t != -1;
}

const char* IslandK7StuckForm(uintptr_t character)
{
	if (!K7FormOn()) return "off";
	IslandOrder* o = FindOrderForCharacter(character);   // pointer compare only
	if (!o) return "-";
	if (o->k7PostDeathHold) return "held";
	if (o->k7DeletedSince > 0.0) return "del";
	// Armed, waiting on the destination cell -- ordered below held/del so
	// order_outcome.cpp's stops= attribution for those two states is
	// unchanged by this key. Only reported while the key can actually send:
	// with it off, a build with the arrival mechanism reads the same as one
	// without it.
	if (o->k7ArrivalWaitSince > 0.0 && k7ArrivalTriggerEnabled) return "arr";
	if (o->k7Preempted) return "pre";
	return o->k7Seen29 ? "trk" : "new";
}

bool IslandK7IsUnconcious(uintptr_t character)
{
	if (!character) return false;
	return K7IsUnconcious(character);
}

// PLAYER STUCK's stops= class guess (order_outcome.cpp): whichever of the
// deleted-order form's two end signatures is newer, "-" when neither has
// latched. "reached" is a partial path end short of the destination (class
// 7's own signature); "failed" is a Havok path-state failure (class 2's).
const char* IslandK7StopSig(uintptr_t character)
{
	IslandOrder* o = FindOrderForCharacter(character);
	if (!o || (o->k7ReachedTime <= 0.0 && o->k7FailedTime <= 0.0)) return "-";
	return (o->k7ReachedTime > o->k7FailedTime) ? "reached" : "failed";
}

bool IslandReadHc136(uintptr_t character, int* outHc136)
{
	if (!character) return false;
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm) return false;
	uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
	if (!hc) return false;
	*outHc136 = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
	return true;
}

namespace islands_reissue_detail {
// A paused game (GameWorld::paused, kept behind the escape menu through a
// loader unpause -- zone_pause.cpp) must not advance any K7/tracker timer:
// nothing is simulating, so an elapsed-wall-clock read across the pause would
// count time that never actually passed for the character. IslandReissuePollTick
// skipping its whole body while paused (in islands_reissue.cpp) stops every one of
// these clocks from advancing during the pause itself; this corrects for a
// clock that had already started ticking on the frame just before the pause
// began -- on the frame the pause ends, every nonzero timestamp any K7/F1
// comparison reads is shifted forward by the paused duration together, so a
// later `now - <timestamp>` or `<timestamp A> - <timestamp B>` reads the same
// as if the pause had never happened. A partial shift is worse than none: an
// unshifted `k7DeletedSince` compared against a shifted `k7ReachedTime` (or
// the reverse) can permanently refuse the deleted-order re-issue for the rest
// of an episode (K7TryDeletedReissue's K7_SIG_WINDOW test) or flip
// K7ClassifySwap from HOLD to DROP -- so every field below moves together.
static inline void ShiftStamp(double& t, double d) { if (t > 0.0) t += d; }

void K7RebasePausedClocks(bool paused, double now)
{
	if (paused)
	{
		if (!g_k7WasPaused) { g_k7WasPaused = true; g_k7PauseStarted = now; }
		return;
	}
	if (!g_k7WasPaused) return;
	g_k7WasPaused = false;
	double pausedFor = now - g_k7PauseStarted;
	if (pausedFor <= 0.0) return;
	for (int i = 0; i < g_orderCount; ++i)
	{
		IslandOrder& o = g_orders[i];
		if (!o.active) continue;
		// Every field IslandOrder holds that any K7/F1 comparison reads
		// against another such field or against `now`. Not `orderTime` (never
		// compared) and never `IslandOrder`'s (a)-discriminator fields, which
		// have no game-time meaning. FormationGroup::createdTime is also left
		// alone: PollFormationGroups is not pause-gated, so it already runs
		// through a pause.
		ShiftStamp(o.parkTime, pausedFor);
		ShiftStamp(o.lastReissueTime, pausedFor);
		ShiftStamp(o.stoppedSince, pausedFor);
		ShiftStamp(o.k7ReachedTime, pausedFor);
		ShiftStamp(o.k7FailedTime, pausedFor);
		ShiftStamp(o.k7DeletedSince, pausedFor);
		ShiftStamp(o.k7Last29Time, pausedFor);
		ShiftStamp(o.k7SigOnset, pausedFor);
		ShiftStamp(o.k7SwapSeenTime, pausedFor);
		ShiftStamp(o.k7DestWaitSince, pausedFor);
		ShiftStamp(o.k7DestLastNotIn, pausedFor);
		ShiftStamp(o.k7ArrivalWaitSince, pausedFor);
		ShiftStamp(o.k7ArrivalWouldFireTime, pausedFor);
	}
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
		if (g_reissueChecks[i].active) ShiftStamp(g_reissueChecks[i].issueTime, pausedFor);
	FormationRebaseReissueClocks(pausedFor);
}

} // namespace islands_reissue_detail
using namespace islands_reissue_detail;
