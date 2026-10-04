// formation_follow_policy.h - The follow probe's decisions, pure: who follows at the order, when the
// poll captures a member, why a follower is released, what a record does each poll, who leads when the
// leader is lost, the owner predicate's half and the window's distance percentiles. No game header;
// main thread in the game.
#ifndef KEO_FORMATION_FOLLOW_POLICY_H
#define KEO_FORMATION_FOLLOW_POLICY_H

// The probe issues task 44 itself, so only its own followers read it: the game's Follow command is task
// 31 (same follow body and distance), and the follow-loop break that addJob runs for 31 and 45 never runs for 44.
const int    FOLLOW_TASK_TYPE       = 44;       // TaskType FOLLOW_PLAYER_ORDER
const int    FOLLOW_MOVE_DIRECTION  = 2;        // MovementMode MOVE_DIRECTION: the follow task's direct steering
const double FOLLOW_CAPTURE_SECONDS = 2.0;      // at most one capture per member per this long
const double FOLLOW_MAX_SECONDS     = 600.0;    // a record without its group ends this long after its order
const double FOLLOW_LINE_SECONDS    = 5.0;      // one Follow: line per record per this long
const double FOLLOW_SAMPLE_SECONDS  = 0.25;     // a follower's distance to its leader is sampled this often
const float  FOLLOW_ARRIVAL_DIST_SQ = 2500.0f;  // a leader within 50 units of the destination has arrived
const int    FOLLOW_SAMPLES_MAX     = 512;      // distance samples one Follow: line summarises

enum FollowMode { FF_OFF = 0, FF_PROBE };
enum FollowRelease { FFR_NONE = 0, FFR_ORDER, FFR_STOP, FFR_SPEED, FFR_ARRIVAL, FFR_COMPLETE, FFR_GONE,
                     FFR_DROPPED, FFR_TIMEOUT, FFR_PROMOTED, FFR_COUNT };
// The owner query's seams, each counted apart: the tracker's park test and per-order poll, the order
// outcome's motion note, the formation's travel re-issue, its gather send and its travel send.
enum FollowSeam { FFS_PARK = 0, FFS_POLL, FFS_OUTCOME, FFS_TRAVEL, FFS_GATHER, FFS_SEND, FFS_COUNT };

// At the order: member index (0 is the leader, never a follower) follows when within the gather radius
// of the leader (x-z, both squared).
bool FollowAtOrder(int index, float distSqToLeader, float gatherRadiusSq);
// The poll's capture of a live member that is not following: within the gather radius of the leader,
// and no capture for FOLLOW_CAPTURE_SECONDS (lastTry <= 0: none yet).
bool FollowCaptureDue(float distSqToLeader, float gatherRadiusSq, double now, double lastTry);
// The poll captures only a member its formation group still lists and has not dispatched: the stop key
// and a new order take the member off the group, and the arrival scatter dispatches it.
bool FollowMayCapture(bool inGroup, bool dispatched);

// The current tasks a move resumes after rather than ends on: a forced stumble (147), a stand-still a
// passer-by posts (62) and self-preservation (32).
bool FollowPreemptionTask(int taskType);
// One follower this poll: in the player list, its speed mode GROUPED, whether its current task has read
// the follow task since its capture (seenTask), reads it now (taskNow), or reads a preemption the
// follow task resumes after (preempted).
struct FollowerState { int live; int grouped; int seenTask; int taskNow; int preempted; };
// FFR_GONE (not live), FFR_SPEED (not grouped), FFR_DROPPED (seen, and its task neither the follow task
// nor a preemption: the engine ended it without a release), else FFR_NONE: still following.
int FollowerStep(const FollowerState& s);

// One record this poll.
struct FollowRecordState
{
	int    followers;            // members following now
	int    leaderLive;           // the leader in the player list, not detached by an order or the stop key
	int    groupActive;          // its formation group still active (the same slot and id)
	int    leaderGrouped;        // the leader's speed mode GROUPED
	float  leaderDistSqToDest;   // x-z
	double age;                  // seconds since the order
};
enum FollowRecordAction { FRA_KEEP = 0, FRA_PROMOTE, FRA_RELEASE_SEND, FRA_RELEASE, FRA_END };
// In this order: no live leader, PROMOTE while a follower remains, else END; the group active, KEEP (its
// scatter releases the followers, and a member may still be captured); no follower, END; the leader
// within FOLLOW_ARRIVAL_DIST_SQ of the destination, RELEASE_SEND with FFR_COMPLETE (each follower is sent
// the destination); the record FOLLOW_MAX_SECONDS old, RELEASE_SEND with FFR_TIMEOUT; the leader's speed
// mode not grouped, RELEASE with FFR_SPEED (the followers keep their vanilla task); else KEEP. *reason is
// FFR_NONE unless a release.
int FollowRecordStep(const FollowRecordState& s, int* reason);
// The new leader when the leader is lost: the first live follower in member order; -1 when none.
int FollowPromote(const int* liveFollowing, int n);
// The pct-th percentile (0..100, nearest rank) of samples[0..n), which it sorts; 0 for n <= 0.
float FollowPercentile(float* samples, int n, int pct);
// The owner predicate's half: a member is owned while the probe is armed and it follows.
bool FollowOwns(int armed, int following);

#endif // KEO_FORMATION_FOLLOW_POLICY_H
