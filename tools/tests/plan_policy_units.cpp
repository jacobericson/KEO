// The route planner's decisions over a fixture route running east from cell (30, 30): portals at
// the borders into cells 31, 32 and 33, then the destination in cell 33. Every leg's portal edge
// runs 80 units along z around its midpoint.

#include <cmath>
#include <cstdio>
#include <cstring>
#include "planner/plan_policy.h"

#include "check.h"

using namespace planner;

// The order tracker's parked test, restated: edge mode, idle, destination beyond 100, not halted
// (10), and the waypoint within 20 is a park at the edge.
static const float PARK_REACH     = 20.0f;
static const float PARK_HALT      = 10.0f;
static const float PARK_FAR_DEST  = 100.0f;

static const float ROW_Z = -7000.0f;   // cell 30 in z

static void MakeLeg(PlanLeg* leg, float x, float z, int farSection, int isDestination)
{
	memset(leg, 0, sizeof(*leg));
	leg->point[0] = x;
	leg->point[1] = 50.0f;
	leg->point[2] = z;
	if (isDestination)
	{
		memcpy(leg->edgeA, leg->point, sizeof(leg->point));
		memcpy(leg->edgeB, leg->point, sizeof(leg->point));
	}
	else
	{
		leg->edgeA[0] = x; leg->edgeA[1] = 40.0f; leg->edgeA[2] = z - 40.0f;
		leg->edgeB[0] = x; leg->edgeB[1] = 60.0f; leg->edgeB[2] = z + 40.0f;
	}
	PlanCellOf(x, z, &leg->cellX, &leg->cellY);
	leg->farSection = farSection;
	leg->isDestination = isDestination;
}

// Legs 0-2 are the portals into cells 31, 32 and 33; leg 3 is the destination in cell 33.
static void BuildRoute(PlanLeg legs[4])
{
	MakeLeg(&legs[0], -4600.0f, ROW_Z, 11, 0);
	MakeLeg(&legs[1], 10.0f, ROW_Z, 12, 0);
	MakeLeg(&legs[2], 4620.0f, ROW_Z, 13, 0);
	MakeLeg(&legs[3], 6000.0f, ROW_Z, 13, 1);
}

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

static bool Near3(const float a[3], float x, float y, float z)
{
	return std::fabs(a[0] - x) < 0.01f && std::fabs(a[1] - y) < 0.01f && std::fabs(a[2] - z) < 0.01f;
}

static void CheckCellsAndMode()
{
	int cx = -1, cy = -1;
	PlanCellOf(-147456.0f, -147456.0f, &cx, &cy);
	bool origin = cx == 0 && cy == 0;
	PlanCellOf(0.0f, -0.5f, &cx, &cy);
	bool centre = cx == 32 && cy == 31;
	PlanCellOf(-147456.0f + 4608.0f * 28.0f + 1.0f, -147456.0f + 4608.0f * 19.0f - 1.0f, &cx, &cy);
	bool tile = cx == 28 && cy == 18;
	Check(origin && centre && tile, "cell: the tile grid");
	Check(PlanCellSpan(30, 30, 33, 31) == 3 && PlanCellSpan(33, 31, 30, 30) == 3 && PlanCellSpan(5, 5, 5, 5) == 0,
	      "cell: the span is Chebyshev");
	PlanLeg legs[4];
	BuildRoute(legs);
	Check(legs[0].cellX == 31 && legs[1].cellX == 32 && legs[2].cellX == 33 && legs[3].cellX == 33 &&
	      legs[0].cellY == 30, "cell: the fixture's legs sit in cells 31, 32, 33 and 33");

	Check(PLANNER_OFF == 0, "mode: off is zero");
	Check(PLANNER_OBSERVE == 1 && PLANNER_ON == 2, "mode: observe is one and on is two");
}

