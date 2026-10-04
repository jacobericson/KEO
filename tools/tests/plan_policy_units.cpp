// The route planner's decisions over a fixture route running east from cell (30, 30): portals at
// the borders into cells 31, 32 and 33, then the destination in cell 33. Every leg's portal edge
// runs 80 units along z around its midpoint.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cfloat>
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
	Check(PlanDecideVerdict(true, 4, 0xFu, 1, 2, 0) == PV_DIRECT, "verdict: every leg loaded and span below legSpan is direct");
	Check(PlanDecideVerdict(true, 4, 0xBu, 1, 2, 0) == PV_LEGGED, "verdict: one unloaded leg is legged");
	Check(PlanDecideVerdict(true, 4, 0xFu, 2, 2, 0) == PV_LEGGED, "verdict: span at legSpan is legged");
	Check(PlanDecideVerdict(false, 0, 0xFFFFFFFFu, 0, 2, 0) == PV_NO_ROUTE, "verdict: no route is no route");
	Check(PlanDecideVerdict(true, 32, 0xFFFFFFFFu, 0, 2, 0) == PV_DIRECT &&
	      PlanDecideVerdict(true, 32, 0x7FFFFFFFu, 0, 2, 0) == PV_LEGGED,
	      "verdict: all 32 legs read the whole mask");
}

