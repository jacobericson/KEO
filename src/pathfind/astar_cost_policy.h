#ifndef KEO_ASTAR_COST_POLICY_H
#define KEO_ASTAR_COST_POLICY_H

// Pure classification and bucketing for the A* cost instrument. No game or
// Windows headers, so it is host-testable
// (tools/tests/astar_cost_policy_units.cpp) and cannot drift from what
// astar_cost.cpp does with these return values.

enum AstarCallerClass
{
	ASTAR_CALLER_CHARACTER_PLAYER  = 0,
	ASTAR_CALLER_CHARACTER_NPC     = 1,
	ASTAR_CALLER_CHARACTER_UNKNOWN = 2,  // the character call site, but the request's tag was unresolved
	ASTAR_CALLER_GATE              = 3,
	ASTAR_CALLER_GENERATION        = 4,
	ASTAR_CALLER_HAVOK_INTERNAL    = 5,  // a Havok-internal caller, not the generation one
	ASTAR_CALLER_OTHER             = 6,  // unrecognized in-exe return address, or an out-of-exe one
	ASTAR_CALLER_CLASS_COUNT       = 7
};

// Return-address RVAs, immediately after the 5-byte E8 call to
// Havok__findPathFull (0xCE56D0), for each of its six known callers:
//   Gates__findPath+0x88             call RVA 0x2EC5C8 -> return 0x2EC5CD
//   NavMesh__findPath+0x1B8          call RVA 0x3AADA8 -> return 0x3AADAD
//   sub_140CE62B0+0x5C (unnamed)     call RVA 0xCE630C -> return 0xCE6311
//   hkaiNavMeshClustering__edgeCostSearch+0x225
//                                     call RVA 0xD29B15 -> return 0xD29B1A
//   hkaiWorld__stepPathSearches+0x3DB
//                                     call RVA 0xD8D9AB -> return 0xD8D9B0
//   sub_140DB6C00+0x1F9 (hkai*Behavior vtable slot 11)
//                                     call RVA 0xDB6DF9 -> return 0xDB6DFE
const unsigned ASTAR_RET_GATE_FINDPATH       = 0x2EC5CDu;
const unsigned ASTAR_RET_CHARACTER_FINDPATH  = 0x3AADADu;
const unsigned ASTAR_RET_HAVOK_CE62B0        = 0xCE6311u;
const unsigned ASTAR_RET_GENERATION_EDGECOST = 0xD29B1Au;
const unsigned ASTAR_RET_HAVOK_STEPPATH      = 0xD8D9B0u;
const unsigned ASTAR_RET_HAVOK_DB6C00        = 0xDB6DFEu;

// returnRva is the caller's return address minus the game module's base;
// inExe is whether that subtraction is trustworthy (astar_cost.cpp checks it
// against the module's own SizeOfImage -- an out-of-range or out-of-module
// return address, e.g. another plugin's own detour chained ahead of this
// hook, must not be classified on a guess). playerByReq is the request-
// derived tag hook_findPathFull already resolves for the boost/PathPool
// split: -1 unknown, 0 npc, 1 player.
//
// A returnRva that is not one of the six known sites, or inExe == false,
// classifies as ASTAR_CALLER_OTHER; it is never folded into the character,
// gate or generation buckets on a guess.
AstarCallerClass AstarClassifyCaller(unsigned returnRva, bool inExe, int playerByReq);

// Fixed log2 buckets: bucket 0 is "<= 0"; bucket k (k >= 1) covers
// [2^(k-1), 2^k) of whatever unit the caller passes (microseconds for
// latency, a plain count for iterations), clamped into [0, numBuckets).
// One bucketing rule and one test file cover both histograms.
const int ASTAR_HIST_BUCKETS = 24;
int AstarLogBucket(long long value, int numBuckets);

#endif // KEO_ASTAR_COST_POLICY_H
