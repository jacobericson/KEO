// island_orders.cpp - main-thread island order tracking and parked recovery.
// Poll one order at a time; K7 observation precedes formation skips.
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
// -------------------------------------------------------------------------
// The tracker's "still a live player character" test.
//
// Factored out of PollOrders, whose behaviour is unchanged: a character is
// live while it appears in PlayerInterface's playerCharacters lektor
// (RVA_GLOBAL_PLAYER -> count +0x2B8 / data +0x2C0, game.h), read with the
// same sanity bounds PollOrders always used (non-null data, 1..200 entries).
// A character that leaves the squad, or is freed by a save load, drops out of
// that list; PollOrders drops its IslandOrder on exactly this test ("squad
// removal"), and the pending checks in island_reissue.cpp use it before they touch a
// stored Character pointer.
// -------------------------------------------------------------------------

// false = no usable list (no PlayerInterface, null data, count 0 or > 200).
bool TrackerPlayerList(uintptr_t** outStuff, unsigned int* outCount)
{
	*outStuff = NULL;
	*outCount = 0;
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf) return false;
	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 200) return false;
	*outStuff = scStuff;
	*outCount = scCount;
	return true;
}

// Pointer comparison only: never dereferences `character`.
bool TrackerListHas(const uintptr_t* stuff, unsigned int count, uintptr_t character)
{
	for (unsigned int j = 0; j < count; ++j)
		if (stuff[j] == character) return true;
	return false;
}

bool TrackerIsLivePlayerCharacter(uintptr_t character)
{
	uintptr_t* stuff;
	unsigned int count;
	if (!TrackerPlayerList(&stuff, &count)) return false;
	return TrackerListHas(stuff, count, character);
}
} // namespace islands_reissue_detail
namespace islands_reissue_detail {



// (c) IsCharacterParkedNow (below) needs somewhere to
// hold the stopped-form hysteresis clock for a character it did not itself
// receive an IslandOrder& for (it is called with a representative's raw
// character pointer). The representative is a selected character of the
// same order, so it already has its own g_orders[] entry; look it up and
// share the exact same stoppedSince field PollOrders's own loop iteration
// for that entry would use. Returns NULL if the character has no active
// entry (never called from anywhere the caller's own entry can't apply,
// but callable defensively).
IslandOrder* FindOrderForCharacter(uintptr_t character)
{
	for (int i = 0; i < g_orderCount; ++i)
		if (g_orders[i].active && g_orders[i].character == character)
			return &g_orders[i];
	return NULL;
}

// Live park test for ANY character, used to
// check a formation representative's CURRENT CharMovement state directly
// rather than through its (possibly not-yet-updated-this-tick) IslandOrder
// flag. g_orders[k].parked is only as fresh as the last time that entry's
// own loop iteration ran; querying live state instead removes the
// dependency on which index runs first within one PollOrders() pass, so a
// representative and a member parking "together" can never see different
// answers to "is the representative parked" within the same tick.
bool IsCharacterParkedNow(uintptr_t character, float destX, float destZ, double now)
{
	if (!character) return false;
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm) return false;

	float posX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
	float posZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	if (Dist2(posX, posZ, destX, destZ) < PARK_MIN_DEST_DIST * PARK_MIN_DEST_DIST)
		return false;   // arrived, not parked