static void CheckLegTarget()
{
	PlanLeg legs[4];
	BuildRoute(legs);
	// The 3x3 around cell 30 holds cells 29-31: leg 0's far side is in it, leg 1's is not.
	Check(PlanLegTarget(legs, 4, 0x1u, 0, 30, 30, 2, 0, 0) == 0, "leg: a plain 3x3 loaded set targets the first exit");
	Check(PlanLegTarget(legs, 4, 0xFu, 0, 30, 30, 3, 0, 0) == 1 && PlanLegTarget(legs, 4, 0x1u, 0, 30, 30, 3, 0, 0) == 0,
	      "leg: a widened loaded set targets a bounded leg");
	Check(PlanLegTarget(legs, 4, 0xFu, 0, 30, 30, 2, 0, 0) == 0 && PlanLegTarget(legs, 4, 0xFu, 0, 31, 30, 2, 0, 0) == 1,
	      "leg: the target stays within legSpan - 1 cells");
	Check(PlanLegTarget(legs, 4, 0x0u, 0, 30, 30, 2, 0, 0) == 0 && PlanLegTarget(legs, 4, 0x1u, 1, 31, 30, 2, 0, 0) == 1,
	      "leg: an unloaded next leg is itself the target");

	// A loop: out through cells 31 and 32, back into 32 then 31 two rows up, beyond the bound.
	PlanLeg loop[4];
	MakeLeg(&loop[0], -4600.0f, ROW_Z, 11, 0);
	MakeLeg(&loop[1], 10.0f, ROW_Z, 12, 0);
	MakeLeg(&loop[2], 10.0f, ROW_Z + 9216.0f, 22, 0);
	MakeLeg(&loop[3], -4600.0f, ROW_Z + 9216.0f, 21, 1);
	Check(loop[2].cellY == 32 && loop[3].cellX == 31 && loop[3].cellY == 32 &&
	      PlanLegTarget(loop, 4, 0xFu, 0, 30, 30, 2, 0, 0) == 0 && PlanLegTarget(loop, 4, 0x5u, 0, 30, 30, 2, 0, 0) == 0 &&
	      PlanLegTarget(loop, 4, 0xDu, 0, 30, 30, 2, 0, 0) == 0,
	      "leg: a loop whose re-entry is loaded but beyond the bound is not the target");

	// A loaded, bounded leg after an unloaded one is past a stretch the character cannot walk.
	PlanLeg back[3];
	MakeLeg(&back[0], -4600.0f, ROW_Z, 11, 0);
	MakeLeg(&back[1], -4600.0f, ROW_Z + 4608.0f, 12, 0);
	MakeLeg(&back[2], -4700.0f, ROW_Z + 10.0f, 10, 1);
	Check(PlanLegTarget(back, 3, 0x5u, 0, 30, 30, 2, 0, 0) == 0, "leg: an unloaded leg ends the run");

	PlanLeg dest[1];
	MakeLeg(&dest[0], 6000.0f, ROW_Z, 13, 1);
	bool alone = PlanLegTarget(dest, 1, 0x1u, 0, 33, 30, 2, 0, 0) == 0 && PlanLegTarget(dest, 1, 0x0u, 0, 33, 30, 2, 0, 0) == -1 &&
	             PlanLegTarget(dest, 1, 0x1u, 0, 30, 30, 2, 0, 0) == -1;
	bool onRoute = PlanLegTarget(legs, 4, 0xFu, 0, 32, 30, 2, 0, 0) == 3 && PlanLegTarget(legs, 4, 0x7u, 0, 32, 30, 2, 0, 0) == 2 &&
	               PlanLegTarget(legs, 4, 0xFu, 3, 30, 30, 2, 0, 0) == -1;
	Check(alone && onRoute, "leg: the destination qualifies only loaded and within the bound");
	Check(PlanLegTarget(legs, 4, 0xFu, 4, 30, 30, 2, 0, 0) == -1 && PlanLegTarget(legs, 4, 0xFu, -1, 30, 30, 2, 0, 0) == -1,
	      "leg: a start outside the legs has no target");
	Check(PlanLegTarget(legs, 4, 0x3u, 0, 30, 30, 1, 0, 0) == 0, "leg: a portal with no bounded run is still the next step");
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

	FlipIn(&in, PLANNER_ON, PV_LEGGED, 0);
	Set3(in.dest, 109.0f, 20.0f, 300.5f);
	Set3(in.resend[0], 109.0f, 20.0f, 300.5f);
	in.resendCount = 1;
	Check(PlanFlipRule(in) == PFA_FALSE, "flip: a call carrying the recorded re-send answers false");
	in.resendCount = 0;
	Check(PlanFlipRule(in) == PFA_NOT_MINE, "flip: a call eight units off without a recorded re-send answers not-mine");

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

	float finalDest[3], probe[3];
	float rec[PLAN_RESEND_POINTS][3];
	memset(rec, 0, sizeof(rec));
	Set3(finalDest, 10.0f, 5.0f, 20.0f);
	Set3(rec[0], 18.0f, 5.0f, 20.0f);
	Set3(probe, 19.0f, 5.0f, 20.5f);
	bool near = PlanDestIsPlans(probe, finalDest, rec, 1);
	Set3(probe, 21.0f, 5.0f, 20.0f);
	bool past = PlanDestIsPlans(probe, finalDest, rec, 1);
	Set3(probe, 18.0f, 5.0f, 20.0f);
	bool unrecorded = PlanDestIsPlans(probe, finalDest, rec, 0);
	Check(near && !past && !unrecorded, "dest: a recorded re-send matches within two units only");

	float leg[3];
	Set3(leg, 300.0f, 0.0f, 40.0f);
	Set3(probe, 10.5f, 0.0f, 21.0f);   // the final destination in x-z, its height not read
	bool xzFinal = PlanDestIsPlansXz(probe, finalDest, rec, 1);
	Set3(probe, 18.5f, 0.0f, 19.0f);
	bool xzResend = PlanDestIsPlansXz(probe, finalDest, rec, 1);
	bool xzLeg = PlanDestIsPlansXz(leg, finalDest, rec, 1);
	Set3(probe, 60.0f, 0.0f, 60.0f);   // a halt at the character's own position, far from all three
	bool xzHalt = PlanDestIsPlansXz(probe, finalDest, rec, 1);
	Set3(probe, 18.0f, 0.0f, 20.0f);
	bool xzUnrecorded = PlanDestIsPlansXz(probe, finalDest, rec, 0);
	Check(xzFinal && xzResend, "dest: x-z matches the final destination or a re-send whatever the height");
	Check(!xzLeg && !xzHalt && !xzUnrecorded, "dest: x-z never matches a far point (the leg is not an input), a halt or an unrecorded re-send");

	float raw[3], snapped[3];
	Set3(raw, 0.0f, 0.0f, 0.0f);
	Set3(snapped, 3.0f, 9.0f, 4.0f);
	Check(std::fabs(PlanSnapDistance(raw, snapped) - 5.0f) < 0.001f, "snap: the snap distance is measured in x-z");

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

// The subset rows' movement destinations, as |movement destination - position| (the tracker's halt
// measure), with the character at the origin and the plan's destination 500 away (the rows' destDist):
// a halt (under PARK_HALT) or a point 50 away is neither the destination nor a re-send, which lies
// within PLAN_RESEND_REACH of it; 500 is the destination itself and 492 a recorded re-send.
static const int HALT_COUNT = 6;
static const float HALTS[HALT_COUNT] = { 0.0f, 5.0f, 9.99f, 50.0f, 500.0f, 492.0f };

static bool HaltIsPlans(float haltDist)
{
	float moveDest[3], finalDest[3];
	float rec[PLAN_RESEND_POINTS][3];
	Set3(moveDest, haltDist, 0.0f, 0.0f);
	Set3(finalDest, 500.0f, 0.0f, 0.0f);
	Set3(rec[0], 492.0f, 0.0f, 0.0f);
	Set3(rec[1], 505.0f, 0.0f, 3.0f);
	return PlanDestIsPlansXz(moveDest, finalDest, rec, 2);
}

static void CheckOwns()
{
	static const float FAR_WP = 1.0e9f;
	Check(PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 12.0f, FAR_WP, FAR_WP, true), "owns: on, legged, waiting and within reach owns the wait");
	Check(!PlanOwnsWait(PLANNER_OBSERVE, PV_LEGGED, 0, 1, 12.0f, FAR_WP, FAR_WP, true) && !PlanOwnsWait(PLANNER_OFF, PV_LEGGED, 0, 1, 12.0f, FAR_WP, FAR_WP, true),
	      "owns: observe never owns a wait");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 1, 1, 12.0f, FAR_WP, FAR_WP, true), "owns: the destination leg is never owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 20.0f, FAR_WP, FAR_WP, true) && !PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 35.0f, FAR_WP, FAR_WP, true),
	      "owns: beyond reach is not owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 12.0f, FAR_WP, FAR_WP, true) && !PlanOwnsWait(PLANNER_ON, PV_DIRECT, 0, 1, 12.0f, FAR_WP, FAR_WP, true),
	      "owns: no waiting word or a direct plan is not owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 12.0f, FAR_WP, FAR_WP, false), "owns: a halt at the portal is not owned (held wait)");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 7.0f, 5.0f, 3.0f, false), "owns: a halt at the portal is not owned (arrival)");

	bool subset = PLAN_REACH <= PARK_REACH && PLAN_POST_ARRIVAL == PARK_FAR_DEST;
	int ownedWaits = 0;
	float dists[5] = { 0.0f, 5.0f, 12.0f, 19.0f, 19.99f };
	for (int i = 0; i < 5; ++i)
		for (int h = 0; h < HALT_COUNT; ++h)
		{
			bool mine = HaltIsPlans(HALTS[h]);
			if (PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, dists[i], FAR_WP, FAR_WP, mine))
			{
				++ownedWaits;
				subset = subset && ParkedAtEdge(true, true, HALTS[h], dists[i], 500.0f);
			}
		}
	Check(subset && ownedWaits == 10 && PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 1, 19.99f, FAR_WP, FAR_WP, true),
	      "owns: an owned wait satisfies the parked predicate");

	// The stop at the planner's own portal, before the engine's next advance: waiting word clear.
	Check(PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 7.0f, 5.0f, 3.0f, true), "owns: standing at the planner's waypoint owns the arrival");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 300.0f, 5.0f, 295.0f, true), "owns: walking toward the planner's waypoint is not owned");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 27.0f, 25.0f, 3.0f, true), "owns: a waypoint beyond reach of the portal is not owned");
	Check(!PlanOwnsWait(PLANNER_OBSERVE, PV_LEGGED, 0, 0, 7.0f, 5.0f, 3.0f, true) && !PlanOwnsWait(PLANNER_OFF, PV_LEGGED, 0, 0, 7.0f, 5.0f, 3.0f, true),
	      "owns: observe never owns an arrival");
	Check(!PlanOwnsWait(PLANNER_ON, PV_LEGGED, 1, 0, 7.0f, 5.0f, 3.0f, true), "owns: the destination leg's arrival is never owned");

	bool arrivalSubset = true;
	int ownedArrivals = 0;
	float toWp[5] = { 0.0f, 5.0f, 12.0f, 19.0f, 19.99f };
	for (int i = 0; i < 5; ++i)
		for (int h = 0; h < HALT_COUNT; ++h)
		{
			bool mine = HaltIsPlans(HALTS[h]);
			if (PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, toWp[i] + 5.0f, 5.0f, toWp[i], mine))
			{
				++ownedArrivals;
				arrivalSubset = arrivalSubset && ParkedAtEdge(true, true, HALTS[h], toWp[i], 500.0f);
			}
		}
	Check(arrivalSubset && ownedArrivals == 10 && PlanOwnsWait(PLANNER_ON, PV_LEGGED, 0, 0, 24.99f, 5.0f, 19.99f, true),
	      "owns: an owned arrival satisfies the parked predicate");
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

