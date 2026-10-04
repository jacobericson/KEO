// The follow probe's decisions: followers at the order, the capture, its rate and its group test, each release reason,
// a record's step, the leader promotion, the owner predicate and the distance percentiles.
#include <cstdio>
#include "movement/formation_follow_policy.h"

#include "check.h"

static void CheckOrderAndCapture()
{
	const float r2 = 40.0f * 40.0f;
	CHECK(FollowAtOrder(1, 30.0f * 30.0f, r2) && !FollowAtOrder(2, 50.0f * 50.0f, r2),
	      "follow: at the order a member within the gather radius follows and one beyond does not");
	CHECK(!FollowAtOrder(0, 0.0f, r2), "follow: the leader never follows");
	CHECK(FollowCaptureDue(30.0f * 30.0f, r2, 10.0, 0.0), "follow: a member that comes within the radius is captured");
	CHECK(!FollowCaptureDue(30.0f * 30.0f, r2, 11.5, 10.0), "follow: a capture is not retried within 2 s");
	CHECK(FollowCaptureDue(30.0f * 30.0f, r2, 12.0, 10.0), "follow: a capture is retried after 2 s");
	CHECK(!FollowCaptureDue(50.0f * 50.0f, r2, 20.0, 0.0), "follow: a member beyond the radius is not captured");
	CHECK(FollowMayCapture(true, false), "follow: a member the group still lists undispatched may be captured");
	CHECK(!FollowMayCapture(false, false), "follow: a member off the group (stop key, new order) is not captured");
	CHECK(!FollowMayCapture(true, true), "follow: a member the arrival scatter dispatched is not captured");
}

static void CheckRelease()
{
	FollowerState s = { 1, 1, 0, 0, 0 };
	CHECK(FollowerStep(s) == FFR_NONE, "follow: a follower whose task has not switched yet keeps following");
	s.seenTask = 1;
	s.taskNow = 1;
	CHECK(FollowerStep(s) == FFR_NONE, "follow: a follower on the follow task keeps following");
	s.taskNow = 0;
	CHECK(FollowerStep(s) == FFR_DROPPED, "follow: a follow task that ended without a release is dropped");
	s.preempted = 1;
	CHECK(FollowerStep(s) == FFR_NONE, "follow: a follower in a stumble or a stand-still keeps following");
	s.preempted = 0;
	CHECK(FollowPreemptionTask(147) && FollowPreemptionTask(62) && FollowPreemptionTask(32)
	      && !FollowPreemptionTask(FOLLOW_TASK_TYPE) && !FollowPreemptionTask(31) && !FollowPreemptionTask(-1),
	      "follow: a stumble, a stand-still and self-preservation are preemptions, the follow tasks are not");
	s.grouped = 0;
	CHECK(FollowerStep(s) == FFR_SPEED, "follow: a follower that changed speed mode is released");
	s.live = 0;
	CHECK(FollowerStep(s) == FFR_GONE, "follow: a follower gone from the list is released");
}

static void CheckRecord()
{
	int reason = -1;
	FollowRecordState r = { 2, 1, 1, 1, 90000.0f, 30.0 };
	CHECK(FollowRecordStep(r, &reason) == FRA_KEEP && reason == FFR_NONE,
	      "follow record: while its formation group is active the record keeps");
	r.followers = 0;
	CHECK(FollowRecordStep(r, &reason) == FRA_KEEP,
	      "follow record: while its group is active a record with no follower yet keeps");
	r.groupActive = 0;
	CHECK(FollowRecordStep(r, &reason) == FRA_END, "follow record: a record with no follower and no group ends");
	r.followers = 2;
	r.leaderLive = 0;
	CHECK(FollowRecordStep(r, &reason) == FRA_PROMOTE, "follow record: a lost leader promotes a follower");
	r.followers = 0;
	CHECK(FollowRecordStep(r, &reason) == FRA_END, "follow record: a lost leader with no follower ends");
	r.followers = 2;
	r.leaderLive = 1;
	CHECK(FollowRecordStep(r, &reason) == FRA_KEEP, "follow record: a leader still travelling keeps its followers");
	r.leaderDistSqToDest = 900.0f;
	CHECK(FollowRecordStep(r, &reason) == FRA_RELEASE_SEND && reason == FFR_COMPLETE,
	      "follow record: a leader at the destination without the group's scatter releases each follower to it");
	r.leaderDistSqToDest = 90000.0f;
	r.age = 601.0;
	CHECK(FollowRecordStep(r, &reason) == FRA_RELEASE_SEND && reason == FFR_TIMEOUT,
	      "follow record: a record past its age releases each follower to the destination");
	r.age = 30.0;
	r.leaderGrouped = 0;
	CHECK(FollowRecordStep(r, &reason) == FRA_RELEASE && reason == FFR_SPEED,
	      "follow record: a leader that changed speed mode releases its followers in place");
}

static void CheckPromoteOwnsPercentile()
{
	const int live[3] = { 0, 1, 1 };
	const int none[2] = { 0, 0 };
	CHECK(FollowPromote(live, 3) == 1 && FollowPromote(none, 2) == -1, "follow: the first live follower leads");
	CHECK(FollowOwns(1, 1) && !FollowOwns(1, 0), "follow: an armed probe owns its followers alone");
	CHECK(!FollowOwns(0, 1), "follow: an unarmed probe owns nothing");
	float s[10] = { 10.0f, 1.0f, 9.0f, 2.0f, 8.0f, 3.0f, 7.0f, 4.0f, 6.0f, 5.0f };
	CHECK(FollowPercentile(s, 10, 50) == 5.0f && FollowPercentile(s, 10, 90) == 9.0f,
	      "follow: the distance percentiles are the nearest-rank values");
	CHECK(FollowPercentile(s, 0, 50) == 0.0f, "follow: no sample reads 0");
}

int main()
{
	CheckOrderAndCapture();
	CheckRelease();
	CheckRecord();
	CheckPromoteOwnsPercentile();
	return CheckExit("formation_follow_policy_units");
}