static void CheckVerdict()
{
	Check(PlanDecideVerdict(true, 4, 0xFu, 1, 2) == PV_DIRECT, "verdict: every leg loaded and span below legSpan is direct");
	Check(PlanDecideVerdict(true, 4, 0xBu, 1, 2) == PV_LEGGED, "verdict: one unloaded leg is legged");
	Check(PlanDecideVerdict(true, 4, 0xFu, 2, 2) == PV_LEGGED, "verdict: span at legSpan is legged");
	Check(PlanDecideVerdict(false, 0, 0xFFFFFFFFu, 0, 2) == PV_NO_ROUTE, "verdict: no route is no route");
	Check(PlanDecideVerdict(true, 32, 0xFFFFFFFFu, 0, 2) == PV_DIRECT &&
	      PlanDecideVerdict(true, 32, 0x7FFFFFFFu, 0, 2) == PV_LEGGED,
	      "verdict: all 32 legs read the whole mask");
}

static void CheckLegTarget()
{
	PlanLeg legs[4];
	BuildRoute(legs);
	// The 3x3 around cell 30 holds cells 29-31: leg 0's far side is in it, leg 1's is not.
	Check(PlanLegTarget(legs, 4, 0x1u, 0, 30, 30, 2) == 0, "leg: a plain 3x3 loaded set targets the first exit");
	Check(PlanLegTarget(legs, 4, 0xFu, 0, 30, 30, 3) == 1 && PlanLegTarget(legs, 4, 0x1u, 0, 30, 30, 3) == 0,
	      "leg: a widened loaded set targets a bounded leg");
	Check(PlanLegTarget(legs, 4, 0xFu, 0, 30, 30, 2) == 0 && PlanLegTarget(legs, 4, 0xFu, 0, 31, 30, 2) == 1,
	      "leg: the target stays within legSpan - 1 cells");
	Check(PlanLegTarget(legs, 4, 0x0u, 0, 30, 30, 2) == 0 && PlanLegTarget(legs, 4, 0x1u, 1, 31, 30, 2) == 1,
	      "leg: an unloaded next leg is itself the target");

	// A loop: out through cells 31 and 32, back into 32 then 31 two rows up, beyond the bound.
	PlanLeg loop[4];
	MakeLeg(&loop[0], -4600.0f, ROW_Z, 11, 0);
	MakeLeg(&loop[1], 10.0f, ROW_Z, 12, 0);
	MakeLeg(&loop[2], 10.0f, ROW_Z + 9216.0f, 22, 0);
	MakeLeg(&loop[3], -4600.0f, ROW_Z + 9216.0f, 21, 1);
	Check(loop[2].cellY == 32 && loop[3].cellX == 31 && loop[3].cellY == 32 &&
	      PlanLegTarget(loop, 4, 0xFu, 0, 30, 30, 2) == 0 && PlanLegTarget(loop, 4, 0x5u, 0, 30, 30, 2) == 0 &&
	      PlanLegTarget(loop, 4, 0xDu, 0, 30, 30, 2) == 0,
	      "leg: a loop whose re-entry is loaded but beyond the bound is not the target");

	// A loaded, bounded leg after an unloaded one is past a stretch the character cannot walk.
	PlanLeg back[3];
	MakeLeg(&back[0], -4600.0f, ROW_Z, 11, 0);
	MakeLeg(&back[1], -4600.0f, ROW_Z + 4608.0f, 12, 0);
	MakeLeg(&back[2], -4700.0f, ROW_Z + 10.0f, 10, 1);
	Check(PlanLegTarget(back, 3, 0x5u, 0, 30, 30, 2) == 0, "leg: an unloaded leg ends the run");

	PlanLeg dest[1];
	MakeLeg(&dest[0], 6000.0f, ROW_Z, 13, 1);
	bool alone = PlanLegTarget(dest, 1, 0x1u, 0, 33, 30, 2) == 0 && PlanLegTarget(dest, 1, 0x0u, 0, 33, 30, 2) == -1 &&
	             PlanLegTarget(dest, 1, 0x1u, 0, 30, 30, 2) == -1;
	bool onRoute = PlanLegTarget(legs, 4, 0xFu, 0, 32, 30, 2) == 3 && PlanLegTarget(legs, 4, 0x7u, 0, 32, 30, 2) == 2 &&
	               PlanLegTarget(legs, 4, 0xFu, 3, 30, 30, 2) == -1;
	Check(alone && onRoute, "leg: the destination qualifies only loaded and within the bound");
	Check(PlanLegTarget(legs, 4, 0xFu, 4, 30, 30, 2) == -1 && PlanLegTarget(legs, 4, 0xFu, -1, 30, 30, 2) == -1,
	      "leg: a start outside the legs has no target");
	Check(PlanLegTarget(legs, 4, 0x3u, 0, 30, 30, 1) == 0, "leg: a portal with no bounded run is still the next step");
}