static void CheckComplete()
{
	Check(!PlanLegComplete(1, 300.0f, 1, 2), "complete: a character walking its leg is not complete");
	Check(PlanLegComplete(1, 30.0f, 1, 1) && PlanLegComplete(1, 30.0f, 1, 0),
	      "complete: a character stopped at its path's end outside reach is complete");
	Check(!PlanLegComplete(1, 10.0f, 1, 1), "complete: a character stopped within reach is not complete");
	Check(!PlanLegComplete(1, 30.0f, 4, 1) && !PlanLegComplete(1, 30.0f, 0, 1), "complete: a path still waiting is not complete");
	Check(!PlanLegComplete(0, 30.0f, 1, 1), "complete: the destination leg is never complete");
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

static void CheckArm()
{
	Check(PlanArmDecide(PLANNER_OFF, false) == PLAN_ARM_OFF && PlanArmDecide(PLANNER_OFF, true) == PLAN_ARM_OFF,
	      "arm: off stays off");
	Check(PlanArmDecide(PLANNER_OBSERVE, false) == PLAN_ARM_GO && PlanArmDecide(PLANNER_OBSERVE, true) == PLAN_ARM_GO,
	      "arm: observe arms without playerHierarchical");
	Check(PlanArmDecide(PLANNER_ON, true) == PLAN_ARM_GO, "arm: on with playerHierarchical=on arms");
	Check(PlanArmDecide(PLANNER_ON, false) == PLAN_ARM_REFUSE_PREREQ, "arm: on without playerHierarchical=on refuses");
}

// ---- The water cost ----

static PlanWaterInputs Water(int mode, float land, float water, float engine)
{
	PlanWaterInputs in;
	memset(&in, 0, sizeof(in));
	in.mode = mode;
	in.readOk = 1;
	in.landSpeed = land;
	in.waterSpeed = water;
	in.raceWalkSpeed = 15.0f;
	in.swims = 1;
	in.engineValue = engine;
	return in;
}

static bool Near(float a, float b, float tol)
{
	return std::fabs(a - b) <= tol;
}

static void CheckWaterModes()
{
	PlanWaterInputs off = Water(PWC_OFF, 95.0f, 1.0f, 5.0f);
	PlanWaterInputs offUnread = off;
	offUnread.readOk = 0;
	CHECK(PlanWaterMultiplier(off) == 1.0f && PlanWaterMultiplier(offUnread) == 1.0f,
	      "water: off is 1 whatever the inputs");

	PlanWaterInputs eng = Water(PWC_ENGINE, 95.0f, 1.0f, 5.0f);
	PlanWaterInputs engLow = Water(PWC_ENGINE, 95.0f, 1.0f, 0.5f);
	PlanWaterInputs engZero = Water(PWC_ENGINE, 95.0f, 1.0f, 0.0f);
	CHECK(PlanWaterMultiplier(eng) == 5.0f && PlanWaterMultiplier(engLow) == 1.0f && PlanWaterMultiplier(engZero) == 1.0f,
	      "water: engine mode is the engine value floored at 1");

	CHECK(Near(PlanWaterMultiplier(Water(PWC_FLOOR, 80.0f, 7.9f, 5.0f)), 10.13f, 0.01f),
	      "water: floor takes the ratio when it exceeds the engine value");
	CHECK(PlanWaterMultiplier(Water(PWC_FLOOR, 95.0f, 25.0f, 5.0f)) == 5.0f,
	      "water: floor takes the engine value when it exceeds the ratio");
	CHECK(Near(PlanWaterMultiplier(Water(PWC_DYNAMIC, 95.0f, 25.0f, 5.0f)), 3.8f, 1e-4f),
	      "water: dynamic is the ratio alone");

	// A race that does not swim: W' = 2 * 15 = 30 whatever its swim speed reads; 95 / 30 = 3.1667.
	PlanWaterInputs robotDyn = Water(PWC_DYNAMIC, 95.0f, 999.0f, 5.0f);
	robotDyn.swims = 0;
	PlanWaterInputs robotFloor = robotDyn;
	robotFloor.mode = PWC_FLOOR;
	CHECK(Near(PlanWaterMultiplier(robotDyn), 3.1667f, 1e-3f) && PlanWaterMultiplier(robotFloor) == 5.0f,
	      "water: a race that does not swim walks the bottom at twice its walk speed");

	CHECK(PlanWaterMultiplier(Water(PWC_FLOOR, 95.0f, 1.0f, 5.0f)) == PLAN_WATER_CAP
	      && PlanWaterMultiplier(Water(PWC_DYNAMIC, 95.0f, 1.0f, 5.0f)) == 20.0f,
	      "water: the multiplier is capped at 20");

	bool floored = true;
	for (int mode = PWC_OFF; mode <= PWC_ENGINE; ++mode)
		floored = floored && PlanWaterMultiplier(Water(mode, 11.0f, 25.0f, 0.5f)) == 1.0f;
	CHECK(floored, "water: the multiplier is never below 1");

	PlanWaterInputs unreadFloor = Water(PWC_FLOOR, 95.0f, 1.0f, 5.0f);
	unreadFloor.readOk = 0;
	PlanWaterInputs unreadDyn = unreadFloor;
	unreadDyn.mode = PWC_DYNAMIC;
	CHECK(PlanWaterMultiplier(unreadFloor) == 5.0f && PlanWaterMultiplier(unreadDyn) == 5.0f,
	      "water: a failed read falls back to the engine value");

	// WALK: both speeds bounded by the walk speed 15; 15 / 13.6 = 1.1029.
	PlanWaterInputs walk = Water(PWC_DYNAMIC, 95.0f, 13.6f, 1.0f);
	walk.speedCap = 15.0f;
	CHECK(Near(PlanWaterMultiplier(walk), 1.1029f, 1e-3f), "water: the walk cap bounds both speeds");
}

static void CheckWaterGroup()
{
	// A runs slowly and swims well, B runs fast and swims badly: the squad runs at 70 and swims at 6.
	PlanWaterInputs pair[2] = { Water(PWC_DYNAMIC, 70.0f, 44.0f, 1.0f), Water(PWC_DYNAMIC, 120.0f, 6.0f, 1.0f) };
	float group = PlanWaterGroupMultiplier(pair, 2);
	float larger = PlanWaterMultiplier(pair[0]) > PlanWaterMultiplier(pair[1]) ? PlanWaterMultiplier(pair[0])
	                                                                          : PlanWaterMultiplier(pair[1]);
	CHECK(Near(group, 11.667f, 1e-3f) && larger == 20.0f,
	      "water: a run-together order plans on its slowest runner and slowest swimmer");

	PlanWaterInputs races[2] = { Water(PWC_FLOOR, 60.0f, 30.0f, 3.5f), Water(PWC_FLOOR, 60.0f, 30.0f, 5.5f) };
	CHECK(PlanWaterGroupMultiplier(races, 2) == 5.5f, "water: the group's engine floor is the largest member's");

	PlanWaterInputs failed[2] = { Water(PWC_DYNAMIC, 70.0f, 44.0f, 1.0f), Water(PWC_DYNAMIC, 10.0f, 2.0f, 1.0f) };
	failed[1].readOk = 0;
	CHECK(Near(PlanWaterGroupMultiplier(failed, 2), 1.5909f, 1e-3f),
	      "water: a member whose read failed is left out of the group's speeds");
}

static void CheckWaterArcs()
{
	CHECK(PlanWaterArcCost(123.4f, 1.0f, 255, 255) == 123.4f && PlanWaterArcCost(123.4f, 0.5f, 255, 0) == 123.4f,
	      "water: an arc costs its length at m 1");
	CHECK(Near(PlanWaterArcCost(100.0f, 5.0f, 255, 255), 500.0f, 1e-3f), "water: an all-water arc costs m times its length");
	CHECK(Near(PlanWaterArcCost(100.0f, 5.0f, 0, 255), 300.0f, 1e-3f),
	      "water: a half-wet arc costs its length times 1 + (m - 1) / 2");

	// Steps of 100 (bytes 0, 255: half wet) and 200 (255, 255: all wet): (50 + 200) / 300.
	const float centres[3][3] = { { 0.0f, 0.0f, 0.0f }, { 100.0f, 0.0f, 0.0f }, { 300.0f, 0.0f, 0.0f } };
	const int water[3] = { 0, 255, 255 };
	CHECK(Near(PlanRouteWaterShare(centres, water, 3), 0.8333f, 1e-3f) && PlanRouteWaterShare(centres, water, 1) == 0.0f,
	      "water: the route share weights each step by its two nodes");
}

static void CheckAcid()
{
	const float m = 5.0f;
	CHECK(PlanAcidArcCost(100.0f, m, 1.0f, 255, 255, 1, 1) == PlanWaterArcCost(100.0f, m, 255, 255)
	      && PlanAcidArcCost(123.4f, 2.5f, 1.0f, 17, 200, 1, 0) == PlanWaterArcCost(123.4f, 2.5f, 17, 200),
	      "acid: at a = 1 an arc costs exactly the water rule's cost");
	CHECK(PlanAcidArcCost(100.0f, m, 3.0f, 255, 255, 0, 0) == PlanWaterArcCost(100.0f, m, 255, 255),
	      "acid: an arc with neither end in an acid cell costs the water rule's cost");
	CHECK(Near(PlanAcidArcCost(100.0f, m, 3.0f, 255, 255, 1, 1), 1500.0f, 1e-2f),
	      "acid: an all-water arc in acid cells costs m times a times its length");
	CHECK(Near(PlanAcidArcCost(100.0f, m, 3.0f, 255, 255, 1, 0), 1000.0f, 1e-2f),
	      "acid: an arc with one end in an acid cell weighs that end's water at m times a");
	CHECK(Near(PlanAcidArcCost(100.0f, 1.0f, 3.0f, 255, 255, 1, 1), 300.0f, 1e-2f),
	      "acid: with water priced as land an acid swim still costs a times its length");
	CHECK(PlanAcidArcCost(100.0f, m, 3.0f, 0, 0, 1, 1) == 100.0f, "acid: a dry arc in an acid cell costs its length");
	CHECK(PlanAcidFactor(1, 3) == 1.0f && PlanAcidFactor(0, 3) == 3.0f && PlanAcidFactor(0, 1) == 1.0f,
	      "acid factor: an immune race reads 1, a non-immune one the acid cost");
	CHECK(PlanAcidFactor(0, 0) == 1.0f && PlanAcidFactor(0, 15) == 10.0f, "acid factor: the acid cost is held to 1..10");
	const float group[3] = { 1.0f, 3.0f, 1.0f };
	CHECK(PlanAcidGroupFactor(group, 3) == 3.0f && PlanAcidGroupFactor(group, 1) == 1.0f
	      && PlanAcidGroupFactor(group, 0) == 1.0f,
	      "acid group: the most vulnerable member sets the order's factor");
	int cx = -1, cy = -1;
	float x = 0.0f, z = 0.0f;
	PlanCellCentre(0, 0, &x, &z);
	PlanCellOf(x, z, &cx, &cy);
	bool corner = cx == 0 && cy == 0;
	PlanCellCentre(63, 17, &x, &z);
	PlanCellOf(x, z, &cx, &cy);
	CHECK(corner && cx == 63 && cy == 17, "cell centre: a cell's centre lies in that cell");
}

static void CheckWaterRequest()
{
	CHECK(PlanWaterRequestValue(PWC_OFF, 8.0f, 5.0f) == 0.0f, "water request: off leaves the engine's value");
	CHECK(PlanWaterRequestValue(PWC_DYNAMIC, 0.0f, 5.0f) == 0.0f && PlanWaterRequestValue(PWC_DYNAMIC, -1.0f, 5.0f) == 0.0f,
	      "water request: no multiplier leaves the engine's value");
	CHECK(PlanWaterRequestValue(PWC_DYNAMIC, 1.6f, 5.0f) == 1.6f, "water request: dynamic writes the character's multiplier");
	CHECK(PlanWaterRequestValue(PWC_FLOOR, 10.1f, 5.0f) == 10.1f,
	      "water request: under floor the request takes the floored multiplier");
	CHECK(PlanWaterRequestValue(PWC_FLOOR, 5.0f, 5.0f) == 0.0f, "water request: a multiplier equal to the engine's is left");
	float one = PlanWaterRequestValue(PWC_DYNAMIC, 1.0f, 5.0f);
	float half = PlanWaterRequestValue(PWC_DYNAMIC, 0.5f, 5.0f);
	CHECK(one != 1.0f && one == PLAN_WATER_REQ_MIN && half == PLAN_WATER_REQ_MIN && PLAN_WATER_REQ_MIN - 1.0f == FLT_EPSILON,
	      "water request: a multiplier at or below 1 is written just above 1, never 1");
	CHECK(PlanWaterRequestValue(PWC_DYNAMIC, 35.0f, 5.0f) == PLAN_WATER_CAP, "water request: the multiplier is capped at 20");
	CHECK(PlanWaterEffectiveMode(PWC_DYNAMIC, 0) == PWC_FLOOR,
	      "water mode: dynamic arms as floor while the engine write cannot run");
	CHECK(PlanWaterEffectiveMode(PWC_DYNAMIC, 1) == PWC_DYNAMIC, "water mode: dynamic arms as dynamic while the engine write runs");
	CHECK(PlanWaterEffectiveMode(PWC_FLOOR, 0) == PWC_FLOOR && PlanWaterEffectiveMode(PWC_ENGINE, 1) == PWC_ENGINE
	      && PlanWaterEffectiveMode(PWC_OFF, 0) == PWC_OFF,
	      "water mode: floor, engine and off arm as configured");
}

// A route east along the row whose far sections are real exterior indices: leg 0's portal into cell
// (31, 30), leg 1's into (32, 30), the destination in (33, 30), two cells past leg 0's far side.
static void BuildCellRoute(PlanLeg legs[3])
{
	MakeLeg(&legs[0], -4600.0f, ROW_Z, 30 * 64 + 31, 0);
	MakeLeg(&legs[1], 10.0f, ROW_Z, 30 * 64 + 32, 0);
	MakeLeg(&legs[2], 4700.0f, ROW_Z, 30 * 64 + 33, 1);
}

// An interior goal's legs: the portal into cell (31, 30), the portal into the building's interior
// (directory index 4103, past the 4096 exterior slots) 300 units on, and the click inside it.
static void BuildInteriorRoute(PlanLeg legs[3])
{
	MakeLeg(&legs[0], -4600.0f, ROW_Z, 30 * 64 + 31, 0);
	MakeLeg(&legs[1], -4300.0f, ROW_Z, 4096 + 7, 0);
	MakeLeg(&legs[2], -4240.0f, ROW_Z, 4096 + 7, 1);
}

static void CheckInteriorHold()
{
	PlanLeg legs[3];
	BuildInteriorRoute(legs);
	Check(PlanLegTarget(legs, 3, 0x7u, 0, 30, 30, 2, 4096, 1) == 1,
	      "interior hold: the scan stops at the leg into the interior");
	Check(PlanDecideVerdict(true, 3, 0x7u, 1, 2, 1) == PV_LEGGED,
	      "interior hold: a held plan's verdict stays legged at span 1");
	Check(PlanLegTarget(legs, 3, 0x7u, 0, 30, 30, 2, 4096, 0) == 2 && PlanDecideVerdict(true, 3, 0x7u, 1, 2, 0) == PV_DIRECT,
	      "interior hold: without the hold the click is the target and the verdict direct");
	PlanLeg route[3];
	BuildCellRoute(route);
	Check(PlanLegTarget(route, 3, 0x7u, 0, 32, 30, 2, 4096, 1) == 2 && PlanLegTarget(route, 3, 0x7u, 0, 32, 30, 2, 4096, 0) == 2,
	      "interior hold: an exterior goal's legs answer as without it");
	Check(PlanLegTarget(legs, 3, 0x7u, 2, 31, 30, 2, 4096, 1) == 2,
	      "interior hold: past the interior portal the click is the target");
	Check(PlanLegTarget(legs, 3, 0x5u, 0, 30, 30, 2, 4096, 1) == 0,
	      "interior hold: an unloaded interior portal ends the run before it");
	Check(PlanHoldInteriorPortal(1, 4103, 4096) == 1 && PlanHoldInteriorPortal(0, 4103, 4096) == 0
	      && PlanHoldInteriorPortal(1, 30 * 64 + 31, 4096) == 0,
	      "interior hold: only an outdoors order with an interior goal holds");

	float atLeg0[3], atLeg1[3];
	Set3(atLeg0, -4590.0f, 50.0f, ROW_Z + 5.0f);
	Set3(atLeg1, -4295.0f, 50.0f, ROW_Z);
	PlanEdgeIn in;
	PlanEdgeOut out;
	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg0, 0, 0x7u);
	in.exteriorSlots = 4096;
	in.holdInteriorPortal = 1;
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 1 && Same3(out.point, legs[1].point),
	      "interior hold: the recheck at the exterior portal advances to the interior portal, not the click");
	EdgeIn(&in, PES_RECHECK, 0.0f, atLeg1, 1, 0x7u);
	in.exteriorSlots = 4096;
	in.holdInteriorPortal = 1;
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 2 && Same3(out.point, legs[2].point),
	      "interior hold: the recheck at the interior portal advances onto the click");
}

