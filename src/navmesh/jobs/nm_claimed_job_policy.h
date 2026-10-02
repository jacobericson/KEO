// nm_claimed_job_policy.h - Pure release plan for a claimed NavMesh job. No game or platform types.
#ifndef KEO_NM_CLAIMED_JOB_POLICY_H
#define KEO_NM_CLAIMED_JOB_POLICY_H

enum CjReleasePoint
{
	CJ_RELEASE_PIPELINE_WRITE = 0,
	CJ_RELEASE_WORKER_CLONE,
	CJ_FINISH_WORKER,
	CJ_FINISH_BG_CONTENT_LOST,
	CJ_FINISH_BG_PIPELINE_RETURN,
	CJ_RELEASE_POINT_COUNT
};

struct CjReleaseFacts
{
	bool hasPendingData;
	bool hasClone;
	bool bgAdj;
};

enum CjTail
{
	CJ_TAIL_NONE = 0,
	CJ_TAIL_WORKER,
	CJ_TAIL_BG_DROP,
	CJ_TAIL_BG_FINISHED
};

struct CjReleasePlan
{
	bool writePending;
	bool releaseClone;
	CjTail tail;
	bool bgAdj;
};

CjReleasePlan CjReleasePlanFor(CjReleasePoint point, const CjReleaseFacts& facts);

struct CjReleaseOps
{
	void* ctx;
	void (*writePending)(void*);
	bool (*cleanupBegin)(void*);
	void (*freeClone)(void*);
	void (*cleanupEnd)(void*);
	void (*clearClaimSlot)(void*);
	void (*adjFinished)(void*);
	void (*adjDropped)(void*);
	void (*leaveBusy)(void*);
	void (*idle)(void*);
};

void CjRunReleases(const CjReleasePlan& plan, const CjReleaseOps& ops);

// A claimed job's progress. A finish runs its release tail only from
// CJ_STAGE_CLAIMED and moves the stage to CJ_STAGE_FINISHED.
enum CjStage { CJ_STAGE_EMPTY = 0, CJ_STAGE_CLAIMED, CJ_STAGE_FINISHED };
// True, with the stage moved to finished, for the first finish of a claimed
// job; false, nothing changed, for a job never claimed or already finished.
bool CjFinishAdmit(int* stage);
// A claimed job's finish: runs the plan's releases only when CjFinishAdmit
// admits the stage, so the release tail runs at most once. True when it ran.
bool CjFinishRun(int* stage, const CjReleasePlan& plan, const CjReleaseOps& ops);

#endif
