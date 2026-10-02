// plan_policy.cpp - The route planner's decisions as pure functions of their inputs: no state, no
// lock, no allocation, any thread.

#include <cmath>
#include <float.h>
#include "planner/plan_policy.h"

namespace planner {

static const float GRID_ORIGIN = 147456.0f;
static const float GRID_CELL   = 4608.0f;

static const double ARRIVAL_TIMEOUT_SECONDS = 8.0;
static const double PLAN_AGE_SECONDS        = 120.0;
static const int    RUNGS_BEFORE_REPLAN     = 3;

static bool LegLoaded(unsigned loadedMask, int i)
{
	return i >= 0 && i < PLAN_MAX_LEGS && ((loadedMask >> i) & 1u) != 0;
}

static float DistanceXz(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dz * dz);
}

static void Copy3(float out[3], const float in[3])
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
}

void PlanCellOf(float x, float z, int* cx, int* cy)
{
	*cx = (int)std::floor((x + GRID_ORIGIN) / GRID_CELL);
	*cy = (int)std::floor((z + GRID_ORIGIN) / GRID_CELL);
}

int PlanCellSpan(int ax, int ay, int bx, int by)
{
	int sx = ax - bx, sy = ay - by;
	if (sx < 0) sx = -sx;
	if (sy < 0) sy = -sy;
	return (sx > sy) ? sx : sy;
}

PlanVerdict PlanDecideVerdict(bool routeFound, int legCount, unsigned loadedMask, int span, int legSpan)
{
	if (!routeFound) return PV_NO_ROUTE;
	if (legCount < 0) legCount = 0;
	unsigned all = (legCount >= 32) ? 0xFFFFFFFFu : ((1u << legCount) - 1u);
	if ((loadedMask & all) == all && span < legSpan) return PV_DIRECT;
	return PV_LEGGED;
}

// The scan walks the route from leg `from` while each leg's far section is loaded and its cell is
// inside the bound: an unloaded leg or one past the bound ends the run, so the target is never
// beyond a stretch the character cannot walk or a leg that leaves the bounded span. A portal leg
// with no qualifying run is still the next step; the destination leg is never a fallback.
int PlanLegTarget(const PlanLeg* legs, int n, unsigned loadedMask, int from, int cx, int cy, int legSpan)
{
	if (n > PLAN_MAX_LEGS) n = PLAN_MAX_LEGS;
	if (!legs || from < 0 || from >= n) return -1;
	int target = -1;
	for (int i = from; i < n; ++i)
	{
		if (!LegLoaded(loadedMask, i)) break;
		if (!(PlanCellSpan(legs[i].cellX, legs[i].cellY, cx, cy) <= legSpan - 1)) break;
		target = i;
	}
	if (target >= 0) return target;
	return legs[from].isDestination ? -1 : from;
}

static void EdgeRecheck(const PlanLeg* legs, int n, const PlanEdgeIn& in, PlanEdgeOut* out)
{
	const PlanLeg& cur = legs[in.legIndex];
	Copy3(out->point, cur.point);
	if (!(DistanceXz(in.pos, cur.point) < PLAN_REACH) || cur.isDestination) return;

	// Past the current portal only once its far section is in: until then the character holds it.
	int target = -1;
	if (LegLoaded(in.loadedMask, in.legIndex))
	{
		int cx, cy;
		PlanCellOf(in.pos[0], in.pos[2], &cx, &cy);
		target = PlanLegTarget(legs, n, in.loadedMask, in.legIndex + 1, cx, cy, in.legSpan);
		// A truncated plan's destination leg is not the order's end: hold its last portal instead.
		if (in.routeTruncated && target >= 0 && legs[target].isDestination) target -= 1;
	}
	if (target > in.legIndex)
	{
		out->newLegIndex = target;
		Copy3(out->point, legs[target].point);
		return;
	}
	out->waiting = 1;
}

void PlanEdgeStep(const PlanLeg* legs, int n, const PlanEdgeIn& in, PlanEdgeOut* out)
{
	out->action      = PEA_PASS;
	out->newLegIndex = in.legIndex;
	out->waiting     = 0;
	out->rung        = 0;
	out->point[0] = out->point[1] = out->point[2] = 0.0f;
	if (n > PLAN_MAX_LEGS) n = PLAN_MAX_LEGS;
	if (!legs || in.legIndex < 0 || in.legIndex >= n) return;
	if (in.site != PES_RECHECK && in.site != PES_COMPUTE) return;

	out->action = PEA_POINT;
	if (in.site == PES_RECHECK)
	{
		EdgeRecheck(legs, n, in, out);
		return;
	}
	const PlanLeg& cur = legs[in.legIndex];
	if (in.offset != 0.0f)
	{
		out->rung = 1;
		if (!cur.isDestination)
		{
			PlanRungSlide(cur, in.offset, out->point);
			return;
		}
	}
	Copy3(out->point, cur.point);
}

