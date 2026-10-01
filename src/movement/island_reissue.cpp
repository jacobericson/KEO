// island_reissue.cpp - main-thread deferred reissue checks and send path.
// Nudges move at least 8 units, direction-aware; 2 s cooldown per actual send.
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
#include "planner/plan_store.h"
namespace island_reissue_detail {
// One summary-mode FormationReissueTravel call (group above 6 members).
struct ReissueDispatch {
	bool active;
	bool closed;    // IslandEndReissueDispatch ran: no more members will be recorded
	int  slot;      // formation group slot, for the summary line
	int  total;     // members recorded (dropped ones included)
	int  sent;      // members resolved with post=sent
	int  pending;   // members recorded and not yet resolved or dropped
};
}
using namespace island_reissue_detail;
namespace order_tracker_detail {
// -------------------------------------------------------------------------
// Deferred (a) discriminator (main thread only).
//
// fn_moveOrder (playerMoveOrderDefault, vtable+0x318) hands the order to the
// AI task system, which applies it asynchronously, so a same-tick read of
// +0xDC always saw the previous destination. Each re-issue records a pending
// check here; ResolveDueReissueChecks (from IslandTick, every frame) resolves
// it at the first tick at least REISSUE_RESULT_DELAY after the order.
//
// REISSUE_RESULT_DELAY is below REISSUE_COOLDOWN, and both tracker paths
// (ReissueOrder's own-entry cooldown, FormationReissueTravel's
// IslandRecentlyReissued skip) enforce the cooldown per character, so the
// tracker cannot send the same character a newer order before its check
// resolves. IslandRecordReissueCheck still handles that case (a player click
// resets IslandOrder.lastReissueTime via IslandNoteOrder).
// -------------------------------------------------------------------------

const double REISSUE_RESULT_DELAY   = 1.0;   // must stay < REISSUE_COOLDOWN (2.0)
const int    MAX_REISSUE_DISPATCHES = 16;    // 2x MAX_FORMATION_GROUPS (group cooldown 2 s > delay 1 s)


volatile long g_reissues = 0;
// One pending check per character at most (a newer order resolves the old
// one). Checks come from solo IslandOrders (MAX_ISLAND_ORDERS = 64) AND from
// every member of a group dispatch (MAX_FORMATION_GROUPS 8 x
// MAX_FORMATION_MEMBERS 30), which are not IslandOrders. The worst case is
// therefore 64 + 240 = 304 distinct characters with a check pending inside
// the same 1 s window, which 256 does NOT cover. The table is left at 256
// (squads that large parking together are not expected): past 256, a new
// check resolves the oldest one early with its current state instead of
// dropping a line, counted as reissueCheckEarly= and visible as a short dt=.
ReissueCheck    g_reissueChecks[MAX_REISSUE_CHECKS];
// Islands: summary-line counters (cumulative for the session, main thread).
long g_reissuePostSent     = 0;
long g_reissuePostLast     = 0;
long g_reissuePostOther    = 0;
long g_reissueCheckDropped = 0;
long g_reissueCheckEarly   = 0;   // resolved before the delay because the table was full
static int             g_reissueCheckActive = 0;
static ReissueDispatch g_reissueDispatches[MAX_REISSUE_DISPATCHES];

// Prints the summary line and frees the dispatch once it is closed and every
// member has resolved or been dropped. A dispatch that recorded nobody
// (FormationReissueTravel sent no member) is freed without a line, as before.
static void FinishDispatchIfDone(int d)
{
	if (d < 0 || d >= MAX_REISSUE_DISPATCHES) return;
	ReissueDispatch& rd = g_reissueDispatches[d];
	if (!rd.active || !rd.closed || rd.pending > 0) return;
	if (rd.total > 0)
	{
		std::ostringstream ss;
		ss << "Island reissue result: group " << rd.slot
		   << " summary " << rd.sent << "/" << rd.total << " sent";
		LogMsg(ss.str());
	}
	rd.active = false;
}

// Resolves one pending check with the character's CURRENT state and frees its
// slot. `live` is the caller's TrackerIsLivePlayerCharacter answer: when it
// is false the stored pointer is never dereferenced -- the entry is dropped
// silently and counted (reissueCheckDropped=), and a summary-mode member
// still counts toward its dispatch's total, not its sent.
static void ResolveReissueCheck(ReissueCheck& c, double now, bool live)
{
	int d = c.dispatch;
	bool inDispatch = (d >= 0 && d < MAX_REISSUE_DISPATCHES && g_reissueDispatches[d].active);

	if (!live)
	{
		g_reissueCheckDropped++;
	}
	else
	{
		uintptr_t character = c.character;
		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
		float wpX = 0.0f, wpZ = 0.0f;
		int edge = 0, ctr = 0, ps = 0, hc136 = 0;
		bool haveMoved = false;
		float moved = 0.0f;
		if (cm)
		{
			wpX   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
			wpZ   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
			edge  = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE));
			ctr   = *(int*)(KLIB_MEMBER(3, cm, CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER));
			uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
			if (hc)
			{
				ps    = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));
				hc136 = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
			}
			// moved=: straight-line distance between the CharMovement position
			// (+0xC4, OFF_CMOV_POS -- the vector CharMovement::stop() 0x65F1E0
			// copies into +0xDC/+0xE8) captured before fn_moveOrder and now.
			if (c.pre.valid)
			{
				float mx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS))     - c.pre.posX;
				float mz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8)) - c.pre.posZ;
				moved = sqrtf(mx * mx + mz * mz);
				haveMoved = true;
			}
		}

		// The existing rule, unchanged (single owner of post=).
		IslandReissuePost cls = IslandClassifyReissuePost(character, c.sentX, c.sentZ, c.pre);
		if (cls == ISLAND_POST_SENT)      g_reissuePostSent++;
		else if (cls == ISLAND_POST_LAST) g_reissuePostLast++;
		else                              g_reissuePostOther++;
		if (inDispatch && cls == ISLAND_POST_SENT)
			g_reissueDispatches[d].sent++;

		// Summary mode keeps its behaviour: per-member lines only for results
		// that are not post=sent.
		if (!inDispatch || cls != ISLAND_POST_SENT)
		{
			const char* post = (cls == ISLAND_POST_SENT) ? "sent" : (cls == ISLAND_POST_LAST) ? "last" : "other";
			float ddx = c.pre.lastX - c.sentX, ddz = c.pre.lastZ - c.sentZ;
			float dist = sqrtf(ddx * ddx + ddz * ddz);
			double dtSec = now - c.issueTime;
			int dtMs = (int)(dtSec * 1000.0 + 0.5);

			std::ostringstream ss;
			ss << std::fixed << std::setprecision(1);
			ss << "Island reissue result: " << c.label
			   << " post=" << post << " d=" << dist
			   << " order=" << c.pre.orderType
			   << " edge=" << c.pre.edge << "/" << c.pre.edgeCtr << "->" << edge << "/" << ctr
			   << " wp=(" << c.pre.wpX << "," << c.pre.wpZ << ")->(" << wpX << "," << wpZ << ")"
			   << " ps=" << c.pre.hcPathState << "->" << ps
			   << " hc136=" << c.pre.hcArrival << "->" << hc136
			   << " dt=" << dtMs
			   << " moved=";
			if (haveMoved) ss << moved;
			else           ss << "-";
			if (c.overtaken & ISLAND_OVERTAKEN_CLICK) ss << " click=1";
			LogMsg(ss.str());
		}
	}

	c.active = false;
	c.character = 0;
	g_reissueCheckActive--;
	if (inDispatch)
	{
		g_reissueDispatches[d].pending--;
		FinishDispatchIfDone(d);
	}
}