	float lastX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
	float lastZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
	float wpX   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	float wpZ   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	bool edge   = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE)) != 0;

	bool edgeParked = edge && Dist2(wpX, wpZ, posX, posZ) < PARK_WP_DIST * PARK_WP_DIST;

	// (b) The stopped form only fires while the character's cached order is
	// still the move order the tracker watches (ORDER_TYPE_MOVE) -- a later
	// non-move order already dropped the IslandOrder in hook_addOrderSelected
	// (item (a)), but a stale move order can also be replaced in place
	// (combat AI, a knockout, task completion) without going through that hook.
	bool stoppedPredicate = !edge
	                      && ReadCharOrderType(character) == ORDER_TYPE_MOVE
	                      && Dist2(lastX, lastZ, posX, posZ) < 100.0f    // |+0xDC - pos| < 10
	                      && Dist2(wpX, wpZ, posX, posZ) < 400.0f;       // |+0xE8 - pos| < 20

	// (c) Same 3s hysteresis as PollOrders's own stopped test, applied here so
	// the live representative check cannot park a character faster than that
	// character's own loop iteration would. If this character has no tracked
	// IslandOrder (should not happen for a representative -- see above -- but
	// checked defensively), the stopped form never parks it here.
	bool stoppedParked = false;
	IslandOrder* ord = FindOrderForCharacter(character);
	if (ord)
	{
		if (stoppedPredicate)
		{
			if (ord->stoppedSince <= 0.0) ord->stoppedSince = now;
			stoppedParked = (now - ord->stoppedSince) >= STOPPED_HYSTERESIS;
		}
		else
		{
			ord->stoppedSince = 0.0;
		}
	}
	return edgeParked || stoppedParked;
}
} // namespace islands_reissue_detail
namespace islands_reissue_detail {



struct PollOrdersCtx {
	uintptr_t zm;
	double now;
	bool k7On;
	uintptr_t* scStuff;
	unsigned int scCount;
	bool haveLastCell;
	int lastCellGX, lastCellGY, lastCellCls;
	uintptr_t cm;
	float posX, posZ, lastX, lastZ, wpX, wpZ;
	bool edge;
	bool k7Deleted, forceCharacterOnly;
	int gx, gy;
	bool edgeParked, stoppedParked, parkedNow;
	uintptr_t charZone;
};

// Main-thread order poll: validate the tracked character and read one movement snapshot.
static bool PollOrderRead(IslandOrder& o, PollOrdersCtx& c)
{
	uintptr_t* scStuff = c.scStuff;
	unsigned int scCount = c.scCount;
	if (!o.active) return true;

	// Squad removal: the tracker's live-player-character test.
	if (!TrackerListHas(scStuff, scCount, o.character))
	{
		if (o.k7ArrivalWaitSince > 0.0) g_k7ArrivalResumed++;
		o.active = false; return true;
	}

	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, o.character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm) return true;

	float posX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
	float posZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	float lastX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
	float lastZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
	float wpX  = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	float wpZ  = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	bool edge  = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE)) != 0;

	if (Dist2(posX, posZ, o.destX, o.destZ) < PARK_MIN_DEST_DIST * PARK_MIN_DEST_DIST)
	{
		if (o.k7ArrivalWaitSince > 0.0) g_k7ArrivalResumed++;
		o.active = false;   // arrived
		return true;
	}
	c.cm = cm;
	c.posX = posX; c.posZ = posZ;
	c.lastX = lastX; c.lastZ = lastZ;
	c.wpX = wpX; c.wpZ = wpZ;
	c.edge = edge;
	return false;
}

// Main-thread K7 sampling precedes formation evaluation for every far order.
static bool PollOrderK7(IslandOrder& o, PollOrdersCtx& c)
{
	bool k7On = c.k7On;
	float posX = c.posX, posZ = c.posZ;
	uintptr_t cm = c.cm;
	double now = c.now;
	bool& haveLastCell = c.haveLastCell;
	int& lastCellGX = c.lastCellGX;
	int& lastCellGY = c.lastCellGY;
	int& lastCellCls = c.lastCellCls;
	// Once per poll, for every far entry, remember the last time its
	// destination cell read not-in-world -- at most one try-shared +0x200
	// classify per *distinct* destination cell per poll (a squad sharing
	// one destination reuses the previous member's answer), bounded by
	// g_orderCount <= MAX_ISLAND_ORDERS, independent of whether the
	// arm/fire machinery is currently active for this entry.
	// K7SampleSignatures' arm test reads this so a signature that fires
	// just after the cell already streamed in can still recognise a
	// recent transition (the leg was requested just before the cell
	// arrived, walked, and stopped just after it did).
	if (k7On)
	{
		float dDestSqNow = Dist2(posX, posZ, o.destX, o.destZ);
		if (dDestSqNow > K7_ARRIVAL_MIN_DIST_SQ)
		{
			int dgx, dgy;
			if (WorldToZoneGrid(o.destX, o.destZ, &dgx, &dgy))
			{
				int clsNow;
				if (haveLastCell && dgx == lastCellGX && dgy == lastCellGY)
				{
					clsNow = lastCellCls;
				}
				else
				{
					int destCell[2] = { dgx, dgy };
					uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
					clsNow = ClassifyZoneReadiness(sectionMgr, destCell, /*splitMap*/false);
					haveLastCell = true;
					lastCellGX = dgx; lastCellGY = dgy; lastCellCls = clsNow;
				}
				if (clsNow == ZR_NOT_IN_WORLD) o.k7DestLastNotIn = now;
			}
		}
	}

	// K7: sample the order state for the deleted-order form and
	// apply its permanent drops (game-initiated clears, other tasks,
	// appended orders). Runs for every entry, before the (c) block can
	// skip a formation member, so every entry's state stays current.
	bool k7Deleted = false;
	if (k7On && K7Observe(o, cm, now, &k7Deleted))
	{
		if (o.k7ArrivalWaitSince > 0.0) g_k7ArrivalResumed++;
		o.active = false;
		return true;
	}
	// Another task runs with the move still queued. No form
	// re-issues while it lasts (an order sent now would be appended behind
	// the preempting task); the stopped form's hysteresis restarts after.
	if (k7On && o.k7Preempted)
	{
		o.stoppedSince = 0.0;
		return true;
	}
	c.k7Deleted = k7Deleted;
	return false;
}

