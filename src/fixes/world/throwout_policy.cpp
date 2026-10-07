// throwout_policy.cpp - The throw-out fix's pure decisions.
#include "fixes/world/throwout_policy.h"

bool ThrowoutMayAskGateCode(bool indoors, bool gatesPresent, bool navmeshPresent, bool navmeshStopped)
{
	return !indoors && gatesPresent && navmeshPresent && !navmeshStopped;
}

ThrowoutFlag ThrowoutFlagDecide(bool indoors, int gateCode)
{
	if (indoors)
		return TF_INDOORS;
	if (gateCode < 0)
		return TF_UNRESOLVED;
	return TF_WRITE;
}

bool ThrowoutTownDrop(int reason, bool townTask)
{
	return reason != TR_NONE && townTask;
}

bool ThrowoutTimedOut(double stampHours, double nowHours)
{
	return stampHours != 0.0 && (nowHours - stampHours) * 60.0 >= 5.0;
}

ThrowoutDrop ThrowoutClassifyDrop(int reason, bool timedOut, ThrowoutFlag flag, int gateCode)
{
	if (reason == TR_NONE)
		return TD_NONE;
	if (reason == TR_PATH_IMPOSSIBLE)
		return TD_PATH_IMPOSSIBLE;
	if (timedOut)
		return TD_TIMEOUT;
	if (flag == TF_WRITE && gateCode == 0)
		return TD_REACHED_OUTSIDE;
	return TD_REACHED_INSIDE;
}

ThrowoutHold ThrowoutHoldDecide(double nowHours, double expiryHours, bool unconscious)
{
	if (expiryHours <= 0.0)
		return TH_NONE;
	if (nowHours >= expiryHours)
		return TH_CAP;
	if (!unconscious)
		return TH_WOKE;
	return TH_HELD;
}

double ThrowoutHoldExpiry(double nowHours, int holdMinutes)
{
	return nowHours + (double)holdMinutes / 60.0;
}

ThrowoutFinderMode ThrowoutFinderModeFor(bool otherFinderLoaded)
{
	return otherFinderLoaded ? TFM_CHAIN : TFM_REPLACE;
}

bool ThrowoutCandidate(bool haveChar, bool beingCarried, bool sameTown, bool unconscious,
                       int inSomething, int insideWalls, bool held)
{
	return haveChar && !beingCarried && sameTown && unconscious && inSomething != kThrowoutInPrison
	    && insideWalls > 0 && !held;
}