static void EdgeIn(PlanEdgeIn* in, int site, float offset, const float pos[3], int legIndex, unsigned mask)
{
	memset(in, 0, sizeof(*in));
	in->site = site;
	in->offset = offset;
	memcpy(in->pos, pos, sizeof(in->pos));
	in->legIndex = legIndex;
	in->loadedMask = mask;
	in->legSpan = 2;
}

static void CheckEdgeArrival()
{
	PlanLeg legs[4];
	BuildRoute(legs);
	float atLeg0[3];
	Set3(atLeg0, -4590.0f, 50.0f, ROW_Z + 5.0f);
	PlanEdgeIn in;
	PlanEdgeOut rc, cp;

	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x7u);
	PlanEdgeStep(legs, 4, in, &rc);
	EdgeIn(&in, PES_COMPUTE, 0.0f, atLeg0, rc.newLegIndex, 0x7u);
	PlanEdgeStep(legs, 4, in, &cp);
	bool advanced = rc.action == PEA_POINT && cp.action == PEA_POINT && Same3(rc.point, cp.point);
	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x0u);
	PlanEdgeStep(legs, 4, in, &rc);
	EdgeIn(&in, PES_COMPUTE, 0.0f, atLeg0, rc.newLegIndex, 0x0u);
	PlanEdgeStep(legs, 4, in, &cp);
	Check(advanced && Same3(rc.point, cp.point), "edge: the recheck and the recompute of one frame return the same point");

	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x7u);
	PlanEdgeStep(legs, 4, in, &rc);
	Check(rc.action == PEA_POINT && rc.newLegIndex == 1 && rc.waiting == 0 && Same3(rc.point, legs[1].point),
	      "edge: arrival within reach advances to the next target");
	float dx = rc.point[0] - atLeg0[0], dy = rc.point[1] - atLeg0[1], dz = rc.point[2] - atLeg0[2];
	Check(dx * dx + dy * dy + dz * dz > 40.0f, "edge: an advanced point moves more than sqrt(40)");

	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x1u);
	PlanEdgeStep(legs, 4, in, &rc);
	Check(rc.newLegIndex == 1 && rc.waiting == 0 && Same3(rc.point, legs[1].point),
	      "edge: arrival with the next far side unloaded walks to that portal");

	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x0u);
	PlanEdgeStep(legs, 4, in, &rc);
	Check(rc.action == PEA_POINT && rc.newLegIndex == 0 && rc.waiting == 1 && Same3(rc.point, legs[0].point),
	      "edge: arrival with nothing loaded holds and waits");

	float away[3];
	Set3(away, -4600.0f - 25.0f, 50.0f, ROW_Z);
	EdgeIn(&in, PES_RECHECK, 0.0f, away, 0, 0x7u);
	PlanEdgeStep(legs, 4, in, &rc);
	Check(rc.newLegIndex == 0 && rc.waiting == 0 && Same3(rc.point, legs[0].point),
	      "edge: outside the reach the recheck keeps the current portal");

	PlanLeg trunc[4];
	BuildRoute(trunc);
	float atLeg2[3];
	Set3(atLeg2, 4625.0f, 50.0f, ROW_Z);
	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg2, 2, 0xFu);
	in.routeTruncated = 1;
	PlanEdgeStep(trunc, 4, in, &rc);
	bool held = rc.newLegIndex == 2 && rc.waiting == 1 && Same3(rc.point, trunc[2].point);
	in.routeTruncated = 0;
	PlanEdgeStep(trunc, 4, in, &cp);
	Check(held && cp.newLegIndex == 3, "edge: a truncated plan's last portal holds instead of advancing onto the destination");
}