// Main-thread formation phase: a skip ends this order, not the whole poll.
static bool PollOrderFormation(IslandOrder& o, PollOrdersCtx& c)
{
	bool k7On = c.k7On;
	bool& k7Deleted = c.k7Deleted;
	double now = c.now;
	// (c) A formation group is normally evaluated once, through its first
	// alive member. A member of a GATHERED group whose own park test holds
	// while the representative is not parked is evaluated -- and, if
	// needed, re-issued -- on its own, instead of being invisible until
	// the group's 120s timeout.
	int slot = FormationSlotForCharacter(o.character);
	bool forceCharacterOnly = false;
	// While the character's formation group is still gathering,
	// its own gather order is a mod move the cancel hooks never see (for
	// the leader, a move to where it already stands), so a deletion here
	// is the formation's, not the engine's: no deleted form, for the
	// representative too. The travel dispatch re-creates every member's
	// order after the gather.
	if (k7On && slot >= 0 && !formationGroups[slot].gathered)
	{
		k7Deleted = false;
		o.k7DeletedSince = 0.0;
		o.k7Counted = false;
		o.k7PostDeathHold = false;   // the gather is the formation's, not a held combat swap
		// The gather-arrival's own end signature must never leave an
		// arrival wait armed against a group that is walking normally --
		// K7SampleSignatures' own arm test already excludes a gathering
		// member, but a member that armed just before joining this group
		// (or whose gather starts inside an already-armed wait from a
		// previous episode) must be cleared here too.
		if (o.k7ArrivalWaitSince > 0.0)
		{
			o.k7ArrivalWaitSince = 0.0;
			o.k7ArrivalWouldFireTime = 0.0;
			g_k7ArrivalResumed++;
		}
	}
	if (slot >= 0)
	{
		uintptr_t rep = FormationFirstAliveMember(slot);
		if (rep && rep != o.character)
		{
			// Still gathering: this member is mid-approach to the leader,
			// not stranded en route. Leave it to the group logic.
			if (!formationGroups[slot].gathered) return true;

			// Read the representative's
			// CURRENT CharMovement state directly instead of its
			// IslandOrder.parked flag, which is only as fresh as the last
			// time THAT entry's own loop iteration ran. A scan-order
			// dependency there let a member (processed before the
			// representative's own entry, in a tick where both park
			// together) see a stale "not parked" answer, take the solo
			// path, and then get a second order moments later from the
			// representative's own group-wide re-issue in the SAME tick.
			// Every rep of this same order shares o.destX/Y/Z (the click
			// destination, per IslandNoteOrder), so the live test uses it
			// directly -- no g_orders[] lookup, no ordering dependency.
			if (IsCharacterParkedNow(rep, o.destX, o.destZ, now)) return true;   // the group-wide re-issue covers it

			// Group-then-solo cross-tick residual: a group dispatch stamps
			// THIS member's own cooldown (IslandMarkReissued,
			// formation.cpp) without ever touching o.parked, so a member
			// whose order was swallowed by the group blast reaches this
			// point with o.parked still false. Evaluating it now would run
			// the fresh park-entry branch below and call ReissueOrder
			// immediately -- a second order inside REISSUE_COOLDOWN.
			// Defer the whole solo evaluation (not just the send) until
			// the cooldown clears: leaving o.parked false here means the
			// park-entry branch runs uninterrupted once it does, so the
			// rescue still happens, just ~2s after the group's own order.
			if (IslandRecentlyReissued(o.character, now)) return true;
			forceCharacterOnly = true;
		}
	}
	c.forceCharacterOnly = forceCharacterOnly;
	return false;
}

