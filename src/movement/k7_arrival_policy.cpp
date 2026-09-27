#include "movement/k7_arrival_policy.h"

const float  K7_ARRIVAL_MIN_DIST_SQ      = 1000.0f * 1000.0f;
const double K7_ARRIVAL_MAX_WAIT         = 15.0;
const double K7_ARRIVAL_RECENT_TRANSITION = 4.0;

bool K7ArrivalShouldArm(float dDestSq, int destCellClass, double secsSinceNotIn)
{
	if (dDestSq <= K7_ARRIVAL_MIN_DIST_SQ) return false;
	if (destCellClass == K7_ARRIVAL_ZR_NOT_IN_WORLD) return true;
	if (destCellClass != K7_ARRIVAL_ZR_BUILDINGS_PENDING) return false;
	return secsSinceNotIn >= 0.0 && secsSinceNotIn <= K7_ARRIVAL_RECENT_TRANSITION;
}

bool K7ArrivalArmEdge(bool prevSig, bool sigNow, bool gathering, bool paused, float dDestSq,
                      int destCellClass, double secsSinceNotIn)
{
	return !paused && sigNow && !prevSig && !gathering
	    && K7ArrivalShouldArm(dDestSq, destCellClass, secsSinceNotIn);
}

K7ArrivalOutcome K7ArrivalPoll(int destCellClass, double waitedSec, double maxWaitSec)
{
	if (destCellClass == K7_ARRIVAL_ZR_BUILDINGS_PENDING) return K7_ARRIVAL_FIRE;
	if (waitedSec >= maxWaitSec) return K7_ARRIVAL_EXPIRE;
	return K7_ARRIVAL_WAIT;
}

K7ArrivalAction K7ArrivalFireGate(bool enabled, bool held, bool stillStopped, bool destMatch,
                                  bool zonesOk, bool charBlocked, bool cooldown,
                                  int reissueCount, int maxReissues)
{
	if (held) return K7_ARR_NONE;
	// Every refusal gate is evaluated regardless of `enabled`, so a would-fire
	// latch (k7ArrivalTrigger=false) never overstates savedMs by latching a
	// poll `true` would have refused anyway.
	if (!stillStopped || !destMatch || !zonesOk || charBlocked || cooldown || reissueCount >= maxReissues)
		return K7_ARR_REFUSE;
	return enabled ? K7_ARR_SEND : K7_ARR_LATCH_OBSERVE;
}