static void CheckEdgeRungs()
{
	PlanLeg legs[4];
	BuildRoute(legs);
	float pos[3];
	Set3(pos, -4700.0f, 50.0f, ROW_Z);
	PlanEdgeIn in;
	PlanEdgeOut out;

	EdgeIn(&in, PES_COMPUTE, 10.0f, pos, 0, 0x0u);
	PlanEdgeStep(legs, 4, in, &out);
	Check(out.action == PEA_POINT && out.rung == 1 && Near3(out.point, -4600.0f, 52.5f, ROW_Z + 10.0f),
	      "edge: a rung slides along the edge");

	EdgeIn(&in, PES_COMPUTE, 100.0f, pos, 0, 0x0u);
	PlanEdgeStep(legs, 4, in, &out);
	bool high = Near3(out.point, -4600.0f, 58.75f, ROW_Z + 35.0f);
	EdgeIn(&in, PES_COMPUTE, -100.0f, pos, 0, 0x0u);
	PlanEdgeStep(legs, 4, in, &out);
	bool low = Near3(out.point, -4600.0f, 41.25f, ROW_Z - 35.0f);
	PlanLeg shortEdge;
	MakeLeg(&shortEdge, 100.0f, 200.0f, 1, 0);
	shortEdge.edgeA[2] = 196.0f;
	shortEdge.edgeB[2] = 204.0f;
	float mid[3];
	PlanRungSlide(shortEdge, 3.0f, mid);
	Check(high && low && Near3(mid, 100.0f, 50.0f, 200.0f), "edge: a rung is clamped inside the edge");

	EdgeIn(&in, PES_COMPUTE, 10.0f, pos, 3, 0xFu);
	PlanEdgeStep(legs, 4, in, &out);
	Check(out.action == PEA_POINT && Same3(out.point, legs[3].point), "edge: the destination leg is never slid");

	EdgeIn(&in, PES_COMPUTE, 0.0f, pos, 1, 0x0u);
	PlanEdgeStep(legs, 4, in, &out);
	Check(out.rung == 0 && Same3(out.point, legs[1].point), "edge: the recompute returns the current point");

	EdgeIn(&in, PES_OTHER, 0.0f, pos, 0, 0xFu);
	PlanEdgeStep(legs, 4, in, &out);
	bool other = out.action == PEA_PASS;
	EdgeIn(&in, PES_RECHECK, 0.0f, pos, 4, 0xFu);
	PlanEdgeStep(legs, 4, in, &out);
	bool past = out.action == PEA_PASS;
	EdgeIn(&in, PES_COMPUTE, 0.0f, pos, -1, 0xFu);
	PlanEdgeStep(legs, 4, in, &out);
	Check(other && past && out.action == PEA_PASS, "edge: another site passes");
}

static void FlipIn(PlanFlipIn* in, int mode, int verdict, int legIsDestination)
{
	memset(in, 0, sizeof(*in));
	in->mode = mode;
	in->haveChar = 1;
	in->haveSlot = 1;
	in->verdict = verdict;
	in->legIsDestination = legIsDestination;
	Set3(in->dest, 100.0f, 20.0f, 300.0f);
	Set3(in->finalDest, 101.0f, 20.0f, 300.5f);
}