// Main-thread park forms: arrival send precedes deleted send.
static bool PollOrderParkForms(IslandOrder& o, PollOrdersCtx& c)
{
	uintptr_t zm = c.zm, cm = c.cm;
	double now = c.now;
	bool k7On = c.k7On, k7Deleted = c.k7Deleted, edge = c.edge;
	float posX = c.posX, posZ = c.posZ;
	float lastX = c.lastX, lastZ = c.lastZ, wpX = c.wpX, wpZ = c.wpZ;
	int gx, gy;
	if (!WorldToZoneGrid(posX, posZ, &gx, &gy)) return true;

	// Re-issue budget resets once the squad has moved more than one zone.
	if (o.reissueCount > 0)
	{
		int ddx = gx - o.limitGX; if (ddx < 0) ddx = -ddx;
		int ddy = gy - o.limitGY; if (ddy < 0) ddy = -ddy;
		if (ddx > 1 || ddy > 1) { o.reissueCount = 0; o.limitGX = gx; o.limitGY = gy; }
	}

	// An armed arrival wait fires the instant its destination cell is in
	// the world (or was, within the recent-transition window), ahead of
	// the deletedParked form below -- it does not require k7Deleted (a
	// live task-29 continuation can stay armed too, subject to the same
	// still-stopped re-check every other fire needs) and bypasses
	// K7TryDeletedReissue's own STOPPED_HYSTERESIS wait.
	if (k7On && K7TryArrivalReissue(o, zm, cm, posX, posZ, gx, gy, now))
		return true;

	// K7: third park form, deletedParked. Evaluated solo (a
	// formation member the (c) block let through, or a representative,
	// gets a character-only re-issue). When it sends, this entry is done
	// for the tick; otherwise the park / stop / growth forms below run
	// exactly as before.
	if (k7Deleted && K7TryDeletedReissue(o, zm, cm, posX, posZ, gx, gy, now))
		return true;

	// (e) stop()-ed characters count as parked too: edge already 0,
	// getDestination() (+0xDC) and pathDestination (+0xE8) both collapsed
	// onto pos, and (checked above) more than 100 units from the order
	// destination with an active order entry.
	//
	// (b) The stopped form additionally requires the
	// character's cached order to still be the move order this entry is
	// tracking. A later non-move player order already drops the entire
	// IslandOrder in hook_addOrderSelected (item (a)); this catches an
	// in-place replacement of the same move order (combat AI, a knockout,
	// task completion) that never goes through that hook.
	//
	// (c) The stopped form must also hold continuously for
	// STOPPED_HYSTERESIS before it counts as parked, so one bad poll (a
	// still-settling order, a momentary state change) can't park a
	// character that is about to move again. o.stoppedSince resets to 0
	// the instant the predicate fails; IsCharacterParkedNow (above) shares
	// this same field for the live representative check.
	bool edgeParked = edge && Dist2(wpX, wpZ, posX, posZ) < PARK_WP_DIST * PARK_WP_DIST;
	bool stoppedPredicate = !edge
	                      && ReadCharOrderType(o.character) == ORDER_TYPE_MOVE
	                      && Dist2(lastX, lastZ, posX, posZ) < 100.0f   // |+0xDC - pos| < 10
	                      && Dist2(wpX, wpZ, posX, posZ) < 400.0f;      // |+0xE8 - pos| < 20
	if (stoppedPredicate) { if (o.stoppedSince <= 0.0) o.stoppedSince = now; }
	else                  { o.stoppedSince = 0.0; }
	bool stoppedParked = stoppedPredicate && (now - o.stoppedSince >= STOPPED_HYSTERESIS);
	bool parkedNow = edgeParked || stoppedParked;
	uintptr_t charZone = (uintptr_t)GetZoneEntry((void*)zm, gx, gy);
	if (!charZone) return true;
	c.gx = gx; c.gy = gy;
	c.edgeParked = edgeParked;
	c.stoppedParked = stoppedParked;
	c.parkedNow = parkedNow;
	c.charZone = charZone;
	return false;
}