void PlanRungSlide(const PlanLeg& leg, float offset, float out[3])
{
	const float* a = leg.edgeA;
	const float* b = leg.edgeB;
	float dx = b[0] - a[0], dz = b[2] - a[2];
	float len = std::sqrt(dx * dx + dz * dz);
	if (!(len >= 2.0f * PLAN_RUNG_INSET))
	{
		out[0] = (a[0] + b[0]) * 0.5f;
		out[1] = (a[1] + b[1]) * 0.5f;
		out[2] = (a[2] + b[2]) * 0.5f;
		return;
	}
	float ux = dx / len, uz = dz / len;
	float t = (leg.point[0] - a[0]) * ux + (leg.point[2] - a[2]) * uz + offset;
	if (!(t >= PLAN_RUNG_INSET)) t = PLAN_RUNG_INSET;
	else if (t > len - PLAN_RUNG_INSET) t = len - PLAN_RUNG_INSET;
	out[0] = a[0] + ux * t;
	out[1] = a[1] + (b[1] - a[1]) * (t / len);
	out[2] = a[2] + uz * t;
}

PlanFlipAnswer PlanFlipRule(const PlanFlipIn& in)
{
	if (in.mode != PLANNER_ON) return PFA_NOT_MINE;
	return PlanFlipRuleOn(in);
}

PlanFlipAnswer PlanFlipRuleOn(const PlanFlipIn& in)
{
	if (!in.haveChar || !in.haveSlot) return PFA_NOT_MINE;
	if (in.verdict != PV_DIRECT && in.verdict != PV_LEGGED) return PFA_NOT_MINE;
	if (!PlanDestIsPlans(in.dest, in.finalDest, in.resend, in.resendCount)) return PFA_NOT_MINE;
	if (in.verdict == PV_LEGGED && !in.legIsDestination) return PFA_FALSE;
	return PFA_VANILLA;
}

bool PlanDestMatches(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz <= PLAN_DEST_MATCH * PLAN_DEST_MATCH;
}

bool PlanDestIsPlans(const float dest[3], const float finalDest[3], const float resend[][3], int resendCount)
{
	if (PlanDestMatches(dest, finalDest)) return true;
	if (!resend || resendCount < 0) resendCount = 0;
	if (resendCount > PLAN_RESEND_POINTS) resendCount = PLAN_RESEND_POINTS;
	for (int i = 0; i < resendCount; ++i)
		if (PlanDestMatches(dest, resend[i])) return true;
	return false;
}

bool PlanDestIsPlansXz(const float dest[3], const float finalDest[3], const float resend[][3], int resendCount)
{
	if (DistanceXz(dest, finalDest) <= PLAN_DEST_MATCH) return true;
	if (!resend || resendCount < 0) resendCount = 0;
	if (resendCount > PLAN_RESEND_POINTS) resendCount = PLAN_RESEND_POINTS;
	for (int i = 0; i < resendCount; ++i)
		if (DistanceXz(dest, resend[i]) <= PLAN_DEST_MATCH) return true;
	return false;
}

bool PlanEdgeSteers(int mode, int verdict, bool destMatches)
{
	return mode == PLANNER_ON && verdict == PV_LEGGED && destMatches;
}

bool PlanReplacesAhead(int mode, int verdict)
{
	return mode == PLANNER_ON && verdict == PV_LEGGED;
}

bool PlanOwnsWait(int mode, int verdict, int legIsDestination, int waiting, float distToPortal,
                  float wpToPortal, float posToWp, bool destIsPlans)
{
	if (mode != PLANNER_ON || verdict != PV_LEGGED || legIsDestination) return false;
	if (!destIsPlans) return false;
	if (waiting != 0 && distToPortal < PLAN_REACH) return true;
	return wpToPortal < PLAN_REACH && posToWp < PLAN_REACH;
}

bool PlanLegComplete(int portalLeg, float distToPortal, int pathState, int characterState)
{
	return portalLeg && distToPortal > PLAN_REACH && pathState == PLAN_PATH_COMPLETE && characterState >= 0 && characterState <= PLAN_CHAR_GOAL_REACHED;
}

float PlanSnapDistance(const float raw[3], const float snapped[3])
{
	return DistanceXz(raw, snapped);
}

PlanReplanWhy PlanReplanDue(const PlanReplanIn& in)
{
	bool nearPortal = in.distToPortal < PLAN_REACH;
	if (in.rungs >= RUNGS_BEFORE_REPLAN) return PRW_RUNGS;
	if (in.completeSince > 0.0 && in.now - in.completeSince >= ARRIVAL_TIMEOUT_SECONDS && !nearPortal)
		return PRW_ARRIVAL_TIMEOUT;
	if (in.routeTruncated && in.legIndex == in.legCount - 2 && nearPortal) return PRW_ROUTE_END;
	if (in.waitSince > 0.0 && in.now - in.waitSince >= (double)in.waitSeconds && !in.awaitedLoaded &&
	    in.loadedChangedSincePlan)
		return PRW_WAIT;
	if (in.goalByFootprint && in.goalLoaded) return PRW_GOAL_LOADED;
	if (in.now - in.planTime >= PLAN_AGE_SECONDS) return PRW_AGE;
	return PRW_NONE;
}