static void CheckFlip()
{
	PlanFlipIn in;
	FlipIn(&in, PLANNER_ON, PV_LEGGED, 0);
	in.haveChar = 0;
	bool noChar = PlanFlipRule(in) == PFA_NOT_MINE && PlanFlipRuleOn(in) == PFA_NOT_MINE;
	FlipIn(&in, PLANNER_ON, PV_LEGGED, 0);
	in.haveSlot = 0;
	Check(noChar && PlanFlipRule(in) == PFA_NOT_MINE, "flip: no thread-local character answers not-mine");

	FlipIn(&in, PLANNER_ON, PV_LEGGED, 0);
	Set3(in.dest, 150.0f, 20.0f, 300.0f);
	Check(PlanFlipRule(in) == PFA_NOT_MINE, "flip: a road waypoint answers not-mine");

	FlipIn(&in, PLANNER_ON, PV_LEGGED, 0);
	Check(PlanFlipRule(in) == PFA_FALSE, "flip: legged with a portal leg answers false in on");
	FlipIn(&in, PLANNER_ON, PV_LEGGED, 1);
	Check(PlanFlipRule(in) == PFA_VANILLA, "flip: the destination leg answers vanilla");
	FlipIn(&in, PLANNER_ON, PV_DIRECT, 0);
	Check(PlanFlipRule(in) == PFA_VANILLA, "flip: direct answers vanilla");
	FlipIn(&in, PLANNER_ON, PV_NO_ROUTE, 0);
	bool noRoute = PlanFlipRule(in) == PFA_NOT_MINE;
	FlipIn(&in, PLANNER_ON, PV_NONE, 0);
	Check(noRoute && PlanFlipRule(in) == PFA_NOT_MINE, "flip: no route answers not-mine");

	FlipIn(&in, PLANNER_OBSERVE, PV_LEGGED, 0);
	bool obsLegged = PlanFlipRule(in) == PFA_NOT_MINE;
	FlipIn(&in, PLANNER_OBSERVE, PV_DIRECT, 0);
	Check(obsLegged && PlanFlipRule(in) == PFA_NOT_MINE, "flip: observe answers not-mine");
	FlipIn(&in, PLANNER_OFF, PV_LEGGED, 0);
	Check(PlanFlipRule(in) == PFA_NOT_MINE, "flip: off answers not-mine");

	bool same = true;
	for (int v = PV_NONE; v <= PV_NO_ROUTE; ++v)
		for (int d = 0; d < 2; ++d)
		{
			PlanFlipIn on, obs;
			FlipIn(&on, PLANNER_ON, v, d);
			FlipIn(&obs, PLANNER_OBSERVE, v, d);
			same = same && PlanFlipRuleOn(obs) == PlanFlipRule(on);
		}
	FlipIn(&in, PLANNER_OBSERVE, PV_LEGGED, 0);
	Check(same && PlanFlipRuleOn(in) == PFA_FALSE, "flip: observe's would-answer is on's");
}

static void CheckDestSteerAhead()
{
	float a[3], b[3];
	Set3(a, 10.0f, 5.0f, 20.0f);
	Set3(b, 11.0f, 5.0f, 21.5f);   // 1.8 apart
	Check(PlanDestMatches(a, b), "dest: within two units matches");
	Set3(b, 10.0f, 8.0f, 20.0f);
	Check(!PlanDestMatches(a, b), "dest: three units does not match");

	Check(!PlanEdgeSteers(PLANNER_OBSERVE, PV_LEGGED, true) && !PlanEdgeSteers(PLANNER_OFF, PV_LEGGED, true),
	      "steer: observe never steers");
	Check(PlanEdgeSteers(PLANNER_ON, PV_LEGGED, true), "steer: on steers a legged plan whose destination matches");
	Check(!PlanEdgeSteers(PLANNER_ON, PV_DIRECT, true) && !PlanEdgeSteers(PLANNER_ON, PV_NO_ROUTE, true),
	      "steer: on does not steer a direct plan");
	Check(!PlanEdgeSteers(PLANNER_ON, PV_LEGGED, false), "steer: on does not steer a mismatched destination");

	Check(!PlanReplacesAhead(PLANNER_OBSERVE, PV_LEGGED) && !PlanReplacesAhead(PLANNER_OFF, PV_LEGGED),
	      "ahead: observe keeps the straight-line enqueue");
	Check(PlanReplacesAhead(PLANNER_ON, PV_LEGGED), "ahead: on skips it for a legged plan");
	Check(!PlanReplacesAhead(PLANNER_ON, PV_DIRECT) && !PlanReplacesAhead(PLANNER_ON, PV_NO_ROUTE),
	      "ahead: on keeps it for a direct plan");
}

