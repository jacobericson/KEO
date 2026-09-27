#include "fixes/stitch/unstitch_probe_policy.h"

UnstitchNodeVerdict UnstitchClassifyNodeIndex(int nodeIndex, int nodeMapSize)
{
	if (nodeMapSize <= 0)
		return UNSTITCH_NODE_IN_BOUNDS;  // the indexed read is never made
	if (nodeIndex < 0)
		return UNSTITCH_NODE_NEGATIVE;
	if (nodeIndex >= nodeMapSize)
		return UNSTITCH_NODE_PAST_END;
	return UNSTITCH_NODE_IN_BOUNDS;
}

bool UnstitchVerdictIsRow(UnstitchNodeVerdict v)
{
	return v == UNSTITCH_NODE_NEGATIVE || v == UNSTITCH_NODE_PAST_END;
}

bool UnstitchSetCountPlausible(int setCount)
{
	return setCount >= 0 && setCount <= UNSTITCH_MAX_SETS;
}

bool UnstitchConnCountPlausible(int connCount)
{
	return connCount >= 0 && connCount <= UNSTITCH_MAX_CONNS;
}

UnstitchSetAction UnstitchClassifySet(int thisUid, int ownUid, int connCount)
{
	if (thisUid != ownUid)
		return UNSTITCH_SET_SKIP_NOT_OURS;
	if (!UnstitchConnCountPlausible(connCount))
		return UNSTITCH_SET_SKIP_IMPLAUSIBLE;
	return UNSTITCH_SET_WALK;
}

UnstitchWalkTally UnstitchWalkStubs(int ownUid, const UnstitchSetStub* sets, int setCount)
{
	UnstitchWalkTally t;
	t.setsSeen = 0;
	t.setsMatched = 0;
	t.oppositeResolved = 0;
	t.connsExamined = 0;
	t.rows = 0;
	t.negativeRows = 0;
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
			UnstitchNodeVerdict v = UnstitchClassifyNodeIndex(nodeIndex, s.oppositeNodeMapSize);
			if (!UnstitchVerdictIsRow(v))
				continue;
			++t.rows;
			if (v == UNSTITCH_NODE_NEGATIVE)
				++t.negativeRows;
		}
	}
	return t;
}