static void SectionIn(PlanEdgeIn* in, const float pos[3], unsigned mask, int section, int aim)
{
	EdgeIn(in, PES_RECHECK, 0.0f, pos, 0, mask);
	in->advanceSection = section;
	in->aim = aim;
	in->exteriorSlots = 4096;
}

static void CheckSectionAdvance()
{
	PlanLeg legs[3];
	BuildCellRoute(legs);
	float inFar[3];
	Set3(inFar, -4500.0f, 50.0f, ROW_Z + 30.0f);   // past leg 0's portal, in cell (31, 30), 104 units off
	PlanEdgeIn in;
	PlanEdgeOut out;

	SectionIn(&in, inFar, 0x7u, 1, 0);
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 1 && out.bySection == 1 && out.waiting == 0 && Same3(out.point, legs[1].point),
	      "section: a start in the far section's cell advances");
	SectionIn(&in, inFar, 0x0u, 1, 0);
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 0 && out.waiting == 0 && Same3(out.point, legs[0].point),
	      "section: an unloaded far section keeps the portal, not waiting");
	PlanLeg inner[3];
	BuildCellRoute(inner);
	inner[0].farSection = 4096 + 5;
	SectionIn(&in, inFar, 0x7u, 1, 0);
	PlanEdgeStep(inner, 3, in, &out);
	Check(out.newLegIndex == 0 && out.bySection == 0, "section: an interior far section keeps the reach rule");
	SectionIn(&in, inFar, 0x7u, 0, 0);
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 0 && out.bySection == 0 && Same3(out.point, legs[0].point),
	      "section: with the key off the reach rule alone decides");
	float atLeg0[3];
	Set3(atLeg0, -4590.0f, 50.0f, ROW_Z + 5.0f);
	SectionIn(&in, atLeg0, 0x7u, 1, 0);
	PlanEdgeStep(legs, 3, in, &out);
	Check(out.newLegIndex == 1 && out.bySection == 0, "section: an arrival within the reach is not counted as an entry");
	Check(PlanInFarCell(legs[0], inFar, 4096) && !PlanInFarCell(legs[1], inFar, 4096),
	      "section: the far cell is the directory index of the start's cell");
}