// IslandTick, every frame (main thread): resolve every check whose delay has
// elapsed. One player-list read per call; each check is tested against it
// before its pointer is used.
void ResolveDueReissueChecks(double now)
{
	if (g_reissueCheckActive <= 0) return;
	uintptr_t* stuff = NULL;
	unsigned int count = 0;
	bool haveList = TrackerPlayerList(&stuff, &count);
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
	{
		ReissueCheck& c = g_reissueChecks[i];
		if (!c.active) continue;
		if (now - c.issueTime < REISSUE_RESULT_DELAY) continue;
		bool live = haveList && TrackerListHas(stuff, count, c.character);
		ResolveReissueCheck(c, now, live);
	}
}

bool ReissueCharacter(uintptr_t character, float dx, float dy, float dz, double now)
{
	uintptr_t charVtable = *(uintptr_t*)character;
	if (!charVtable) return false;
	typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
	moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
	if (!fn_moveOrder) return false;

	float dest[3] = { dx, dy, dz };
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (cm)
	{
		// (b) Direction-aware nudge: CharMovement::setDestination_Vec3 drops a
		// re-issue within 2 units of the last requested destination (+0xDC)
		// while routing to an island edge. Push away from +0xDC by >= 8 units
		// instead of the old fixed, one-sided +3 on x.
		float lx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		float lz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		IslandNudgeAwayFromLastDest(lx, lz, &dest[0], &dest[2]);
		// The route planner keeps a character's plan across this re-send of its order.
		planner::PlannerNoteModSend(cm, dest, planner::PLAN_SEND_RESEND, now);
	}

	// (a) Discriminator line: capture state immediately before fn_moveOrder,
	// call it, and record a pending check that IslandTick resolves
	// REISSUE_RESULT_DELAY later (the order is applied asynchronously, so a
	// same-tick read cannot tell whether it took effect). character's low 16
	// bits give distinct log tags without a meaningful per-character index
	// available at this call site.
	IslandReissueTrace trace;
	IslandCaptureReissueTrace(character, &trace);
	KlibDispatchMoveOrder(fn_moveOrder, character, NULL, NULL, dest);
	{
		std::ostringstream label;
		label << "char@" << std::hex << (character & 0xFFFF) << std::dec;
		std::string labelStr = label.str();
		IslandRecordReissueCheck(character, labelStr.c_str(), dest[0], dest[2], trace, now, -1);
	}
	return true;
}

