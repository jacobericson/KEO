// formation_follow_policy.cpp - The follow probe's pure decisions. Main thread in the game; no lock.
#include <algorithm>
#include "movement/formation_follow_policy.h"

bool FollowAtOrder(int index, float distSqToLeader, float gatherRadiusSq)
{
	return index > 0 && distSqToLeader <= gatherRadiusSq;
}

bool FollowCaptureDue(float distSqToLeader, float gatherRadiusSq, double now, double lastTry)
{
	if (!(distSqToLeader <= gatherRadiusSq))
		return false;
	return lastTry <= 0.0 || now - lastTry >= FOLLOW_CAPTURE_SECONDS;
}

bool FollowMayCapture(bool inGroup, bool dispatched)
{
	return inGroup && !dispatched;
}

bool FollowPreemptionTask(int taskType)
{
	return taskType == 147 || taskType == 62 || taskType == 32;
}

int FollowerStep(const FollowerState& s)
{
	if (!s.live)
		return FFR_GONE;
	if (!s.grouped)
		return FFR_SPEED;
	if (s.seenTask && !s.taskNow && !s.preempted)
		return FFR_DROPPED;
	return FFR_NONE;
}

int FollowRecordStep(const FollowRecordState& s, int* reason)
{
	*reason = FFR_NONE;
	if (!s.leaderLive)
		return s.followers > 0 ? FRA_PROMOTE : FRA_END;
	if (s.groupActive)
		return FRA_KEEP;
	if (s.followers <= 0)
		return FRA_END;
	if (s.leaderDistSqToDest <= FOLLOW_ARRIVAL_DIST_SQ)
	{
		*reason = FFR_COMPLETE;
		return FRA_RELEASE_SEND;
	}
	if (s.age >= FOLLOW_MAX_SECONDS)
	{
		*reason = FFR_TIMEOUT;
		return FRA_RELEASE_SEND;
	}
	if (!s.leaderGrouped)
	{
		*reason = FFR_SPEED;
		return FRA_RELEASE;
	}
	return FRA_KEEP;
}

int FollowPromote(const int* liveFollowing, int n)
{
	for (int i = 0; liveFollowing && i < n; ++i)
		if (liveFollowing[i])
			return i;
	return -1;
}

float FollowPercentile(float* samples, int n, int pct)
{
	if (!samples || n <= 0)
		return 0.0f;
	std::sort(samples, samples + n);
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	int rank = (pct * n + 99) / 100;
	if (rank < 1)
		rank = 1;
	return samples[rank - 1];
}

bool FollowOwns(int armed, int following)
{
	return armed != 0 && following != 0;
}
