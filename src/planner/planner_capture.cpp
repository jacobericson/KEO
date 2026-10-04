// planner_capture.cpp - The route planner's order capture: each captured player move order located,
// searched and written as one plan per ordered character, with its per-order line; the drop on a
// non-move order or the stop key; the ahead-enqueue query.
// Main thread only. The one game lock is the section manager's world lock (+0x200), taken
// try-shared (never blocking) by the locator and the snapshot and released on every path; no mod
// lock is taken.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_tick.h"
#include "planner/planner_tick_internal.h"
#include "planner/plan_build.h"
#include "planner/plan_policy.h"
#include "planner/plan_store.h"
#include "planner/coarse_graph.h"
#include "planner/planner_water.h"
#include "game/game.h"
#include "base/core.h"
#include <stdio.h>
#include <string.h>

namespace planner {

using namespace planner_tick_detail;

static const int      LINE_TILES_MAX  = 16;

// The order capture's own state and per-order scratch. Main thread.
static int            s_orderSeq      = 0;
static float          s_orderMult[PLAN_WATER_ORDER_MAX];   // the current order's water multipliers
static float          s_orderAcid[PLAN_WATER_ORDER_MAX];   // the current order's acid factors
static unsigned char  s_orderRepeat[PLAN_WATER_ORDER_MAX]; // 1 where the character's plan already answers it

// ---- The order capture ---------------------------------------------------------------------------

static const char* VerdictName(int v)
{
	return v == PV_DIRECT ? "direct" : (v == PV_LEGGED ? "legged" : "noRoute");
}

// The plan line's goal kind: an exterior goal, an interior goal the engine's building argument names,
// or an interior goal the plan holds at its building's portal (the engine routes the order outdoors).
static const char* GoalKind(int goalDir, bool indoors)
{
	if (PlanHoldInteriorPortal(indoors ? 0 : 1, goalDir, CG_EXTERIOR_SLOTS))
		return "held";
	return goalDir >= CG_EXTERIOR_SLOTS ? "int" : "ext";
}

// The distinct exterior cells of the legs, in route order, as gx.gy pairs.
static void FormatTiles(const PlanLeg* legs, int n, char* out, size_t size)
{
	out[0] = '\0';
	size_t len = 0;
	int shown = 0, lastX = -1, lastY = -1;
	for (int i = 0; i < n && shown < LINE_TILES_MAX; ++i)
	{
		if (legs[i].farSection >= CG_EXTERIOR_SLOTS || (legs[i].cellX == lastX && legs[i].cellY == lastY))
			continue;
		lastX = legs[i].cellX;
		lastY = legs[i].cellY;
		int w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%d.%d", shown ? "," : "", lastX, lastY);
		if (w < 0)
			break;
		len += (size_t)w;
		++shown;
	}
}

static void ReportOrderPlan(int order, int k, const Located& from, const Located& to, int verdict,
                            const Built& b, int span, bool indoors, uintptr_t cm)
{
	char tiles[160];
	FormatTiles(b.legs, b.found ? b.legCount : 0, tiles, sizeof(tiles));
	float road = *(const float*)(KLIB_MEMBER(3, cm, CharMovement_roadWeight, 0x100));
	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner plan: order=%d char=%d from=%x:%d to=%x:%d verdict=%s legs=%d tiles=%s span=%d"
	            " cost=%.0f expanded=%d ms=%.1f loc=%s indoors=%d goal=%s"
	            " road=%.3f m=%.2f water=%.0f%%"
	            " a=%.0f acid=%.0f%%",
	            order, k, (unsigned)from.uid, from.node, (unsigned)to.uid, to.node, VerdictName(verdict),
	            b.found ? b.legCount : 0, tiles, span, b.cost, b.expanded, b.ms,
	            to.exact ? "exact" : "footprint", indoors ? 1 : 0, GoalKind(to.dir, indoors), road, b.waterMult, b.waterShare * 100.0f,
	            b.acidMult, b.acidShare * 100.0f);
	PlannerReportPlan(line);
	if (verdict == PV_NO_ROUTE || indoors)
		PlannerReportBorders(order, to.dir);
}

// Whether the character's current plan already answers this order: inside its first second and
// toward the same point. Main thread, the store's one writer; reads the slot without a lock.
static bool IsRepeatOrder(uintptr_t cm, const float dest[3], double now)
{
	PlanView v;
	int slot = PlanStoreFind(cm);
	if (slot < 0 || !PlanStoreRead(slot, &v) || v.cm != cm)
		return false;
	return PlanRepeatDue(v.finalDest, dest, now - PlanStoreMain(slot)->planTime);
}

