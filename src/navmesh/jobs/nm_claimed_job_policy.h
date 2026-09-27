// nm_claimed_job_policy.h - Pure release plan for a claimed NavMesh job. No game or platform types.
#ifndef KENSHI_ZONE_OPT_NM_CLAIMED_JOB_POLICY_H
#define KENSHI_ZONE_OPT_NM_CLAIMED_JOB_POLICY_H

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

#endif
