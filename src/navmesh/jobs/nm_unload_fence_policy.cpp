// nm_unload_fence_policy.cpp - The mod-unload fence's order. No game or platform types.
#include "navmesh/jobs/nm_unload_fence_policy.h"

static NmFenceResult NmFenceBeginOnce(const NmFenceOps& ops)
{
	long job0 = ops.skipJobCount(ops.ctx);
	long claim0 = ops.skipClaimCount(ops.ctx);
	int begun = ops.begin(ops.ctx, NM_FENCE_MODE_FULL);
	if (begun == NM_FENCE_BEGIN_UNAVAILABLE)
		return NM_FENCE_UNAVAILABLE;
	if (begun != NM_FENCE_BEGIN_OK)
	{
		if (ops.skipJobCount(ops.ctx) != job0)
			return NM_FENCE_REFUSED_JOB;
		if (ops.skipClaimCount(ops.ctx) != claim0)
			return NM_FENCE_REFUSED_CLAIM;
		return NM_FENCE_REFUSED;
	}
	int pj = ops.tryPj(ops.ctx);
	if (pj == NM_FENCE_PJ_TIMEOUT)
	{
		ops.end(ops.ctx);
		ops.requestPriority(ops.ctx);
		return NM_FENCE_DEFER_PJ;
	}
	return pj == NM_FENCE_PJ_HELD ? NM_FENCE_HELD : NM_FENCE_NO_LOCK;
}

// One operation: the begin names its own refusal, and nothing here reads a
// count or touches processJobCS.
static NmFenceResult NmFenceBeginClaims(const NmFenceOps& ops)
{
	switch (ops.begin(ops.ctx, NM_FENCE_MODE_CLAIMS))
	{
	case NM_FENCE_BEGIN_OK:          return NM_FENCE_CLAIMS_ONLY;
	case NM_FENCE_BEGIN_CLAIMED:     return NM_FENCE_REFUSED_CLAIM;
	case NM_FENCE_BEGIN_IDLE:        return NM_FENCE_IDLE;
	case NM_FENCE_BEGIN_UNAVAILABLE: return NM_FENCE_UNAVAILABLE;
	default:                         return NM_FENCE_REFUSED;
	}
}

NmFenceResult NmFenceTryBegin(NmFenceResult* r, const NmFenceOps& ops, NmFenceMode mode)
{
	if (*r != NM_FENCE_RELEASED)
		return NM_FENCE_REFUSED;
	*r = mode == NM_FENCE_MODE_CLAIMS ? NmFenceBeginClaims(ops) : NmFenceBeginOnce(ops);
	return *r;
}

void NmFenceRelease(NmFenceResult* r, const NmFenceOps& ops)
{
	if (*r == NM_FENCE_HELD)
		ops.unlockPj(ops.ctx);
	if (NmFenceProceeds(*r))
		ops.end(ops.ctx);
	*r = NM_FENCE_RELEASED;
}
