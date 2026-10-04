// formation_gather_policy.cpp - The gathering group's rules: the skip, the re-send, the gather timeout and
// the group timeout. Pure; the caller's thread.
#include "movement/formation_gather_policy.h"

bool FormationSkipWhileGathering(bool groupGathered, bool memberAlone)
{
	return !groupGathered && !memberAlone;
}

bool FormationGatherResendDue(bool gatherSent, float distSq, float gatherRadiusSq, float farSq, bool orderGone,
                              double sinceLastSendSec, double cooldownSec)
{
	return gatherSent && orderGone && distSq > gatherRadiusSq && distSq > farSq && sinceLastSendSec >= cooldownSec;
}

double FormationGatherTimeout(float routeDist, float speed, double floorSec, double capSec)
{
	if (!(speed > 0.0f) || !(routeDist > 0.0f))
		return floorSec;
	double t = (double)routeDist / (double)speed * FORMATION_GATHER_MARGIN;
	if (t > capSec)
		t = capSec;
	if (t < floorSec)
		t = floorSec;
	return t;
}

bool FormationTimeoutCancels(double ageSec, double limitSec, double ceilingSec, bool merged, int walking)
{
	if (!(ageSec > limitSec))
		return false;
	if (merged && walking > 0)
		return ageSec > ceilingSec;
	return true;
}
