// nm_claimed_job.h - One mod-unlinked type 0/1 NavMesh job and its caller-owned resources:
// the claim's one raise under the queue lock, then its release checkpoints.
// Checkpoints preserve the pipeline write, worker clone and final claim order;
// there is no automatic cleanup when a stack frame unwinds.
#ifndef KEO_NM_CLAIMED_JOB_H
#define KEO_NM_CLAIMED_JOB_H

#include "navmesh/nm_workers.h"
#include "navmesh/jobs/nm_claimed_job_policy.h"

class NmQueueLock;

struct ClaimedJob
{
	uintptr_t job;
	int jobType;
	LONGLONG claimQpc;
	LONG resetRaises;
	int claimSlot;
	void* clone;
	L2WriteBlob pendingWrite;
	int stage;

	ClaimedJob();

private:
	ClaimedJob(const ClaimedJob&);
	ClaimedJob& operator=(const ClaimedJob&);
};

// The one raise of a claim, called holding the generator's queue lock (+152)
// as q right after the job's unlink and before the unlock: the busy bridge
// (on q's generator), this thread's claim slot and the reset gate's raise
// count, in that order, then the job's own fields. q must still hold +152: the
// busy bridge reads its generator from q, and a released q names none. claimQpc
// is taken after the unlock; clone and pendingWrite are the pipeline's.
void ClaimedJobBeginLocked(const NmQueueLock& q, ClaimedJob* claimed, uintptr_t job, int jobType,
                           uintptr_t zone, int claimSlot);
// Called last in PjCtx::Handoff, after the job is handed to the game or deleted.
// No mod cache or process lock is held; the policy's data-only guard keeps the timed writer.
void ClaimedJobWritePending(ClaimedJob* claimed);
// Called at the two worker clone cleanup sites. Each call takes its
// own live retire handshake; it never retries a denied free at Finish.
void ClaimedJobReleaseClone(ClaimedJob* claimed);
// Runs at most once for a claimed job (a second call releases nothing). The worker takes the live retire
// handshake before adjacency/busy release; BG adjacency and busy callbacks
// take queue +152 independently, after all process/cache locks are gone.
void ClaimedJobFinish(ClaimedJob* claimed, CjReleasePoint point, bool bgAdj);

#endif