int PlanFeedCells(const PlanLeg* legs, int n, int from, int ahead, int exteriorSlots, int* outXY)
{
	if (n > PLAN_MAX_LEGS) n = PLAN_MAX_LEGS;
	if (!legs || !outXY || ahead <= 0) return 0;
	bool haveFrom = from >= 0 && from < n;
	int count = 0;
	for (int i = (from + 1 > 0) ? from + 1 : 0; i < n && count < ahead; ++i)
	{
		const PlanLeg& leg = legs[i];
		if (leg.farSection >= exteriorSlots) continue;
		if (haveFrom && leg.cellX == legs[from].cellX && leg.cellY == legs[from].cellY) continue;
		bool seen = false;
		for (int k = 0; k < count && !seen; ++k)
			seen = outXY[k * 2] == leg.cellX && outXY[k * 2 + 1] == leg.cellY;
		if (seen) continue;
		outXY[count * 2]     = leg.cellX;
		outXY[count * 2 + 1] = leg.cellY;
		++count;
	}
	return count;
}

PlanArm PlanArmDecide(int mode, bool playerHierarchicalOn)
{
	if (mode == PLANNER_OBSERVE) return PLAN_ARM_GO;
	if (mode == PLANNER_ON) return playerHierarchicalOn ? PLAN_ARM_GO : PLAN_ARM_REFUSE_PREREQ;
	return PLAN_ARM_OFF;
}

static bool PositiveFinite(float v)
{
	return _finite(v) != 0 && v > 0.0f;
}

static float EngineTerm(float engineValue)
{
	return PositiveFinite(engineValue) ? engineValue : 1.0f;
}

static float WaterSpeedOf(const PlanWaterInputs& in)
{
	return in.swims ? in.waterSpeed : PLAN_BOTTOM_WALK * in.raceWalkSpeed;
}

// The mode formula over a ratio r (0: unknown) and an engine term e, at least 1 and at most the cap.
static float WaterModeValue(int mode, float r, float e)
{
	float m = 1.0f;
	if (mode == PWC_OFF)
		return 1.0f;
	if (mode == PWC_ENGINE)
		m = e;
	else if (mode == PWC_DYNAMIC)
		m = r > 0.0f ? r : e;
	else
		m = r > e ? r : e;
	if (!(m > 1.0f))
		m = 1.0f;
	return m < PLAN_WATER_CAP ? m : PLAN_WATER_CAP;
}

float PlanWaterRatio(const PlanWaterInputs& in)
{
	float land = in.landSpeed;
	float water = WaterSpeedOf(in);
	if (!in.readOk || !PositiveFinite(land) || !PositiveFinite(water))
		return 0.0f;
	if (in.speedCap > 0.0f)
	{
		land = land < in.speedCap ? land : in.speedCap;
		water = water < in.speedCap ? water : in.speedCap;
	}
	return land / water;
}

float PlanWaterMultiplier(const PlanWaterInputs& in)
{
	return WaterModeValue(in.mode, PlanWaterRatio(in), EngineTerm(in.engineValue));
}

float PlanWaterGroupMultiplier(const PlanWaterInputs* members, int n)
{
	if (!members || n <= 0)
		return 1.0f;
	float land = 0.0f, water = 0.0f, e = 1.0f;
	bool read = false;
	for (int k = 0; k < n; ++k)
	{
		const PlanWaterInputs& in = members[k];
		float ek = EngineTerm(in.engineValue);
		e = ek > e ? ek : e;
		float wk = WaterSpeedOf(in);
		if (!in.readOk || !PositiveFinite(in.landSpeed) || !PositiveFinite(wk))
			continue;
		land = !read || in.landSpeed < land ? in.landSpeed : land;
		water = !read || wk < water ? wk : water;
		read = true;
	}
	return WaterModeValue(members[0].mode, read ? land / water : 0.0f, e);
}

float PlanWaterArcCost(float cost, float m, int wFrom, int wTo)
{
	if (m <= 1.0f)
		return cost;
	return cost * (1.0f + (m - 1.0f) * (float)(wFrom + wTo) / 510.0f);
}

float PlanRouteWaterShare(const float (*centres)[3], const int* water, int n)
{
	if (!centres || !water || n < 2)
		return 0.0f;
	double length = 0.0, wet = 0.0;
	for (int i = 0; i + 1 < n; ++i)
	{
		double dx = centres[i + 1][0] - centres[i][0];
		double dy = centres[i + 1][1] - centres[i][1];
		double dz = centres[i + 1][2] - centres[i][2];
		double step = std::sqrt(dx * dx + dy * dy + dz * dz);
		length += step;
		wet += step * (double)(water[i] + water[i + 1]) / 510.0;
	}
	return length > 0.0 ? (float)(wet / length) : 0.0f;
}

} // namespace planner