// k7Fields (K7's deleted-order form only; NULL for every other
// form, whose line is unchanged): printed in place of park=(), which the
// deleted form never sets, and the line gains exhausted=1 when this send
// used the last of the MAX_REISSUES budget.
bool ReissueOrder(IslandOrder& o, double now, const char* why, bool haveCross, float cx, float cz,
                  bool forceCharacterOnly, const char* k7Fields)
{
	if (o.reissueCount >= MAX_REISSUES) return false;
	if (o.lastReissueTime > 0.0 && now - o.lastReissueTime < REISSUE_COOLDOWN) return false;

	bool sent;
	// (c): a member being re-issued alone (its group's representative is not
	// parked) always goes through the character path, even though it belongs
	// to an active formation group.
	int slot = forceCharacterOnly ? -1 : FormationSlotForCharacter(o.character);
	// A representative's own tracked order can outlive its group
	// membership (a later individual move). Only blast the group's
	// destination to every member when the order this park test is acting on
	// still IS that destination; otherwise treat the representative as a
	// lone character, using its own newer order.
	if (slot >= 0 && !FormationGroupDestNear(slot, o.destX, o.destZ, 2500.0f))
		slot = -1;
	if (slot >= 0)
		sent = FormationReissueTravel(slot, why, now);
	else
		sent = ReissueCharacter(o.character, o.destX, o.destY, o.destZ, now);
	if (!sent) return false;

	o.reissueCount++;
	o.lastReissueTime = now;
	o.retryArmed = haveCross;
	o.retryX = cx; o.retryZ = cz;
	InterlockedIncrement(&g_reissues);
	// Every re-issue reason (park/growth/retry/deleted) shares this one
	// success point, so a stall in progress for o.character can resolve as a
	// K7-family recovery whichever form actually sent it. Tagged with the
	// same short form IslandK7StuckForm uses, so a fast-recovery credit
	// (order_outcome_table.cpp's send lookback) can tell the reader which
	// form actually sent it.
	const char* k7Form = why;
	if (strcmp(why, "deleted") == 0)      k7Form = "del";
	else if (strcmp(why, "arrival") == 0) k7Form = "arr";
	OrderOutcomeNoteReissueSent(o.character, k7Form, now);

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(0);
	ss << "Island reissue (" << why << ((o.parkedViaStop && !k7Fields) ? " stopped" : "") << "): ";
	// Name the character the same way the (a) result lines do (char@<hex>)
	// instead of the old hardcoded "char 0", so this line and the matching
	// "Island reissue result" line correlate by label.
	if (slot >= 0)
		ss << "group " << slot;
	else
		ss << "char@" << std::hex << (o.character & 0xFFFF) << std::dec;
	if (k7Fields)
		ss << " " << k7Fields;
	else
		ss << " park=(" << o.parkX << "," << o.parkZ << ")";
	if (haveCross) ss << " cross=(" << cx << "," << cz << ")";
	ss << " dest=(" << o.destX << "," << o.destZ << ")"
	   << " n=" << o.reissueCount << "/" << MAX_REISSUES;
	if (k7Fields && o.reissueCount >= MAX_REISSUES)
		ss << " exhausted=1";
	LogMsg(ss.str());
	return true;
}

