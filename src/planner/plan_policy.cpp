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

void PlanCellCentre(int cx, int cy, float* x, float* z)
{
	*x = ((float)cx + 0.5f) * GRID_CELL - GRID_ORIGIN;
	*z = ((float)cy + 0.5f) * GRID_CELL - GRID_ORIGIN;
}

int PlanCellSpan(int ax, int ay, int bx, int by)
{
	int sx = ax - bx, sy = ay - by;
	if (sx < 0) sx = -sx;
	if (sy < 0) sy = -sy;
	return (sx > sy) ? sx : sy;
}

PlanVerdict PlanDecideVerdict(bool routeFound, int legCount, unsigned loadedMask, int span, int legSpan,
                              int holdInteriorPortal)
{
	if (!routeFound) return PV_NO_ROUTE;
	if (holdInteriorPortal) return PV_LEGGED;
	if (legCount < 0) legCount = 0;
	unsigned all = (legCount >= 32) ? 0xFFFFFFFFu : ((1u << legCount) - 1u);
	if ((loadedMask & all) == all && span < legSpan) return PV_DIRECT;
	return PV_LEGGED;
}

// The scan walks the route from leg `from` while each leg's far section is loaded and its cell is
// inside the bound: an unloaded leg or one past the bound ends the run, so the target is never
// beyond a stretch the character cannot walk or a leg that leaves the bounded span. A held interior
// goal's run also ends at the leg into the interior, so the engine is never handed the goal from
// outside the building's portal. A portal leg with no qualifying run is still the next step; the
// destination leg is never a fallback.
int PlanLegTarget(const PlanLeg* legs, int n, unsigned loadedMask, int from, int cx, int cy, int legSpan,
                  int exteriorSlots, int holdInteriorPortal)
{
	if (n > PLAN_MAX_LEGS) n = PLAN_MAX_LEGS;
	if (!legs || from < 0 || from >= n) return -1;
	int target = -1;
	for (int i = from; i < n; ++i)
	{
		if (!LegLoaded(loadedMask, i)) break;
		if (!(PlanCellSpan(legs[i].cellX, legs[i].cellY, cx, cy) <= legSpan - 1)) break;
		target = i;
		if (holdInteriorPortal && !legs[i].isDestination && legs[i].farSection >= exteriorSlots) break;
	}
	if (target >= 0) return target;
	return legs[from].isDestination ? -1 : from;
}

int PlanHoldInteriorPortal(int orderOutdoors, int goalDir, int exteriorSlots)
{
	return (orderOutdoors && goalDir >= exteriorSlots) ? 1 : 0;
}

// The rung slide from an explicit base point (PlanRungSlide's is the leg's point).
static void SlideFrom(const PlanLeg& leg, const float base[3], float offset, float out[3]);

// Leg i's next point for the aim: the following leg's point; none past the last leg or onto a
// truncated plan's destination leg.
static bool LegNextPoint(const PlanLeg* legs, int n, int i, int routeTruncated, float out[3])
{
	if (i + 1 >= n || (routeTruncated && legs[i + 1].isDestination))
		return false;
	Copy3(out, legs[i + 1].point);
	return true;
}

// The point leg i yields for a call from in.pos: its aimed point while the aim is on and it has a
// next point, else its point.
static void LegPoint(const PlanLeg* legs, int n, int i, const PlanEdgeIn& in, PlanEdgeOut* out, float pt[3])
{
	Copy3(pt, legs[i].point);
	out->aimed = 0;
	out->aimShift = 0.0f;
	float next[3];
	if (!in.aim || legs[i].isDestination || !LegNextPoint(legs, n, i, in.routeTruncated, next))
		return;
	if (PlanLegAim(legs[i], in.pos, next, pt))
	{
		out->aimed = 1;
		out->aimShift = DistanceXz(pt, legs[i].point);
	}
}

static void EdgeRecheck(const PlanLeg* legs, int n, const PlanEdgeIn& in, PlanEdgeOut* out)
{
	const PlanLeg& cur = legs[in.legIndex];
	LegPoint(legs, n, in.legIndex, in, out, out->point);
	if (cur.isDestination) return;
	if (!(PlanDistToPortal(cur, in.pos, in.aim) < PLAN_REACH)) return;

	// Past the current portal only once its far section is in: until then the character holds it.
	int target = -1;
	if (LegLoaded(in.loadedMask, in.legIndex))
	{
		int cx, cy;
		PlanCellOf(in.pos[0], in.pos[2], &cx, &cy);
		target = PlanLegTarget(legs, n, in.loadedMask, in.legIndex + 1, cx, cy, in.legSpan, in.exteriorSlots,
		                       in.holdInteriorPortal);
		// A truncated plan's destination leg is not the order's end: hold its last portal instead.
		if (in.routeTruncated && target >= 0 && legs[target].isDestination) target -= 1;
	}
	if (target > in.legIndex)
	{
		out->newLegIndex = target;
		LegPoint(legs, n, target, in, out, out->point);
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
	out->aimed       = 0;
	out->aimShift    = 0.0f;
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
	float base[3];
	LegPoint(legs, n, in.legIndex, in, out, base);
	if (in.offset != 0.0f)
	{
		out->rung = 1;
		if (!cur.isDestination)
		{
			SlideFrom(cur, base, in.offset, out->point);
			return;
		}
	}
	Copy3(out->point, base);
}

static void SlideFrom(const PlanLeg& leg, const float base[3], float offset, float out[3])
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
	float t = (base[0] - a[0]) * ux + (base[2] - a[2]) * uz + offset;
	if (!(t >= PLAN_RUNG_INSET)) t = PLAN_RUNG_INSET;
	else if (t > len - PLAN_RUNG_INSET) t = len - PLAN_RUNG_INSET;
	out[0] = a[0] + ux * t;
	out[1] = a[1] + (b[1] - a[1]) * (t / len);
	out[2] = a[2] + uz * t;
}

