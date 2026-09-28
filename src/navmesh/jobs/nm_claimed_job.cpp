// nm_claimed_job.cpp - ClaimedJob's release checkpoints bound to the game operations. NavMesh bg and worker threads.
#include "navmesh/jobs/nm_claimed_job.h"
#include "navmesh/nm_workers_internal.h"

ClaimedJob::ClaimedJob() : job(0), jobType(0), claimQpc(0), resetRaises(0), claimSlot(-1), clone(NULL)
{
	memset(&pendingWrite, 0, sizeof(pendingWrite));
}

using namespace nm_workers_detail;
namespace nm_claimed_job_detail {
struct CjGameContext
{
	ClaimedJob* claimed;
};

static void CjGameWrite(void* context)
{
	ClaimedJob* claimed = ((CjGameContext*)context)->claimed;
	LARGE_INTEGER tW0, tW1;
	QueryPerformanceCounter(&tW0);
	WriteDiskCacheBlob(&claimed->pendingWrite);
	QueryPerformanceCounter(&tW1);
	long writeUs = (long)(QPCToMs(tW0, tW1) * 1000.0);
	InterlockedExchangeAdd(&navmesh::g_nmCache.nmDiskWriteUsTimes1, writeUs);
}
static bool CjGameCleanupBegin(void*) { return WorkerCleanupBegin(); }
static void CjGameFreeClone(void* context)
{
	FreeClonedNMG(((CjGameContext*)context)->claimed->clone);
}
static void CjGameCleanupEnd(void*) { WorkerCleanupEnd(); }
static void CjGameClearClaimSlot(void* context)
{
	ClaimZoneClear(((CjGameContext*)context)->claimed->claimSlot);
}
static void CjGameAdjFinished(void*) { NmAdjOwnFinished(); }
static void CjGameAdjDropped(void*) { NmAdjOwnDropped(); }
static void CjGameLeaveBusy(void*) { WorkerBusyLeave(); }
static void CjGameIdle(void*) { NoteWorkerPhase(WPHASE_IDLE); }

static CjReleaseOps CjGameOps(CjGameContext* context)
{
	CjReleaseOps ops = { context, &CjGameWrite, &CjGameCleanupBegin,
	                     &CjGameFreeClone, &CjGameCleanupEnd, &CjGameClearClaimSlot,
	                     &CjGameAdjFinished, &CjGameAdjDropped, &CjGameLeaveBusy,
	                     &CjGameIdle };
	return ops;
}
} // namespace nm_claimed_job_detail

using namespace nm_claimed_job_detail;

void ClaimedJobWritePending(ClaimedJob* claimed)
{
	CjReleaseFacts facts = { claimed->pendingWrite.data != NULL, false, false };
	nm_claimed_job_detail::CjGameContext context = { claimed };
	CjRunReleases(CjReleasePlanFor(CJ_RELEASE_PIPELINE_WRITE, facts), nm_claimed_job_detail::CjGameOps(&context));
}

void ClaimedJobReleaseClone(ClaimedJob* claimed)
{
	CjReleaseFacts facts = { false, claimed->clone != NULL, false };
	nm_claimed_job_detail::CjGameContext context = { claimed };
	CjRunReleases(CjReleasePlanFor(CJ_RELEASE_WORKER_CLONE, facts), nm_claimed_job_detail::CjGameOps(&context));
}

void ClaimedJobFinish(ClaimedJob* claimed, CjReleasePoint point, bool bgAdj)
{
	CjReleaseFacts facts = { false, false, bgAdj };
	nm_claimed_job_detail::CjGameContext context = { claimed };
	CjRunReleases(CjReleasePlanFor(point, facts), nm_claimed_job_detail::CjGameOps(&context));
}
