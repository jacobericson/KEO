// nm_claimed_job_policy.cpp - Pure release plan and runner. No game or platform types.
#include "navmesh/jobs/nm_claimed_job_policy.h"

CjReleasePlan CjReleasePlanFor(CjReleasePoint point, const CjReleaseFacts& facts)
{
	CjReleasePlan plan = { false, false, CJ_TAIL_NONE, false };
	switch (point)
	{
	case CJ_RELEASE_PIPELINE_WRITE:
		plan.writePending = facts.hasPendingData;
		break;
	case CJ_RELEASE_WORKER_CLONE:
		plan.releaseClone = facts.hasClone;
		break;
	case CJ_FINISH_WORKER:
		plan.tail = CJ_TAIL_WORKER;
		break;
	case CJ_FINISH_BG_CONTENT_LOST:
		plan.tail = CJ_TAIL_BG_DROP;
		plan.bgAdj = facts.bgAdj;
		break;
	case CJ_FINISH_BG_PIPELINE_RETURN:
		plan.tail = CJ_TAIL_BG_FINISHED;
		plan.bgAdj = facts.bgAdj;
		break;
	default:
		break;
	}
	return plan;
}

// This same runner is bound to the game operations by nm_claimed_job.cpp and
// to a fixed event recorder by the host suite. Permission is read live at each
// independent worker checkpoint; a denied clone release is never retried here.
void CjRunReleases(const CjReleasePlan& plan, const CjReleaseOps& ops)
{
	if (plan.writePending)
		ops.writePending(ops.ctx);

	if (plan.releaseClone)
	{
		if (ops.cleanupBegin(ops.ctx))
		{
			ops.freeClone(ops.ctx);
			ops.cleanupEnd(ops.ctx);
		}
	}

	if (plan.tail == CJ_TAIL_WORKER)
	{
		ops.clearClaimSlot(ops.ctx);
		if (ops.cleanupBegin(ops.ctx))
		{
			ops.adjFinished(ops.ctx);
			ops.leaveBusy(ops.ctx);
			ops.cleanupEnd(ops.ctx);
		}
		ops.idle(ops.ctx);
	}
	else if (plan.tail == CJ_TAIL_BG_DROP)
	{
		if (plan.bgAdj)
			ops.adjDropped(ops.ctx);
		ops.clearClaimSlot(ops.ctx);
		ops.leaveBusy(ops.ctx);
	}
	else if (plan.tail == CJ_TAIL_BG_FINISHED)
	{
		if (plan.bgAdj)
			ops.adjFinished(ops.ctx);
		ops.clearClaimSlot(ops.ctx);
		ops.leaveBusy(ops.ctx);
	}
}