// Main-thread fixed-ray crossing, growth and swallowed-order retry phase.
static bool PollOrderCrossing(IslandOrder& o, PollOrdersCtx& c)
{
	uintptr_t zm = c.zm, charZone = c.charZone;
	double now = c.now;
	float posX = c.posX, posZ = c.posZ;
	int gx = c.gx, gy = c.gy;
	bool edge = c.edge, edgeParked = c.edgeParked;
	bool stoppedParked = c.stoppedParked, parkedNow = c.parkedNow;
	bool forceCharacterOnly = c.forceCharacterOnly;
	if (!o.parked)
	{
		if (!parkedNow) return true;

		// --- park: record the fixed ray dest -> parkPos and emulate X0 ---
		o.parked = true;
		o.parkedViaStop = !edgeParked && stoppedParked;
		o.parkX = posX; o.parkZ = posZ;
		o.parkGX = gx; o.parkGY = gy;
		o.parkTime = now;
		o.haveX0 = false;
		o.retryArmed = false;
		o.noCrossingLogged = false;
		o.seenGen = IslandOverlayGen();
		o.seenSig = IslandOverlaySetBSig();
		if (o.reissueCount == 0) { o.limitGX = gx; o.limitGY = gy; }

		float rx, rz;
		if (IslandEmulateCrossing((void*)zm, (void*)charZone, o.destX, o.destZ, o.parkX, o.parkZ, &rx, &rz))
		{
			o.haveX0 = true;
			o.x0X = rx; o.x0Z = rz;
			// Missed advance: the island already reaches past the waypoint.
			if (Dist2(rx, rz, o.parkX, o.parkZ) > MISSED_ADVANCE_DIST * MISSED_ADVANCE_DIST)
				ReissueOrder(o, now, "park", true, rx, rz, forceCharacterOnly);
		}
		else
		{
			o.noCrossingLogged = true;
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(0);
			ss << "Island reissue skipped: no crossing"
			   << (o.parkedViaStop ? " stopped" : "")
			   << " park=(" << o.parkX << "," << o.parkZ << ") zone=(" << gx << "," << gy << ")"
			   << " dest=(" << o.destX << "," << o.destZ << ")";
			LogMsg(ss.str());
		}
		return true;
	}

	// --- parked ---
	if (!parkedNow)
	{
		bool stillHere = Dist2(posX, posZ, o.parkX, o.parkZ) < UNPARK_DIST * UNPARK_DIST;
		if (!stillHere)
		{
			o.parked = false;      // moving again
			o.retryArmed = false;
			return true;
		}
		if (!o.retryArmed)
		{
			// Edge flag dropped (a new route was accepted) or the router
			// advanced the waypoint: wait for the character to move. For a
			// stop()-ed park (e) `edge` is already 0, so this only un-parks
			// when the distance tests themselves cleared, i.e. it moved.
			if (!edge) o.parked = false;
			return true;
		}
		// A re-issue that the readiness gate swallowed clears movingToEdge
		// before returning, so the character sits here with edge=0 and an
		// unchanged position. Treat it as still parked so the retry fires.
	}

	bool configChanged = (o.seenGen != IslandOverlayGen()) || (o.seenSig != IslandOverlaySetBSig());
	bool retryDue = o.retryArmed && (now - o.lastReissueTime >= RETRY_DELAY);
	if (!configChanged && !retryDue) return true;
	o.seenGen = IslandOverlayGen();
	o.seenSig = IslandOverlaySetBSig();

	// Keep the ray fixed (dest -> parkPos): only the island can move the crossing.
	float rx, rz;
	bool have = IslandEmulateCrossing((void*)zm, (void*)charZone, o.destX, o.destZ, o.parkX, o.parkZ, &rx, &rz);

	if (configChanged)
	{
		if (o.haveX0)
		{
			if (have && Dist2(rx, rz, o.x0X, o.x0Z) > GROWTH_THRESHOLD_SQ)
			{
				if (ReissueOrder(o, now, "growth", true, rx, rz, forceCharacterOnly))
				{
					o.x0X = rx; o.x0Z = rz;
					return true;
				}
			}
		}
		else if (have)
		{
			// A crossing appeared on the fixed ray: island growth.
			if (Dist2(rx, rz, o.parkX, o.parkZ) > GROWTH_THRESHOLD_SQ)
				ReissueOrder(o, now, "growth", true, rx, rz, forceCharacterOnly);
			o.haveX0 = true;
			o.x0X = rx; o.x0Z = rz;
			return true;
		}
	}

	// Retry: still parked with the same crossing after a re-issue
	// (for example, the readiness gate swallowed the order).
	if (retryDue && have && Dist2(rx, rz, o.retryX, o.retryZ) <= GROWTH_THRESHOLD_SQ)
	{
		if (!ReissueOrder(o, now, "retry", true, rx, rz, forceCharacterOnly))
		{
			if (o.reissueCount >= MAX_REISSUES) o.retryArmed = false;
		}
	}
	else if (retryDue && !have)
		o.retryArmed = false;
	return false;
}

