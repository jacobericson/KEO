// The route planner's plan-building rules: the legs from a coarse route, the footprint pick, the
// drop predicate, the repeat rule and the memo key. Routes run east along one row of cells; each
// section change's portal sits on the border between two cells.

#include <cmath>
#include <cstdio>
#include <cstring>
#include "planner/plan_build.h"

#include "check.h"

using namespace planner;

static const float ROW_Z    = -7000.0f;   // cell 30 in z
static const float CELL     = 4608.0f;
static const float ORIGIN   = -147456.0f;

static void Set3(float out[3], float x, float y, float z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

static bool Same3(const float a[3], const float b[3])
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

// A route of `changes` section changes: two steps per section, the first step of each section
// crossing into the next. Section k is directory index 100 + k; its portal is on cell (k + 1)'s west
// border.
static int MakeRoute(PlanRouteStep* steps, int changes)
{
	int n = 0;
	for (int k = 0; k <= changes; ++k)
	{
		for (int j = 0; j < 2; ++j)
		{
			PlanRouteStep& s = steps[n++];
			memset(&s, 0, sizeof(s));
			s.dirIndex = 100 + k;
			s.node = j;
			s.crossesNext = (j == 1 && k < changes) ? 1 : 0;
			if (s.crossesNext)
			{
				float x = ORIGIN + CELL * (float)(k + 1);
				Set3(s.portal, x, 50.0f, ROW_Z);
				Set3(s.edgeA, x, 40.0f, ROW_Z - 40.0f);
				Set3(s.edgeB, x, 60.0f, ROW_Z + 40.0f);
			}
		}
	}
	return n;
}

static void CheckLegs()
{
	static PlanRouteStep steps[128];
	static PlanLeg legs[PLAN_MAX_LEGS + 1];
	float dest[3];
	Set3(dest, ORIGIN + CELL * 3.5f, 55.0f, ROW_Z);

	int truncated = -1;
	int n = MakeRoute(steps, 3);
	int got = PlanBuildLegs(steps, n, dest, 103, legs, &truncated);
	bool order = got == 4 && legs[0].farSection == 101 && legs[1].farSection == 102 && legs[2].farSection == 103
	          && !legs[0].isDestination && !legs[1].isDestination && !legs[2].isDestination;
	bool last = got == 4 && legs[3].isDestination == 1 && Same3(legs[3].point, dest) && legs[3].farSection == 103
	         && Same3(legs[3].edgeA, dest) && Same3(legs[3].edgeB, dest) && truncated == 0;
	Check(order && last, "legs: one leg per section change, the destination last");

	truncated = -1;
	n = MakeRoute(steps, 0);
	got = PlanBuildLegs(steps, n, dest, 100, legs, &truncated);
	Check(got == 1 && legs[0].isDestination == 1 && legs[0].farSection == 100 && truncated == 0,
	      "legs: a route in one section is the destination leg alone");

	truncated = -1;
	n = MakeRoute(steps, 31);
	got = PlanBuildLegs(steps, n, dest, 131, legs, &truncated);
	Check(got == PLAN_MAX_LEGS && legs[30].isDestination == 0 && legs[31].isDestination == 1 && truncated == 0,
	      "legs: 31 portals then the destination fill 32 slots");

	truncated = -1;
	n = MakeRoute(steps, 40);
	got = PlanBuildLegs(steps, n, dest, 140, legs, &truncated);
	Check(got == PLAN_MAX_LEGS && legs[PLAN_MAX_LEGS - 1].isDestination == 1
	      && Same3(legs[PLAN_MAX_LEGS - 1].point, dest) && legs[PLAN_MAX_LEGS - 2].isDestination == 0
	      && legs[PLAN_MAX_LEGS - 2].farSection == 131 && truncated == 1,
	      "legs: a truncated route keeps the destination leg and sets the flag");

	n = MakeRoute(steps, 3);
	got = PlanBuildLegs(steps, n, dest, 103, legs, &truncated);
	float pa[3], ea[3], eb[3];
	Set3(pa, ORIGIN + CELL * 2.0f, 50.0f, ROW_Z);
	Set3(ea, ORIGIN + CELL * 2.0f, 40.0f, ROW_Z - 40.0f);
	Set3(eb, ORIGIN + CELL * 2.0f, 60.0f, ROW_Z + 40.0f);
	Check(got == 4 && Same3(legs[1].point, pa) && Same3(legs[1].edgeA, ea) && Same3(legs[1].edgeB, eb)
	      && legs[1].cellX == 2 && legs[1].cellY == 30 && legs[3].cellX == 3 && legs[3].cellY == 30,
	      "legs: a portal leg carries its edge and cell");
}

static void MakeBox(PlanNodeBox* b, float x0, float y0, float z0, float x1, float y1, float z1)
{
	Set3(b->boxMin, x0, y0, z0);
	Set3(b->boxMax, x1, y1, z1);
	Set3(b->centre, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f);
}

static void CheckFootprint()
{
	PlanNodeBox boxes[2];
	float p[3];

	// Box 0 spans x 0..100, y 0..10, z 0..100; the point is 4 units east of it and 25 above.
	MakeBox(&boxes[0], 0.0f, 0.0f, 0.0f, 100.0f, 10.0f, 100.0f);
	MakeBox(&boxes[1], 1000.0f, 0.0f, 1000.0f, 1100.0f, 10.0f, 1100.0f);
	Set3(p, 104.0f, 35.0f, 50.0f);
	Check(PlanPickFootprint(boxes, 2, p, false) == 0, "footprint: a point inside a widened box picks it");

	// 150 units east of box 0's centre in x-z, outside every widened box.
	Set3(p, 200.0f, 5.0f, 50.0f);
	bool fallback = PlanPickFootprint(boxes, 2, p, true) == 0;
	Set3(p, 300.0f, 5.0f, 50.0f);
	bool beyond = PlanPickFootprint(boxes, 2, p, true) == -1;
	Check(fallback && beyond, "footprint: a point outside every widened box falls back within 200");

	Set3(p, 200.0f, 5.0f, 50.0f);
	Check(PlanPickFootprint(boxes, 2, p, false) == -1, "footprint: no fallback answers -1");

	// A floor (y 0..80, centre over the click in x-z) and a cliff top (y 90..110, centre 30 east):
	// the click at y 100 is in both widened boxes, and the top's centre is nearer in 3D.
	MakeBox(&boxes[0], -50.0f, 0.0f, -50.0f, 50.0f, 80.0f, 50.0f);
	MakeBox(&boxes[1], -20.0f, 90.0f, -50.0f, 80.0f, 110.0f, 50.0f);
	Set3(p, 0.0f, 100.0f, 0.0f);
	Check(PlanPickFootprint(boxes, 2, p, false) == 1, "footprint: the nearest centre in 3D wins");
}

static void CheckDrop()
{
	float planDest[3], moveDest[3], destAtPlan[3];
	Set3(planDest, 5000.0f, 40.0f, -7000.0f);
	Set3(moveDest, 5000.0f, 0.0f, -7000.0f);
	Set3(destAtPlan, 2000.0f, 0.0f, -7000.0f);   // the destination held at plan time, apart from every other point
	const bool halted = true;

	Check(PlanDropDue(false, false, 900.0f, moveDest, planDest, destAtPlan, false, !halted) == PDW_NOT_PLAYER, "drop: not a player drops");
	Check(PlanDropDue(true, true, 900.0f, moveDest, planDest, destAtPlan, false, !halted) == PDW_KO, "drop: unconscious drops");
	Check(PlanDropDue(true, false, 99.0f, moveDest, planDest, destAtPlan, false, !halted) == PDW_ARRIVED
	      && PlanDropDue(true, false, 101.0f, moveDest, planDest, destAtPlan, false, !halted) == PDW_NONE,
	      "drop: within 100 of the destination drops");

	float off[3];
	Set3(off, 5003.0f, 0.0f, -7000.0f);
	Check(PlanDropDue(true, false, 900.0f, off, planDest, destAtPlan, false, !halted) == PDW_NEW_DEST, "drop: a movement destination 3 units off drops");

	float zero[3];
	Set3(zero, 0.0f, 0.0f, 0.0f);
	Check(PlanDropDue(true, false, 900.0f, zero, planDest, destAtPlan, false, !halted) == PDW_NONE, "drop: a zero movement destination does not drop");

	Check(PlanDropDue(true, false, 900.0f, moveDest, planDest, destAtPlan, false, !halted) == PDW_NONE, "drop: a matching destination does not drop");

	// The destination before the order, the applied order, a halt and a new destination.
	float applied[3], pos[3];
	Set3(applied, 5001.0f, 0.0f, -7000.0f);
	Set3(pos, 3500.0f, 0.0f, -7000.0f);
	Check(PlanDropDue(true, false, 900.0f, destAtPlan, planDest, destAtPlan, false, !halted) == PDW_NONE,
	      "drop: the engine still holds the destination from before the order");
	Check(PlanDropDue(true, false, 900.0f, applied, planDest, destAtPlan, false, !halted) == PDW_NONE,
	      "drop: the engine applied the order");
	Check(PlanDropDue(true, false, 900.0f, pos, planDest, destAtPlan, false, halted) == PDW_NONE,
	      "drop: a halt onto the character's position keeps the plan");
	Check(PlanDropDue(false, false, 900.0f, pos, planDest, destAtPlan, false, halted) == PDW_NOT_PLAYER
	      && PlanDropDue(true, true, 900.0f, pos, planDest, destAtPlan, false, halted) == PDW_KO
	      && PlanDropDue(true, false, 99.0f, pos, planDest, destAtPlan, false, halted) == PDW_ARRIVED,
	      "drop: a halt keeps nothing past the not-player, unconscious and arrival tests");
	Check(PlanDropDue(true, false, 900.0f, pos, planDest, destAtPlan, false, !halted) == PDW_NEW_DEST,
	      "drop: a destination away from both points and not a halt is new");

	// The mod's own sends: a recorded re-send and the gather's hold.
	float rec[PLAN_RESEND_POINTS][3];
	memset(rec, 0, sizeof(rec));
	float hold[3], resent[3];
	Set3(hold, 0.0f, 0.0f, 0.0f);
	Set3(rec[0], 5008.0f, 40.0f, -7000.0f);
	Set3(resent, 5008.0f, 0.0f, -7000.0f);
	bool sent = PlanIsModSend(resent, rec, 1, hold, 0, 0.0);
	Check(sent && PlanDropDue(true, false, 900.0f, resent, planDest, destAtPlan, sent, !halted) == PDW_NONE,
	      "drop: a re-sent destination the mod recorded does not drop");
	bool unrecorded = PlanIsModSend(resent, rec, 0, hold, 0, 0.0);
	Check(!unrecorded && PlanDropDue(true, false, 900.0f, resent, planDest, destAtPlan, unrecorded, !halted) == PDW_NEW_DEST,
	      "drop: an unrecorded destination eight units off drops");
	Set3(rec[1], 4992.0f, 40.0f, -7000.0f);
	float earlier[3];
	Set3(earlier, 5008.5f, 0.0f, -7000.0f);
	bool kept = PlanIsModSend(earlier, rec, 2, hold, 0, 0.0);
	Check(kept && PlanDropDue(true, false, 900.0f, earlier, planDest, destAtPlan, kept, !halted) == PDW_NONE,
	      "drop: an earlier recorded re-send still matches");
	float away[3];
	Set3(away, 5900.0f, 0.0f, -7000.0f);
	bool awaySent = PlanIsModSend(away, rec, 1, hold, 0, 0.0);
	Check(!awaySent && PlanDropDue(true, false, 900.0f, away, planDest, destAtPlan, awaySent, !halted) == PDW_NEW_DEST,
	      "drop: a destination away from the recorded re-send and not a halt drops");
	float gather[3];
	Set3(hold, 4100.0f, 0.0f, -7300.0f);
	Set3(gather, 4100.5f, 0.0f, -7300.0f);
	bool young = PlanIsModSend(gather, rec, 0, hold, 1, 5.0);
	Check(young && PlanDropDue(true, false, 900.0f, gather, planDest, destAtPlan, young, !halted) == PDW_NONE,
	      "drop: the gather point keeps the plan while the hold is young");
	bool old = PlanIsModSend(gather, rec, 0, hold, 1, 21.0);
	Check(!old && PlanDropDue(true, false, 900.0f, gather, planDest, destAtPlan, old, !halted) == PDW_NEW_DEST,
	      "drop: the gather point drops the plan once the hold is older than its bound");

	// A gather's members walk to their own slots around the one recorded point.
	const float spread[4] = { 8.0f, 14.5f, 19.0f, 27.0f };
	for (int i = 0; i < 4; ++i)
	{
		float slot[3];
		Set3(slot, 4100.0f + spread[i] * 0.6f, 0.0f, -7300.0f + spread[i] * 0.8f);
		bool held = PlanIsModSend(slot, rec, 0, hold, 1, 5.0);
		char what[96];
		sprintf(what, "drop: a slot %.1f units from the gather point keeps the plan", spread[i]);
		Check(held && PlanDropDue(true, false, 900.0f, slot, planDest, destAtPlan, held, !halted) == PDW_NONE, what);
	}
	float beyond[3];
	Set3(beyond, 4135.0f, 0.0f, -7300.0f);
	bool notHold = PlanIsModSend(beyond, rec, 0, hold, 1, 5.0);
	Check(!notHold && PlanDropDue(true, false, 900.0f, beyond, planDest, destAtPlan, notHold, !halted) == PDW_NEW_DEST,
	      "drop: a destination 35 units from the gather point is not the hold");
	float slotOld[3];
	Set3(slotOld, 4108.0f, 0.0f, -7300.0f);
	Check(!PlanIsModSend(slotOld, rec, 0, hold, 1, 21.0),
	      "drop: a slot 8 units from the gather point drops once the hold is older than its bound");
	float resent8[3];
	Set3(rec[0], 5008.0f, 40.0f, -7000.0f);
	Set3(resent8, 5016.0f, 0.0f, -7000.0f);
	Check(!PlanIsModSend(resent8, rec, 1, hold, 0, 0.0),
	      "drop: a destination 8 units from a recorded re-send is not the re-send");
	Set3(resent8, 5009.5f, 0.0f, -7000.0f);
	Check(PlanIsModSend(resent8, rec, 1, hold, 0, 0.0),
	      "drop: a destination 1.5 units from a recorded re-send is the re-send");
}

static void CheckRepeat()
{
	float planDest[3], same[3], apart[3], apart12[3];
	Set3(planDest, 5000.0f, 40.0f, -7000.0f);
	Set3(same, 5000.5f, 0.0f, -7000.0f);
	Set3(apart, 5002.0f, 0.0f, -7000.0f);
	Set3(apart12, 5001.2f, 0.0f, -7000.0f);

	Check(PlanRepeatDue(planDest, same, 0.3), "repeat: the same destination inside the settle second is a repeat");
	Check(!PlanRepeatDue(planDest, apart, 0.3), "repeat: a destination two units away is not a repeat");
	Check(!PlanRepeatDue(planDest, apart12, 0.3), "repeat: a destination 1.2 units away is not a repeat");
	Check(!PlanRepeatDue(planDest, same, 1.5), "repeat: the same destination after the settle second is not a repeat");
}

static void CheckMemo()
{
	Check(PlanMemoKey(7, 9) == PlanMemoKey(7, 9), "memo: equal start and goal share a key");
	Check(PlanMemoKey(8, 9) != PlanMemoKey(7, 9) && PlanMemoKey(9, 7) != PlanMemoKey(7, 9),
	      "memo: another start does not");
	Check(PlanMemoSame(PlanMemoKey(7, 9), 5.0f, PlanMemoKey(7, 9), 5.0f), "memo: one multiplier shares a search");
	Check(!PlanMemoSame(PlanMemoKey(7, 9), 5.0f, PlanMemoKey(7, 9), 10.13f)
	      && !PlanMemoSame(PlanMemoKey(7, 9), 1.0f, PlanMemoKey(7, 9), 5.0f)
	      && !PlanMemoSame(PlanMemoKey(8, 9), 5.0f, PlanMemoKey(7, 9), 5.0f),
	      "memo: another multiplier is another search");
}

int main()
{
	CheckLegs();
	CheckFootprint();
	CheckDrop();
	CheckRepeat();
	CheckMemo();
	return CheckExit("plan_build_units");
}