void PlanRungSlide(const PlanLeg& leg, float offset, float out[3])
{
	SlideFrom(leg, leg.point, offset, out);
}

bool PlanLegAim(const PlanLeg& leg, const float start[3], const float next[3], float out[3])
{
	Copy3(out, leg.point);
	if (leg.isDestination)
		return false;
	const float* a = leg.edgeA;
	const float* b = leg.edgeB;
	float ex = b[0] - a[0], ez = b[2] - a[2];
	float len = std::sqrt(ex * ex + ez * ez);
	if (!(len >= 2.0f * PLAN_RUNG_INSET))
		return false;
	float ux = ex / len, uz = ez / len;
	float dx = next[0] - start[0], dz = next[2] - start[2];
	float dl = std::sqrt(dx * dx + dz * dz);
	// The line start + s * d meets a + t * u where t * (u x d) = (start - a) x d, with
	// v x w = v.x * w.z - v.z * w.x.
	float denom = ux * dz - uz * dx;
	if (!(dl > 0.0f) || !(std::fabs(denom) > 1e-4f * dl))
		return false;
	float t = ((start[0] - a[0]) * dz - (start[2] - a[2]) * dx) / denom;
	if (!(t >= PLAN_RUNG_INSET)) t = PLAN_RUNG_INSET;
	else if (t > len - PLAN_RUNG_INSET) t = len - PLAN_RUNG_INSET;
	out[0] = a[0] + ux * t;
	out[1] = a[1] + (b[1] - a[1]) * (t / len);
	out[2] = a[2] + uz * t;
	return true;
}

float PlanDistToPortal(const PlanLeg& leg, const float pos[3], int edgeAware)
{
	if (!edgeAware || leg.isDestination)
		return DistanceXz(pos, leg.point);
	float ex = leg.edgeB[0] - leg.edgeA[0], ez = leg.edgeB[2] - leg.edgeA[2];
	float l2 = ex * ex + ez * ez;
	if (!(l2 > 0.0f))
		return DistanceXz(pos, leg.point);
	float t = ((pos[0] - leg.edgeA[0]) * ex + (pos[2] - leg.edgeA[2]) * ez) / l2;
	if (!(t >= 0.0f)) t = 0.0f;
	else if (t > 1.0f) t = 1.0f;
	float q[3] = { leg.edgeA[0] + ex * t, 0.0f, leg.edgeA[2] + ez * t };
	return DistanceXz(pos, q);
}

