#include <cstdio>
#include "pathfind/astar_cost_policy.h"

#include "check.h"

int main()
{
	// Out-of-exe (or unresolved) return addresses never guess a class, even
	// when the bit pattern happens to match a known site.
	{
		Check(AstarClassifyCaller(ASTAR_RET_GATE_FINDPATH, false, -1) == ASTAR_CALLER_OTHER,
		      "gate site, but not in the exe, is other");
		Check(AstarClassifyCaller(ASTAR_RET_CHARACTER_FINDPATH, false, 1) == ASTAR_CALLER_OTHER,
		      "character site, but not in the exe, is other");
	}

	// The six known call sites.
	{
		Check(AstarClassifyCaller(ASTAR_RET_GATE_FINDPATH, true, -1) == ASTAR_CALLER_GATE,
		      "Gates::findPath's return address is the gate class");
		Check(AstarClassifyCaller(ASTAR_RET_GENERATION_EDGECOST, true, -1) == ASTAR_CALLER_GENERATION,
		      "edgeCostSearch's return address is the generation class");
		Check(AstarClassifyCaller(ASTAR_RET_HAVOK_CE62B0, true, -1) == ASTAR_CALLER_HAVOK_INTERNAL,
		      "the unnamed wrapper is havok-internal");
		Check(AstarClassifyCaller(ASTAR_RET_HAVOK_STEPPATH, true, -1) == ASTAR_CALLER_HAVOK_INTERNAL,
		      "hkaiWorld::stepPathSearches is havok-internal");
		Check(AstarClassifyCaller(ASTAR_RET_HAVOK_DB6C00, true, -1) == ASTAR_CALLER_HAVOK_INTERNAL,
		      "the behavior vtable caller is havok-internal");
	}

	// The character site splits by the request-derived tag, not any sticky
	// global -- unknown stays its own bucket rather than defaulting to NPC.
	{
		Check(AstarClassifyCaller(ASTAR_RET_CHARACTER_FINDPATH, true, 1) == ASTAR_CALLER_CHARACTER_PLAYER,
		      "character site, playerByReq=1, is player");
		Check(AstarClassifyCaller(ASTAR_RET_CHARACTER_FINDPATH, true, 0) == ASTAR_CALLER_CHARACTER_NPC,
		      "character site, playerByReq=0, is npc");
		Check(AstarClassifyCaller(ASTAR_RET_CHARACTER_FINDPATH, true, -1) == ASTAR_CALLER_CHARACTER_UNKNOWN,
		      "character site, playerByReq=-1, is unknown (not npc)");
	}

	// An in-exe return address matching none of the six sites is "other",
	// not folded into any named class.
	{
		Check(AstarClassifyCaller(0x1234u, true, 1) == ASTAR_CALLER_OTHER,
		      "an unrecognized in-exe return address is other");
	}

	// Bucketing: <= 0 is bucket 0; bucket k covers [2^(k-1), 2^k).
	{
		Check(AstarLogBucket(0, ASTAR_HIST_BUCKETS) == 0, "zero is bucket 0");
		Check(AstarLogBucket(-5, ASTAR_HIST_BUCKETS) == 0, "negative is bucket 0");
		Check(AstarLogBucket(1, ASTAR_HIST_BUCKETS) == 1, "1 is bucket 1");
		Check(AstarLogBucket(1LL << 32, ASTAR_HIST_BUCKETS) == ASTAR_HIST_BUCKETS - 1,
		      "far past the top clamps to the last bucket, not undefined behaviour");
	}

	// Bucket boundaries are exact powers of two.
	{
		for (int k = 1; k < ASTAR_HIST_BUCKETS - 1; ++k)
		{
			long long lo = 1LL << (k - 1);
			long long hi = (1LL << k) - 1;
			char msg[64];
			sprintf(msg, "bucket %d low edge", k);
			Check(AstarLogBucket(lo, ASTAR_HIST_BUCKETS) == k, msg);
			sprintf(msg, "bucket %d high edge", k);
			Check(AstarLogBucket(hi, ASTAR_HIST_BUCKETS) == k, msg);
			sprintf(msg, "bucket %d+1 low edge", k);
			Check(AstarLogBucket(hi + 1, ASTAR_HIST_BUCKETS) == k + 1, msg);
		}
	}

	// Degenerate numBuckets never reads past a caller's array.
	{
		Check(AstarLogBucket(1000000, 1) == 0, "numBuckets=1 always returns 0");
		Check(AstarLogBucket(1000000, 0) == 0, "numBuckets=0 always returns 0");
	}

	return CheckExit("astar_cost_policy_units");
}