// =========================================================================
// (a) Discriminator trace: shared by ReissueCharacter (this file) and
// FormationReissueTravel (formation_query.cpp). Main thread only.
// =========================================================================

// Current order type from Character::playerMoveOrderDefault's (0x5D1820)
// cached-order chain (game.h). Every link is null-checked; -1 = no cached
// order (or any link is null), meaning the fresh-AddOrder path always runs.
int ReadCharOrderType(uintptr_t character)
{
	if (!character) return -1;
	uintptr_t p = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_body, OFF_CHAR_PENDING_TASK_PTR));
	if (!p) return -1;
	uintptr_t pendingTask = *(uintptr_t*)(KLIB_MEMBER(3, p, CharBody_currentAction, OFF_PENDING_TASK_HEAD_OFF));
	if (!pendingTask) return -1;
	uintptr_t orderObj = *(uintptr_t*)(KLIB_MEMBER(3, pendingTask, Tasker_taskData, OFF_PENDING_TASK_ORDER_OFF));
	if (!orderObj) return -1;
	return *(int*)(KLIB_MEMBER(3, orderObj, TaskData_key, OFF_ORDER_TYPE));
}
} // namespace order_tracker_detail
using namespace order_tracker_detail;

void IslandCaptureReissueTrace(uintptr_t character, IslandReissueTrace* out)
{
	if (!out) return;
	memset(out, 0, sizeof(*out));
	out->orderType = -1;
	if (!character) return;

	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (cm)
	{
		out->lastX   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		out->lastZ   = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		out->wpX     = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
		out->wpZ     = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
		out->posX    = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
		out->posZ    = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
		out->edge    = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE));
		out->edgeCtr = *(int*)(KLIB_MEMBER(3, cm, CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER));
		uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
		if (hc)
		{
			out->hcPathState = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));
			out->hcArrival   = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
		}
		out->valid = true;
	}
	out->orderType = ReadCharOrderType(character);
}

// The single owner of post=sent|last|other.
// post=sent (order went through), post=last (dropped -- d<=2 is the edge-mode
// 2-unit guard, d>2 is the in-place order-29 branch, item (6)), post=other
// (anything else, e.g. a mis-projected crossing).
IslandReissuePost IslandClassifyReissuePost(uintptr_t character, float sentX, float sentZ,
                                            const IslandReissueTrace& pre)
{
	if (!character) return ISLAND_POST_OTHER;
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm) return ISLAND_POST_OTHER;

	float postX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
	float postZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));

	float sdx = postX - sentX, sdz = postZ - sentZ;
	if (sdx * sdx + sdz * sdz <= 0.25f) return ISLAND_POST_SENT;   // within 0.5 units

	float ldx = postX - pre.lastX, ldz = postZ - pre.lastZ;
	if (ldx * ldx + ldz * ldz <= 0.25f) return ISLAND_POST_LAST;

	return ISLAND_POST_OTHER;
}

// Record a pending check (see islands.h). Main thread only.
void IslandRecordReissueCheck(uintptr_t character, const char* label,
                              float sentX, float sentZ,
                              const IslandReissueTrace& pre, double now, int dispatch)
{
	if (!character) return;

	// A newer order recorded for a character that still has a pending check:
	// resolve the old one first, with the current state. The new order was
	// just handed to the asynchronous AI task system, so what is read here is
	// still what the old order left (its dt= shows it resolved early). The
	// tracker's own cooldown (2 s > the 1 s delay) normally rules this out; a
	// player click in between resets IslandOrder.lastReissueTime, so it can
	// still happen.
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
	{
		ReissueCheck& c = g_reissueChecks[i];
		if (c.active && c.character == character)
			ResolveReissueCheck(c, now, TrackerIsLivePlayerCharacter(character));
	}

	int slot = -1;
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
	{
		if (!g_reissueChecks[i].active) { slot = i; break; }
	}
	if (slot < 0)
	{
		// Table full (256 checks pending inside one second). Resolve the
		// oldest early, with the current state, rather than lose this order's
		// line; dt= shows it and reissueCheckEarly= counts it.
		int oldest = 0;
		for (int i = 1; i < MAX_REISSUE_CHECKS; ++i)
			if (g_reissueChecks[i].issueTime < g_reissueChecks[oldest].issueTime) oldest = i;
		g_reissueCheckEarly++;
		ResolveReissueCheck(g_reissueChecks[oldest], now,
		                    TrackerIsLivePlayerCharacter(g_reissueChecks[oldest].character));
		slot = oldest;
	}

	ReissueCheck& c = g_reissueChecks[slot];
	c.active = true;
	c.character = character;
	int n = 0;
	if (label)
		for (; label[n] && n < REISSUE_LABEL_LEN - 1; ++n) c.label[n] = label[n];
	c.label[n] = '\0';
	if (n == 0) { c.label[0] = '?'; c.label[1] = '\0'; }
	c.sentX = sentX;
	c.sentZ = sentZ;
	c.pre = pre;
	c.issueTime = now;
	c.overtaken = 0;
	c.dispatch = -1;
	if (dispatch >= 0 && dispatch < MAX_REISSUE_DISPATCHES
	    && g_reissueDispatches[dispatch].active && !g_reissueDispatches[dispatch].closed)
	{
		c.dispatch = dispatch;
		g_reissueDispatches[dispatch].total++;
		g_reissueDispatches[dispatch].pending++;
	}
	g_reissueCheckActive++;
}