// The engine's parked recheck at in.pos, through PlanEdgeStep, so the pre-arrival request and the
// recheck at the portal never answer differently. The NOT_MINE test is the recheck's own reach.
void PlanPreArrival(const PlanLeg* legs, int n, const PlanEdgeIn& in, PlanPreOut* out)
{
	out->target = -1;
	out->skip = PPS_NOT_MINE;
	out->point[0] = out->point[1] = out->point[2] = 0.0f;
	if (n > PLAN_MAX_LEGS) n = PLAN_MAX_LEGS;
	if (!legs || in.legIndex < 0 || in.legIndex >= n) return;
	const PlanLeg& cur = legs[in.legIndex];
	if (cur.isDestination || !(PlanDistToPortal(cur, in.pos, in.aim) < PLAN_REACH)) return;
	PlanEdgeIn at = in;
	at.site = PES_RECHECK;
	at.offset = 0.0f;
	PlanEdgeOut rc;
	PlanEdgeStep(legs, n, at, &rc);
	if (rc.newLegIndex <= in.legIndex)
	{
		out->skip = PPS_WAIT;
		return;
	}
	if (in.holdInteriorPortal && legs[rc.newLegIndex].isDestination)
	{
		out->skip = PPS_HELD;
		return;
	}
	out->target = rc.newLegIndex;
	Copy3(out->point, rc.point);
	out->skip = PPS_NONE;
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
                  float wpToPortal, float posToWp, bool destIsPlans, int preInFlight, float posToPrePortal)
{
	if (mode != PLANNER_ON || verdict != PV_LEGGED) return false;
	if (!destIsPlans) return false;
	if (preInFlight && posToPrePortal < PLAN_REACH) return true;
	if (legIsDestination) return false;
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

float PlanWaterRequestValue(int mode, float m, float engineValue)
{
	if (mode == PWC_OFF || !(m > 0.0f))
		return 0.0f;
	float v = m > PLAN_WATER_CAP ? PLAN_WATER_CAP : m;
	if (v < PLAN_WATER_REQ_MIN)
		v = PLAN_WATER_REQ_MIN;
	return v == engineValue ? 0.0f : v;
}

int PlanWaterEffectiveMode(int configured, int engineLive)
{
	return (configured == PWC_DYNAMIC && !engineLive) ? PWC_FLOOR : configured;
}

float PlanAcidArcCost(float cost, float m, float a, int wFrom, int wTo, int acidFrom, int acidTo)
{
	if (!(a > 1.0f) || (!acidFrom && !acidTo))
		return PlanWaterArcCost(cost, m, wFrom, wTo);
	float tFrom = m * (acidFrom ? a : 1.0f) - 1.0f;
	float tTo = m * (acidTo ? a : 1.0f) - 1.0f;
	if (tFrom < 0.0f)
		tFrom = 0.0f;
	if (tTo < 0.0f)
		tTo = 0.0f;
	return cost * (1.0f + (tFrom * (float)wFrom + tTo * (float)wTo) / 510.0f);
}

float PlanAcidFactor(int immune, int acidCost)
{
	if (immune)
		return 1.0f;
	int c = acidCost < 1 ? 1 : (acidCost > PLAN_ACID_COST_MAX ? PLAN_ACID_COST_MAX : acidCost);
	return (float)c;
}

float PlanAcidGroupFactor(const float* factors, int n)
{
	float a = 1.0f;
	for (int k = 0; factors && k < n; ++k)
		if (factors[k] > a)
			a = factors[k];
	return a;
}

float PlanPreArrivalReach(float desiredSpeed, float acceleration, float gameSpeed, int latencyMs, float dt)
{
	float hv = PositiveFinite(desiredSpeed) ? desiredSpeed : 0.0f;
	float s = PositiveFinite(gameSpeed) ? gameSpeed : 0.0f;
	float step = PositiveFinite(dt) ? dt : 0.0f;
	float t = latencyMs > 0 ? (float)latencyMs / 1000.0f : 0.0f;
	float brake = PositiveFinite(acceleration) ? 10.0f * hv * hv / (2.0f * acceleration) : 0.0f;
	float reach = 10.0f * hv * (s * t + step) + brake + PLAN_REACH;
	if (!(reach >= PLAN_PRE_REACH_MIN))
		reach = PLAN_PRE_REACH_MIN;
	else if (reach > PLAN_PRE_REACH_MAX)
		reach = PLAN_PRE_REACH_MAX;
	return reach;
}

void PlanPreResolve(const PlanPreResolveIn& in, PlanPreResolveOut* out)
{
	out->keep = 0;
	out->newState = in.state;
	out->count = PPC_NONE;
	out->stepBack = 0;
	out->sample = 0;
	if (!in.epochHolds || !in.legIsTo)
		return;
	int base = in.state & PLAN_PRE_STATE_MASK;
	bool walking = in.characterState == PLAN_CHAR_FOLLOWING;
	bool stopped = in.characterState == PLAN_CHAR_IDLE || in.characterState == PLAN_CHAR_GOAL_REACHED;
	bool pending = in.pathState == PLAN_PATH_UPDATING || in.pathState == PLAN_PATH_WAITING;
	if (pending && stopped)
	{
		out->keep = 1;
		if (base != PLAN_PRE_LATE)
		{
			out->newState = (in.state & ~PLAN_PRE_STATE_MASK) | PLAN_PRE_LATE;
			out->count = PPC_LATE;
		}
	}
	else if (pending)
		out->keep = 1;
	else if (in.pathState == PLAN_PATH_COMPLETE && walking)
	{
		if (base == PLAN_PRE_ISSUED)
		{
			out->count = PPC_LAND;
			out->sample = 1;
		}
	}
	else if (in.pathState == PLAN_PATH_COMPLETE && stopped)
	{
		if (base != PLAN_PRE_LATE)
			out->count = PPC_LATE;
	}
	else if (in.pathState == PLAN_PATH_BROKEN)
	{
		out->keep = 1;
		if (!(in.state & PLAN_PRE_BROKEN_SEEN))
		{
			out->newState = in.state | PLAN_PRE_BROKEN_SEEN;
			out->count = PPC_BROKEN;
		}
	}
	else if (in.pathState == PLAN_PATH_FAILED)
	{
		out->count = PPC_FAILED;
		out->stepBack = 1;
	}
	else if (in.pathState == PLAN_PATH_NONE)
		out->count = PPC_LOST;
	else
		out->keep = 1;
	if (out->keep && in.age > PLAN_PRE_HOLD_SECONDS)
	{
		out->keep = 0;
		out->count = PPC_LOST;
	}
}

} // namespace planner
