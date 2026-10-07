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

ThrowoutHold ThrowoutHoldDecide(double nowHours, double expiryHours, bool unconscious, double capHours)
{
	if (expiryHours <= 0.0)
		return TH_NONE;
	if (expiryHours - nowHours > capHours + 1.0 / 60.0)
		return TH_STALE;
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

ThrowoutFinderMode ThrowoutFinderModeBeforeTick()
{
	return TFM_CHAIN;
}

bool ThrowoutChainFallback(bool heldResult, bool foreignHopLive)
{
	return heldResult && !foreignHopLive;
}

ThrowoutHopKind ThrowoutClassifyHop(bool readable, bool isJump, bool inExe, bool inOurs, bool inOtherModule)
{
	if (!readable)
		return THK_LIVE;
	if (inOurs)
		return THK_OURS;
	if (isJump)
		return THK_FOLLOW;
	if (inExe)
		return THK_END;
	if (inOtherModule)
		return THK_LIVE;
	return THK_END;
}

static int Disp32(const unsigned char* p)
{
	return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

unsigned __int64 ThrowoutJumpSlot(const unsigned char* bytes, unsigned __int64 at)
{
	if (!bytes || bytes[0] != 0xFF || bytes[1] != 0x25)
		return 0;
	return (unsigned __int64)((__int64)(at + 6) + Disp32(bytes + 2));
}

unsigned __int64 ThrowoutJumpTarget(const unsigned char* bytes, unsigned __int64 at, unsigned __int64 slot)
{
	if (!bytes)
		return 0;
	if (bytes[0] == 0xE9)
		return (unsigned __int64)((__int64)(at + 5) + Disp32(bytes + 1));
	if (bytes[0] == 0xFF && bytes[1] == 0x25)
		return slot;
	return 0;
}

bool ThrowoutSlotResolves(const unsigned char* entryBytes, unsigned __int64 entry, unsigned __int64 expect)
{
	if (entry == expect)
		return true;
	return entryBytes && entryBytes[0] == 0xE9
	    && (unsigned __int64)((__int64)(entry + 5) + Disp32(entryBytes + 1)) == expect;
}

bool ThrowoutCandidate(bool haveChar, bool beingCarried, bool sameTown, bool unconscious,
                       int inSomething, int insideWalls, bool held)
{
	return haveChar && !beingCarried && sameTown && unconscious && inSomething != kThrowoutInPrison
	    && insideWalls > 0 && !held;
}