// Leg 0's edge runs 80 units along z at x = -4600 (edgeA y 40 at z - 40, edgeB y 60 at z + 40).
static void CheckAim()
{
	PlanLeg legs[2];
	MakeLeg(&legs[0], -4600.0f, ROW_Z, 30 * 64 + 31, 0);
	MakeLeg(&legs[1], -4500.0f, ROW_Z - 80.0f / 3.0f, 30 * 64 + 31, 1);
	const float aimY = 40.0f + 20.0f / 3.0f, aimZ = ROW_Z - 40.0f / 3.0f;
	float start[3];
	Set3(start, -4700.0f, 50.0f, ROW_Z);
	float out[3];
	Check(PlanLegAim(legs[0], start, legs[1].point, out) && Near3(out, -4600.0f, aimY, aimZ),
	      "aim: a line crossing the edge at its third aims there");
	float far[3];
	Set3(far, -4500.0f, 50.0f, ROW_Z - 200.0f);
	Check(PlanLegAim(legs[0], start, far, out) && Near3(out, -4600.0f, 41.25f, ROW_Z - 35.0f),
	      "aim: a crossing beyond the inset clamps to it");
	float along[3];
	Set3(along, -4650.0f, 50.0f, ROW_Z - 20.0f / 3.0f);
	Check(PlanLegAim(legs[0], along, legs[1].point, out) && Near3(out, -4600.0f, aimY, aimZ),
	      "aim: a start moved along the aim line keeps the aimed point");
	Check(!PlanLegAim(legs[1], start, far, out) && Same3(out, legs[1].point), "aim: the destination leg is never aimed");

	PlanEdgeIn in;
	PlanEdgeOut rc, cp;
	EdgeIn(&in, PES_COMPUTE, 0.0f, start, 0, 0x3u);
	in.aim = 1;
	PlanEdgeStep(legs, 2, in, &cp);
	Check(cp.aimed == 1 && Near3(cp.point, -4600.0f, aimY, aimZ) && cp.aimShift > 13.3f && cp.aimShift < 13.4f,
	      "aim: the recompute returns the aimed point and its shift");
	EdgeIn(&in, PES_RECHECK, 0.0f, start, 0, 0x3u);
	in.aim = 1;
	PlanEdgeStep(legs, 2, in, &rc);
	Check(rc.newLegIndex == 0 && Same3(rc.point, cp.point), "aim: the recheck and the recompute of one frame agree");
	EdgeIn(&in, PES_COMPUTE, 10.0f, start, 0, 0x3u);
	in.aim = 1;
	PlanEdgeStep(legs, 2, in, &cp);
	Check(cp.rung == 1 && Near3(cp.point, -4600.0f, 40.0f + 20.0f * (80.0f / 3.0f + 10.0f) / 80.0f, aimZ + 10.0f),
	      "aim: a rung slides from the aimed point");
	EdgeIn(&in, PES_COMPUTE, 0.0f, start, 0, 0x3u);
	in.aim = 1;
	in.routeTruncated = 1;
	PlanEdgeStep(legs, 2, in, &cp);
	Check(cp.aimed == 0 && Same3(cp.point, legs[0].point), "aim: a truncated plan's last portal keeps the midpoint");
	EdgeIn(&in, PES_COMPUTE, 0.0f, start, 0, 0x3u);
	PlanEdgeStep(legs, 2, in, &cp);
	Check(cp.aimed == 0 && Same3(cp.point, legs[0].point), "aim: with the key off the midpoint stays");

	float onEdge[3];
	Set3(onEdge, -4601.0f, 50.0f, ROW_Z - 30.0f);   // 1 unit from the edge, 30 from the midpoint
	Check(PlanDistToPortal(legs[0], onEdge, 1) < 1.01f && PlanDistToPortal(legs[0], onEdge, 0) > 30.0f,
	      "aim: the portal's reach is the edge with the aim on and the midpoint with it off");
	EdgeIn(&in, PES_RECHECK, 0.0f, onEdge, 0, 0x3u);
	in.aim = 1;
	PlanEdgeStep(legs, 2, in, &rc);
	Check(rc.newLegIndex == 1, "aim: a character at the aimed edge point arrives");

	PlanLeg route[3];
	BuildCellRoute(route);
	float inFar[3];
	Set3(inFar, -4500.0f, 50.0f, ROW_Z + 30.0f);
	SectionIn(&in, inFar, 0x7u, 1, 1);
	PlanEdgeStep(route, 3, in, &rc);
	float aimed1[3];
	bool aimOk = PlanLegAim(route[1], inFar, route[2].point, aimed1);
	Check(rc.newLegIndex == 1 && rc.bySection == 1 && aimOk && Same3(rc.point, aimed1),
	      "section and aim: an entry advances to the next portal's aimed point");
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
	CheckComplete();
	CheckFeed();
	CheckArm();
	CheckWaterModes();
	CheckWaterGroup();
	CheckWaterArcs();
	CheckWaterRequest();
	CheckAcid();
	CheckSectionAdvance();
	CheckAim();
	CheckInteriorHold();
	return CheckExit("plan_policy_units");
}