// Mark `character`'s pending check, if any, as overtaken by an order from
// outside the tracker. Pointer comparison only.
void IslandFlagReissueOvertaken(uintptr_t character, int reason)
{
	if (!character || g_reissueCheckActive <= 0) return;
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
	{
		ReissueCheck& c = g_reissueChecks[i];
		if (c.active && c.character == character)
		{
			c.overtaken |= reason;
			return;   // at most one pending check per character
		}
	}
}

int IslandBeginReissueDispatch(int slot, bool summaryMode)
{
	if (!summaryMode) return -1;
	for (int d = 0; d < MAX_REISSUE_DISPATCHES; ++d)
	{
		ReissueDispatch& rd = g_reissueDispatches[d];
		if (rd.active) continue;
		rd.active  = true;
		rd.closed  = false;
		rd.slot    = slot;
		rd.total   = 0;
		rd.sent    = 0;
		rd.pending = 0;
		return d;
	}
	return -1;   // full: every member of this dispatch prints its own line
}

void IslandEndReissueDispatch(int dispatch)
{
	if (dispatch < 0 || dispatch >= MAX_REISSUE_DISPATCHES) return;
	ReissueDispatch& rd = g_reissueDispatches[dispatch];
	if (!rd.active) return;
	rd.closed = true;
	FinishDispatchIfDone(dispatch);   // frees at once when nothing was recorded
}

namespace order_tracker_detail {
// Called from IslandReset (preload_saveload.cpp ClearPreloadStateImpl) and IslandTick's
// save-load / new-ZoneManager reset. Drops every pending check and dispatch
// without reading any stored pointer.
void ResetReissueChecks()
{
	for (int i = 0; i < MAX_REISSUE_CHECKS; ++i)
	{
		g_reissueChecks[i].active = false;
		g_reissueChecks[i].character = 0;
	}
	g_reissueCheckActive = 0;
	for (int d = 0; d < MAX_REISSUE_DISPATCHES; ++d)
		g_reissueDispatches[d].active = false;
}
} // namespace order_tracker_detail
using namespace order_tracker_detail;

// Has `character` been re-issued
// (solo or as a formation member) within REISSUE_COOLDOWN of `now`? Scans the
// same g_orders[] the tracker itself maintains -- one entry per character,
// written by IslandNoteOrder -- so this reflects every ReissueOrder call
// regardless of which path (character or group) made it.
bool IslandRecentlyReissued(uintptr_t character, double now)
{
	if (!character) return false;
	for (int i = 0; i < g_orderCount; ++i)
	{
		if (g_orders[i].active && g_orders[i].character == character)
		{
			return g_orders[i].lastReissueTime > 0.0
			    && now - g_orders[i].lastReissueTime < REISSUE_COOLDOWN;
		}
	}
	return false;
}

// Stamp ONLY lastReissueTime (never reissueCount, never parked) so a group
// dispatch's cooldown is visible to IslandRecentlyReissued/ReissueOrder for
// every member it touched, without charging that member's own per-order
// budget or marking it parked -- the (c) rescue must still be free to run its
// own full park-detection once the cooldown clears.
void IslandMarkReissued(uintptr_t character, double now)
{
	if (!character) return;
	for (int i = 0; i < g_orderCount; ++i)
	{
		if (g_orders[i].active && g_orders[i].character == character)
		{
			g_orders[i].lastReissueTime = now;
			return;
		}
	}
}