// Whether character k's plan already answered the current order when it arrived (MarkRepeats).
static bool WasRepeat(int k, uintptr_t cm, const float dest[3], double now)
{
	return k < PLAN_WATER_ORDER_MAX ? s_orderRepeat[k] != 0 : IsRepeatOrder(cm, dest, now);
}

// Drops the plans of the order's characters that were not repeats.
static void DropOrderPlans(const uintptr_t* chars, int n, const float dest[3], double now, volatile LONG* counter)
{
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm || WasRepeat(k, cm, dest, now))
			continue;
		if (counter)
			InterlockedIncrement(counter);
		DropPlan(cm, PDW_UNLOCATED);
	}
}

// Marks each character of the order whose plan already answers it, counting each as a repeat, before
// the goal is located; returns how many characters are left to plan.
static int MarkRepeats(const uintptr_t* chars, int n, const float dest[3], double now)
{
	int fresh = 0;
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		bool repeat = cm && IsRepeatOrder(cm, dest, now);
		if (k < PLAN_WATER_ORDER_MAX)
			s_orderRepeat[k] = repeat ? 1 : 0;
		if (!cm)
			continue;
		if (repeat)
			InterlockedIncrement(&PlannerCountersGet()->repeats);
		else
			++fresh;
	}
	return fresh;
}

// The engine's move branch applies the destination at once whatever the order's two flags carry
// (a plain click sends the add flag set; the flags matter only to the engine's other orders), so
// every captured move order is planned.
int PlannerNoteOrder(const uintptr_t* chars, int n, const float* location, void* destIndoors, bool shift, bool addDontClear,
                     bool formationForms)
{
	if (PlanStoreMode() == PLANNER_OFF) return 0;
	(void)shift;
	(void)addDontClear;
	if (!location)
	{
		InterlockedIncrement(&PlannerCountersGet()->noLocation);
		return 0;
	}
	if (n <= 0)
		return 0;
	if (PlayersOverCap())
	{
		// The tick validates no plan while over the cap, so an older plan never outlives a new order.
		for (int k = 0; k < n; ++k)
		{
			uintptr_t cm = MovementOf(chars[k]);
			if (cm)
				DropPlan(cm, PDW_ORDER);
		}
		return 0;
	}
	int order = ++s_orderSeq;
	float dest[3] = { location[0], location[1], location[2] };
	double now = ElapsedSec();
	if (MarkRepeats(chars, n, dest, now) == 0)
		return 0;
	TakeSnapshot();
	Located goal;
	if (!Locate(dest, &goal))
	{
		DropOrderPlans(chars, n, dest, now, &PlannerCountersGet()->goalUnlocated);
		return 0;
	}
	ClearMemo();
	PlannerOrderWater(chars, n, s_orderMult, s_orderAcid);
	PlanSearchParams orderPrices = { s_orderMult[0], s_orderAcid[0] };
	int lead = MergeOrder(chars, n, goal, dest, orderPrices, s_orderMult, s_orderAcid, destIndoors == NULL ? 1 : 0,
	                      formationForms, order, now);
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm)
			continue;
		PlanSearchParams p;
		p.m = k < PLAN_WATER_ORDER_MAX ? s_orderMult[k] : 1.0f;
		p.a = k < PLAN_WATER_ORDER_MAX ? s_orderAcid[k] : 1.0f;
		if (WasRepeat(k, cm, dest, now))
			continue;
		float pos[3];
		CharPos(chars[k], pos);
		Located start;
		if (!Locate(pos, &start))
		{
			InterlockedIncrement(&PlannerCountersGet()->startUnlocated);
			DropPlan(cm, PDW_UNLOCATED);
			continue;
		}
		const Built* b = SearchAndBuild(start, goal, dest, p);
		int verdict = PV_NONE;
		if (WritePlan(cm, pos, goal, dest, *b, now, &verdict, 0, p, destIndoors == NULL ? 1 : 0) < 0)
			continue;
		int sx, sy, gx, gy;
		PlanCellOf(pos[0], pos[2], &sx, &sy);
		PlanCellOf(dest[0], dest[2], &gx, &gy);
		ReportOrderPlan(order, k, start, goal, verdict, *b, PlanCellSpan(sx, sy, gx, gy), destIndoors != NULL, cm);
	}
	return lead;
}

void PlannerDrop(uintptr_t character)
{
	if (PlanStoreMode() == PLANNER_OFF) return;
	DropPlan(MovementOf(character), PDW_ORDER);
}

bool PlannerRouteReplacesAhead(uintptr_t character)
{
	if (PlanStoreMode() == PLANNER_OFF) return false;
	uintptr_t cm = MovementOf(character);
	int slot = PlanStoreFind(cm);
	PlanView v;
	if (slot < 0 || !PlanStoreRead(slot, &v) || v.cm != cm)
		return false;
	return PlanReplacesAhead(PlanStoreMode(), v.verdict);
}

} // namespace planner