// IslandClassifyEdgePark's at-the-edge answer with the thresholds above.
static bool ParkedAtEdge(bool movingToEdge, bool idle, float haltDist, float wpDist, float destDist)
{
	if (!movingToEdge || !idle || !(destDist > PARK_FAR_DEST)) return false;
	if (haltDist < PARK_HALT) return false;
	return wpDist < PARK_REACH;
}

static void CheckOwns()
{
	Check(PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 12.0f), "owns: on, legged, waiting and within reach owns the wait");
	Check(!PlanOwnsWait(PLANNER_OBSERVE, PV_LEGGED, 0, 1, 12.0f) && !PlanOwnsWait(PLANNER_OFF, PV_LEGGED, 0, 1, 12.0f),
	      "owns: observe never owns a wait");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 1, 1, 12.0f), "owns: the destination leg is never owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 20.0f) && !PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 35.0f),
	      "owns: beyond reach is not owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 12.0f) && !PlanOwnsWait(PLANNER_ON, PV_DIRECT, 0, 1, 12.0f),
	      "owns: no waiting word or a direct plan is not owned");

	bool subset = PLAN_REACH <= PARK_REACH && PLAN_POST_ARRIVAL == PARK_FAR_DEST;
	float dists[5] = { 0.0f, 5.0f, 12.0f, 19.0f, 19.99f };
	for (int i = 0; i < 5; ++i)
		if (PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, dists[i]))
			subset = subset && ParkedAtEdge(true, true, 50.0f, dists[i], 500.0f);
	Check(subset && PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 19.99f), "owns: an owned wait satisfies the parked predicate");
}

static void Quiet(PlanReplanIn* in)
{
	memset(in, 0, sizeof(*in));
	in->now = 1000.0;
	in->planTime = 990.0;
	in->waitSeconds = 10;
	in->legIndex = 1;
	in->legCount = 5;
	in->distToPortal = 50.0f;
}

