// The route planner's plan-building rules: the legs from a coarse route, the footprint pick, the
// drop predicate and the memo key. Routes run east along one row of cells; each section change's
// portal sits on the border between two cells.

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
	float planDest[3], moveDest[3];
	Set3(planDest, 5000.0f, 40.0f, -7000.0f);
	Set3(moveDest, 5000.0f, 0.0f, -7000.0f);

	Check(PlanDropDue(false, false, 900.0f, moveDest, planDest, 5.0) == PDW_NOT_PLAYER, "drop: not a player drops");
	Check(PlanDropDue(true, true, 900.0f, moveDest, planDest, 5.0) == PDW_KO, "drop: unconscious drops");
	Check(PlanDropDue(true, false, 99.0f, moveDest, planDest, 5.0) == PDW_ARRIVED
	      && PlanDropDue(true, false, 101.0f, moveDest, planDest, 5.0) == PDW_NONE,
	      "drop: within 100 of the destination drops");

	float off[3];
	Set3(off, 5003.0f, 0.0f, -7000.0f);
	Check(PlanDropDue(true, false, 900.0f, off, planDest, 5.0) == PDW_NEW_DEST, "drop: a movement destination 3 units off drops");

	float zero[3];
	Set3(zero, 0.0f, 0.0f, 0.0f);
	Check(PlanDropDue(true, false, 900.0f, zero, planDest, 5.0) == PDW_NONE, "drop: a zero movement destination does not drop");

	Check(PlanDropDue(true, false, 900.0f, moveDest, planDest, 5.0) == PDW_NONE, "drop: a matching destination does not drop");

	float moved[3];
	Set3(moved, 5050.0f, 0.0f, -7000.0f);
	Check(PlanDropDue(true, false, 900.0f, moved, planDest, 0.5) == PDW_NONE,
	      "drop: a new destination inside the settle second is not a drop");
	Check(PlanDropDue(true, false, 900.0f, moved, planDest, 1.5) == PDW_NEW_DEST,
	      "drop: a new destination after the settle second drops");
}

static void CheckMemo()
{
	Check(PlanMemoKey(7, 9) == PlanMemoKey(7, 9), "memo: equal start and goal share a key");
	Check(PlanMemoKey(8, 9) != PlanMemoKey(7, 9) && PlanMemoKey(9, 7) != PlanMemoKey(7, 9),
	      "memo: another start does not");
}

int main()
{
	CheckLegs();
	CheckFootprint();
	CheckDrop();
	CheckMemo();
	return CheckExit("plan_build_units");
}
