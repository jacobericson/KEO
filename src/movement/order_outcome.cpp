// order_outcome.cpp -- game-facing glue for the player order-outcome record
// table. All the bookkeeping and classification lives in
// order_outcome_table.cpp (pure, host-tested); this file only translates
// Character* and game state into calls on that API, and binds its sink to
// LogMsg. Main thread only.

#include "movement/order_outcome.h"
#include "movement/order_outcome_table.h"
#include "movement/order_outcome_policy.h"
#include "zone/grid.h"
#include "movement/island_span_policy.h"
#include "zone/zone_pause.h"
#include "planner/plan_store.h"
#include "movement/movement_trace.h"
#include "movement/formation_follow.h"

namespace order_outcome_detail {

void Sink(const std::string& line)
{
	LogMsg(line);
}

} // namespace
using namespace order_outcome_detail;

void OrderOutcomeReset()
{
	MovementTraceReset();
	OOT_Reset(Sink);
	OOT_SetPlannerColumn(planner::PlanStoreMode() != planner::PLANNER_OFF);
}

void OrderOutcomeBegin(const uintptr_t* chars, int count, float destX, float destZ, double now)
{
	if (count <= 0 || !chars) return;
	OOT_SetCloseNote(MovementTraceOnOrderClose);

	int cellSpan = -1;
	int gx, gy, dgx, dgy;
	if (WorldToZoneGrid(GetCharPosX(chars[0]), GetCharPosZ(chars[0]), &gx, &gy)
	    && WorldToZoneGrid(destX, destZ, &dgx, &dgy))
		cellSpan = IslandCellSpan(gx, gy, dgx, dgy);

	// size_t and uintptr_t are the same width everywhere this mod builds
	// (x64); the table is pure and knows characters only as opaque handles.
	const float from[2] = { GetCharPosX(chars[0]), GetCharPosZ(chars[0]) };
	const float to[2] = { destX, destZ };
	OOT_Begin(reinterpret_cast<const size_t*>(chars), count, cellSpan, now, ZonePauseIsPaused(), from, to);
}

void OrderOutcomeNoteMotion(uintptr_t character, bool moving, bool post, double now)
{
	OOT_NoteMotion((size_t)character, moving, post, now, ZonePauseIsPaused());
	if (FormationFollowArmed())
	{
		// A follower's stop beside its leader is the follow task's, never a stall.
		uintptr_t fcm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
		if (fcm && FormationOwnsFollower(fcm, FFS_OUTCOME))
		{
			OOT_NotePlannerWait((size_t)character, now, ZonePauseIsPaused());
			return;
		}
	}
	if (planner::PlanStoreMode() == planner::PLANNER_OFF) return;
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm) return;
	float x = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
	float z = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	float wx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	float wz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	if (planner::PlannerOwnsWait(cm, x, z, wx, wz))
		OOT_NotePlannerWait((size_t)character, now, ZonePauseIsPaused());
}

void OrderOutcomeNoteKo(uintptr_t character, double now)
{
	OOT_NoteKo((size_t)character, now, ZonePauseIsPaused());
}

void OrderOutcomeNoteReissueSent(uintptr_t character, const char* form, double now)
{
	OOT_NoteReissueSent((size_t)character, form, now, ZonePauseIsPaused());
}

void OrderOutcomeCancel(uintptr_t character, double now)
{
	OOT_Cancel((size_t)character, now, ZonePauseIsPaused());
}

void OrderOutcomeOnGroupComplete(const uintptr_t* chars, int count, double now)
{
	if (count <= 0 || !chars) return;
	OOT_OnGroupComplete(reinterpret_cast<const size_t*>(chars), count, now, ZonePauseIsPaused());
}

void OrderOutcomePoll(double now)
{
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned int scCount = 0;
	uintptr_t* scStuff = NULL;
	bool haveList = false;
	if (playerIntf)
	{
		scCount = GetPlayerCharCount(playerIntf);
		scStuff = GetPlayerCharStuff(playerIntf);
		haveList = (scStuff != NULL && scCount > 0 && scCount <= 256);
	}
	OOT_Poll(reinterpret_cast<const size_t*>(scStuff), (int)scCount, haveList, now, ZonePauseIsPaused());
	MovementTraceFrame(now);
}

void OrderOutcomeNoteStopGuess(uintptr_t character, const char* guess, double now)
{
	OOT_NoteStopGuess((size_t)character, guess, now);
}

bool OrderOutcomeAppendSpanTotals(std::ostringstream& ss)
{
	static long lastOrders = -1, lastLongOrders = -1, lastLongStop = -1, lastLongFail = -1;
	static long lastUserRec = -1, lastUnrec = -1;
	OotTotals t = OOT_GetTotals();
	ss << OrderOutcomeFormatSpanTotals(t.orders, t.longOrders, t.longStop, t.longFail, t.userRec, t.unrec);
	bool changed = (t.orders != lastOrders || t.longOrders != lastLongOrders || t.longStop != lastLongStop
	                || t.longFail != lastLongFail || t.userRec != lastUserRec || t.unrec != lastUnrec);
	lastOrders = t.orders; lastLongOrders = t.longOrders; lastLongStop = t.longStop;
	lastLongFail = t.longFail; lastUserRec = t.userRec; lastUnrec = t.unrec;
	return changed;
}