static void CheckReplan()
{
	PlanReplanIn in;
	Quiet(&in);
	bool quiet = PlanReplanDue(in) == PRW_NONE;
	in.rungs = 2;
	bool two = PlanReplanDue(in) == PRW_NONE;
	in.rungs = 3;
	in.completeSince = 900.0;
	Check(quiet && two && PlanReplanDue(in) == PRW_RUNGS, "replan: three rungs");

	Quiet(&in);
	in.completeSince = 992.0;
	bool due = PlanReplanDue(in) == PRW_ARRIVAL_TIMEOUT;
	in.completeSince = 992.5;
	bool early = PlanReplanDue(in) == PRW_NONE;
	in.completeSince = 900.0;
	in.distToPortal = 10.0f;
	bool near = PlanReplanDue(in) == PRW_NONE;
	Check(due && early && near, "replan: the arrival timeout");

	Quiet(&in);
	in.waitSince = 990.0;
	in.loadedChangedSincePlan = 1;
	bool wait = PlanReplanDue(in) == PRW_WAIT;
	in.loadedChangedSincePlan = 0;
	bool unchanged = PlanReplanDue(in) == PRW_NONE;
	in.loadedChangedSincePlan = 1;
	in.awaitedLoaded = 1;
	bool arrived = PlanReplanDue(in) == PRW_NONE;
	in.awaitedLoaded = 0;
	in.waitSince = 991.0;
	bool soon = PlanReplanDue(in) == PRW_NONE;
	Check(wait && unchanged && arrived && soon, "replan: the wait needs a changed loaded set");

	Quiet(&in);
	in.goalByFootprint = 1;
	bool notYet = PlanReplanDue(in) == PRW_NONE;
	in.goalLoaded = 1;
	bool loaded = PlanReplanDue(in) == PRW_GOAL_LOADED;
	in.goalByFootprint = 0;
	Check(notYet && loaded && PlanReplanDue(in) == PRW_NONE, "replan: a footprint goal re-plans once its section loads");

	Quiet(&in);
	in.planTime = 880.0;
	bool old = PlanReplanDue(in) == PRW_AGE;
	in.planTime = 880.5;
	Check(old && PlanReplanDue(in) == PRW_NONE, "replan: age");

	Quiet(&in);
	in.routeTruncated = 1;
	in.legIndex = 3;
	in.distToPortal = 12.0f;
	bool end = PlanReplanDue(in) == PRW_ROUTE_END;
	in.distToPortal = 25.0f;
	bool far = PlanReplanDue(in) == PRW_NONE;
	in.distToPortal = 12.0f;
	in.legIndex = 2;
	bool earlier = PlanReplanDue(in) == PRW_NONE;
	in.legIndex = 3;
	in.routeTruncated = 0;
	Check(end && far && earlier && PlanReplanDue(in) == PRW_NONE, "replan: arrival at a truncated route's last portal");

	Quiet(&in);
	in.completeSince = 900.0;
	in.waitSince = 900.0;
	in.loadedChangedSincePlan = 1;
	in.goalByFootprint = 1;
	in.goalLoaded = 1;
	in.planTime = 0.0;
	bool first = PlanReplanDue(in) == PRW_ARRIVAL_TIMEOUT;
	in.completeSince = 0.0;
	bool second = PlanReplanDue(in) == PRW_WAIT;
	in.waitSince = 0.0;
	bool third = PlanReplanDue(in) == PRW_GOAL_LOADED;
	in.goalLoaded = 0;
	Check(first && second && third && PlanReplanDue(in) == PRW_AGE, "replan: the triggers are taken in order");
}

static void CheckFeed()
{
	PlanLeg legs[7];
	MakeLeg(&legs[0], -4600.0f, ROW_Z, 11, 0);              // cell 31: the current leg
	MakeLeg(&legs[1], -4500.0f, ROW_Z + 100.0f, 11, 0);     // cell 31 again: skipped
	MakeLeg(&legs[2], 10.0f, ROW_Z, 5000, 0);               // an interior: skipped
	MakeLeg(&legs[3], 20.0f, ROW_Z, 12, 0);                 // cell 32
	MakeLeg(&legs[4], 30.0f, ROW_Z + 10.0f, 12, 0);         // cell 32 again: not repeated
	MakeLeg(&legs[5], 4620.0f, ROW_Z, 13, 0);               // cell 33
	MakeLeg(&legs[6], 9300.0f, ROW_Z, 14, 1);               // cell 34
	int xy[16];
	memset(xy, -1, sizeof(xy));
	int n = PlanFeedCells(legs, 7, 0, 2, 4096, xy);
	bool two = n == 2 && xy[0] == 32 && xy[1] == 30 && xy[2] == 33 && xy[3] == 30 && xy[4] == -1;
	n = PlanFeedCells(legs, 7, 0, 8, 4096, xy);
	bool all = n == 3 && xy[4] == 34 && xy[5] == 30;
	int none = PlanFeedCells(legs, 7, 0, 0, 4096, xy) + PlanFeedCells(legs, 7, 6, 3, 4096, xy);
	Check(two && all && none == 0, "feed: at most ahead distinct exterior cells after the current leg");
}

int main()
{
	CheckCellsAndMode();
	CheckVerdict();
	CheckLegTarget();
	CheckEdgeArrival();
	CheckEdgeRungs();
	CheckFlip();
	CheckDestSteerAhead();
	CheckOwns();
	CheckReplan();
	CheckFeed();
	return CheckExit("plan_policy_units");
}
