#include "pathfind/astar_cost_policy.h"

AstarCallerClass AstarClassifyCaller(unsigned returnRva, bool inExe, int playerByReq)
{
	if (!inExe)
		return ASTAR_CALLER_OTHER;

	if (returnRva == ASTAR_RET_GATE_FINDPATH)
		return ASTAR_CALLER_GATE;

	if (returnRva == ASTAR_RET_GENERATION_EDGECOST)
		return ASTAR_CALLER_GENERATION;

	if (returnRva == ASTAR_RET_HAVOK_CE62B0 ||
	    returnRva == ASTAR_RET_HAVOK_STEPPATH ||
	    returnRva == ASTAR_RET_HAVOK_DB6C00)
		return ASTAR_CALLER_HAVOK_INTERNAL;

	if (returnRva == ASTAR_RET_CHARACTER_FINDPATH)
	{
		if (playerByReq > 0)  return ASTAR_CALLER_CHARACTER_PLAYER;
		if (playerByReq == 0) return ASTAR_CALLER_CHARACTER_NPC;
		return ASTAR_CALLER_CHARACTER_UNKNOWN;
	}

	return ASTAR_CALLER_OTHER;
}

int AstarLogBucket(long long value, int numBuckets)
{
	if (numBuckets <= 1 || value <= 0)
		return 0;

	int bucket = 1;
	long long threshold = 1;
	while (value >= (threshold << 1) && bucket < numBuckets - 1)
	{
		threshold <<= 1;
		++bucket;
	}
	return bucket;
}