static void PollOneOrder(IslandOrder& o, PollOrdersCtx& c)
{
	if (PollOrderRead(o, c)) return;
	if (PollOrderK7(o, c)) return;
	if (PollOrderFormation(o, c)) return;
	if (PollOrderParkForms(o, c)) return;
	PollOrderCrossing(o, c);
}

void PollOrders(uintptr_t zm, double now)
{
	if (now - g_lastOrderPoll < ORDER_POLL_INTERVAL) return;
	g_lastOrderPoll = now;
	const bool k7On = K7FormOnLogged();
	if (g_orderCount == 0 || !gridCalibrated) return;

	uintptr_t* scStuff;
	unsigned int scCount;
	if (!TrackerPlayerList(&scStuff, &scCount)) return;

	// Memoise the per-poll destination-cell classify in PollOrderK7 by cell within
	// this one pass -- a squad sharing a destination cell would otherwise
	// repeat the same try-shared +0x200 scan once per member.
	PollOrdersCtx c;
	c.zm = zm;
	c.now = now;
	c.k7On = k7On;
	c.scStuff = scStuff;
	c.scCount = scCount;
	c.haveLastCell = false;
	c.lastCellGX = 0; c.lastCellGY = 0; c.lastCellCls = ZR_UNKNOWN;

	for (int i = 0; i < g_orderCount; ++i)
		PollOneOrder(g_orders[i], c);
}

} // namespace
using namespace islands_reissue_detail;

void IslandNoteOrder(uintptr_t character, const float* location)
{
	if (!character || !location) return;
	// A player move order landing inside a pending re-issue check's 1 s
	// window is flagged (click=1 on the result line), not resolved early.
	IslandFlagReissueOvertaken(character, ISLAND_OVERTAKEN_CLICK);
	int slot = -1;
	for (int i = 0; i < g_orderCount; ++i)
	{
		if (g_orders[i].character == character) { slot = i; break; }
		if (!g_orders[i].active && slot < 0) slot = i;
	}
	if (slot < 0)
	{
		if (g_orderCount >= MAX_ISLAND_ORDERS) return;
		slot = g_orderCount++;
	}
	IslandOrder& o = g_orders[slot];
	// A new order for this slot replaces whatever wait an old order still had
	// armed, silently (the memset below); count it so w != s + x + r + open
	// has an explanation rather than looking like a leak.
	if (o.active && o.k7ArrivalWaitSince > 0.0)
		g_k7ArrivalResumed++;
	memset(&o, 0, sizeof(o));
	o.active = true;
	o.character = character;
	o.destX = (*(const float*)KLIB_MEMBER(5, location, Ogre__Vector3_x, 0));
	o.destY = (*(const float*)KLIB_MEMBER(5, location, Ogre__Vector3_y, 4));
	o.destZ = (*(const float*)KLIB_MEMBER(5, location, Ogre__Vector3_z, 8));
	o.orderTime = ElapsedSec();
}

// (a) Called from hook_addOrderSelected
// for every selected character of a NON-move (task != 29) player order. An
// IslandOrder otherwise only clears on arrival, squad removal, a save load or
// a new move order -- so a later attack/job/pick-up/talk order, AI combat, or
// a knockout left the OLD move destination active, and the stopped-park test
// (e) would eventually re-issue that stale destination to a character the
// player deliberately redirected. Dropping the entry here removes it from the
// tracker outright (matches "clears on ... a new move order": a new NON-move
// order also ends the old move order's relevance).
void IslandDropOrder(uintptr_t character)
{
	if (!character) return;
	for (int i = 0; i < g_orderCount; ++i)
	{
		if (g_orders[i].active && g_orders[i].character == character)
		{
			if (g_orders[i].k7ArrivalWaitSince > 0.0)
				g_k7ArrivalResumed++;
			g_orders[i].active = false;
			break;
		}
	}
}
