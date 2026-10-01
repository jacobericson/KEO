// plan_build.cpp - The route planner's plan-building rules as pure functions of their inputs: no
// state, no lock, no allocation, any thread.

#include <cstring>
#include "planner/plan_build.h"

namespace planner {

static const float FOOTPRINT_WIDEN_XZ   = 5.0f;
static const float FOOTPRINT_WIDEN_Y    = 30.0f;
static const float FOOTPRINT_FALLBACK   = 200.0f;
static const float REPEAT_DEST_MATCH    = 1.0f;

static void Copy3(float out[3], const float in[3])
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
}

static float DistanceSq3(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

static float DistanceSqXz(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dz = a[2] - b[2];
	return dx * dx + dz * dz;
}

static void PortalLeg(const PlanRouteStep& from, int farSection, PlanLeg* leg)
{
	memset(leg, 0, sizeof(*leg));
	Copy3(leg->point, from.portal);
	Copy3(leg->edgeA, from.edgeA);
	Copy3(leg->edgeB, from.edgeB);
	PlanCellOf(leg->point[0], leg->point[2], &leg->cellX, &leg->cellY);
	leg->farSection = farSection;
	leg->isDestination = 0;
}

int PlanBuildLegs(const PlanRouteStep* steps, int n, const float dest[3], int destSection,
                  PlanLeg* out, int* truncated)
{
	int legs = 0;
	int cut = 0;
	for (int i = 0; steps && i + 1 < n; ++i)
	{
		if (!steps[i].crossesNext)
			continue;
		if (legs >= PLAN_MAX_PORTAL_LEGS)
		{
			cut = 1;
			break;
		}
		PortalLeg(steps[i], steps[i + 1].dirIndex, &out[legs]);
		++legs;
	}

	PlanLeg* d = &out[legs];
	memset(d, 0, sizeof(*d));
	Copy3(d->point, dest);
	Copy3(d->edgeA, dest);
	Copy3(d->edgeB, dest);
	PlanCellOf(dest[0], dest[2], &d->cellX, &d->cellY);
	d->farSection = destSection;
	d->isDestination = 1;
	++legs;

	if (truncated)
		*truncated = cut;
	return legs;
}

static bool InWidenedBox(const PlanNodeBox& b, const float p[3])
{
	return p[0] >= b.boxMin[0] - FOOTPRINT_WIDEN_XZ && p[0] <= b.boxMax[0] + FOOTPRINT_WIDEN_XZ
	    && p[1] >= b.boxMin[1] - FOOTPRINT_WIDEN_Y  && p[1] <= b.boxMax[1] + FOOTPRINT_WIDEN_Y
	    && p[2] >= b.boxMin[2] - FOOTPRINT_WIDEN_XZ && p[2] <= b.boxMax[2] + FOOTPRINT_WIDEN_XZ;
}

int PlanPickFootprint(const PlanNodeBox* nodes, int n, const float p[3], bool allowFallback)
{
	int best = -1;
	float bestD = 0.0f;
	for (int i = 0; i < n; ++i)
	{
		if (!InWidenedBox(nodes[i], p))
			continue;
		float d = DistanceSq3(nodes[i].centre, p);
		if (best < 0 || d < bestD)
		{
			best = i;
			bestD = d;
		}
	}
	if (best >= 0 || !allowFallback)
		return best;

	const float limit = FOOTPRINT_FALLBACK * FOOTPRINT_FALLBACK;
	for (int i = 0; i < n; ++i)
	{
		float d = DistanceSqXz(nodes[i].centre, p);
		if (d <= limit && (best < 0 || d < bestD))
		{
			best = i;
			bestD = d;
		}
	}
	return best;
}

// The movement destination is the engine's last requested point, of which the tick reads x and z:
// zero in both is no destination, and the match is measured in x-z.
PlanDropWhy PlanDropDue(bool livePlayer, bool unconscious, float distToDest,
                        const float moveDest[3], const float planDest[3], double planAge, bool moveDestIsModSend)
{
	if (!livePlayer)
		return PDW_NOT_PLAYER;
	if (unconscious)
		return PDW_KO;
	if (distToDest <= PLAN_POST_ARRIVAL)
		return PDW_ARRIVED;
	if (planAge < PLAN_ORDER_SETTLE)
		return PDW_NONE;   // the previous destination may still be in place
	bool haveDest = moveDest[0] != 0.0f || moveDest[2] != 0.0f;
	if (haveDest && !moveDestIsModSend && DistanceSqXz(moveDest, planDest) > PLAN_DEST_MATCH * PLAN_DEST_MATCH)
		return PDW_NEW_DEST;
	return PDW_NONE;
}

bool PlanIsModSend(const float moveDest[3], const float resend[][3], int resendCount,
                   const float holdDest[3], int haveHold, double holdAge)
{
	const float match = PLAN_DEST_MATCH * PLAN_DEST_MATCH;
	if (!resend || resendCount < 0) resendCount = 0;
	if (resendCount > PLAN_RESEND_POINTS) resendCount = PLAN_RESEND_POINTS;
	for (int i = 0; i < resendCount; ++i)
		if (DistanceSqXz(moveDest, resend[i]) <= match) return true;
	return haveHold && holdDest && holdAge < PLAN_HOLD_SECONDS && DistanceSqXz(moveDest, holdDest) <= match;
}

bool PlanRepeatDue(const float planDest[3], const float newDest[3], double planAge)
{
	return planAge < PLAN_ORDER_SETTLE && DistanceSqXz(planDest, newDest) < REPEAT_DEST_MATCH * REPEAT_DEST_MATCH;
}

unsigned __int64 PlanMemoKey(unsigned startNode, unsigned goalNode)
{
	return ((unsigned __int64)startNode << 32) | (unsigned __int64)goalNode;
}

} // namespace planner
