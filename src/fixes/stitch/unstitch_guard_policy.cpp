#include "fixes/stitch/unstitch_guard_policy.h"

UnstitchConnAction UnstitchGuardConnAction(int nodeIndex, int nodeMapSize)
{
	return UnstitchVerdictIsRow(UnstitchClassifyNodeIndex(nodeIndex, nodeMapSize))
		? UNSTITCH_CONN_SKIP_OOB
		: UNSTITCH_CONN_PROCESS;
}

UnstitchMapValueAction UnstitchGuardMapValue(int mapValue, int instancedNodeCount)
{
	if (mapValue == -1)
		return UNSTITCH_MAP_NATIVE_SENTINEL;
	if (mapValue < 0 || mapValue >= instancedNodeCount)
		return UNSTITCH_MAP_SKIP_OUT_OF_RANGE;
	return UNSTITCH_MAP_USE;
}

UnstitchGuardTally UnstitchGuardWalkStubs(int ownUid, const UnstitchSetStub* sets, int setCount,
                                          int* processedOut, int processedCap)
{
	UnstitchGuardTally t;
	t.setsSeen = 0;
	t.setsMatched = 0;
	t.oppositeResolved = 0;
	t.connsExamined = 0;
	t.connsProcessed = 0;
	t.connsSkipped = 0;
	t.implausible = 0;

	if (!UnstitchSetCountPlausible(setCount) || (setCount > 0 && !sets))
	{
		t.implausible = 1;
		return t;
	}

	for (int i = 0; i < setCount; ++i)
	{
		const UnstitchSetStub& s = sets[i];
		++t.setsSeen;

		UnstitchSetAction action = UnstitchClassifySet(s.thisUid, ownUid, s.connCount);
		if (action == UNSTITCH_SET_SKIP_NOT_OURS)
			continue;
		++t.setsMatched;
		if (action == UNSTITCH_SET_SKIP_IMPLAUSIBLE)
		{
			++t.implausible;
			continue;
		}

		if (!s.oppositeResolved)
			continue;
		++t.oppositeResolved;

		for (int j = 0; j < s.connCount; ++j)
		{
			++t.connsExamined;
			int nodeIndex = s.oppositeNodeIndices ? s.oppositeNodeIndices[j] : 0;
			if (UnstitchGuardConnAction(nodeIndex, s.oppositeNodeMapSize) == UNSTITCH_CONN_SKIP_OOB)
			{
				++t.connsSkipped;
				continue;
			}
			if (processedOut && t.connsProcessed < processedCap)
				processedOut[t.connsProcessed] = j;
			++t.connsProcessed;
		}
	}
	return t;
}
