#include "zone/reset/zone_reset_sequence.h"

// processJobCS keeps a job's setup and its tail off the zones the unload frees,
// but a generation released from the lock runs outside it. The drain is taken
// first, so no released generation is still reading a zone while the lock is
// held; with either one missing, only a claimed survivor is left loaded.

namespace zone_reset_sequence_detail
{
	// Ends the drain on every exit, after the lock's guard has run.
	struct DrainGuard
	{
		const ZoneResetFenceOps* ops;
		explicit DrainGuard(const ZoneResetFenceOps* o) : ops(o) {}
		~DrainGuard() { ops->drainEnd(ops->ctx); }
	private:
		DrainGuard(const DrainGuard&);
		DrainGuard& operator=(const DrainGuard&);
	};

	// Releases processJobCS on every exit, and only when it was taken.
	struct LockGuard
	{
		const ZoneResetFenceOps* ops;
		bool held;
		LockGuard(const ZoneResetFenceOps* o, bool h) : ops(o), held(h) {}
		~LockGuard() { if (held) ops->unlock(ops->ctx); }
	private:
		LockGuard(const LockGuard&);
		LockGuard& operator=(const LockGuard&);
	};
}
using namespace zone_reset_sequence_detail;

void ZoneResetRunFence(const ZoneResetFenceOps* ops, unsigned totalMs, unsigned floorMs,
                       bool unloadSurvivors, ZoneResetFenceOutcome* out)
{
	out->drained       = false;
	out->drainMs       = 0;
	out->lock          = ZONE_RESET_LOCK_NONE;
	out->lockWaitMs    = 0;
	out->fenceComplete = false;
	out->survivors     = 0;
	out->kept          = 0;

	unsigned drainMs = 0;
	out->drained = ops->drainBegin(ops->ctx, totalMs, &drainMs);
	out->drainMs = drainMs;
	DrainGuard drain(ops);

	unsigned lockWaitMs = 0;
	out->lock = ops->lock(ops->ctx, ZoneResetLockBudgetMs(totalMs, drainMs, floorMs), &lockWaitMs);
	out->lockWaitMs = lockWaitMs;
	LockGuard lock(ops, out->lock == ZONE_RESET_LOCK_HELD);
	out->fenceComplete = out->drained && lock.held;

	ops->nativeUnload(ops->ctx);

	int survivors = ops->collectSurvivors(ops->ctx);
	out->survivors = survivors;
	if (!unloadSurvivors)
		return;
	for (int i = 0; i < survivors; ++i)
	{
		// With the fence complete no claim can be running, so the claim is
		// only asked for when it can change the answer.
		bool claimed = !out->fenceComplete && ops->survivorClaimed(ops->ctx, i);
		if (ZoneResetDecideSurvivor(out->fenceComplete, claimed) == ZONE_RESET_SKIP_CLAIMED)
		{
			ops->keepSurvivor(ops->ctx, i);
			out->kept++;
		}
		else
			ops->unloadSurvivor(ops->ctx, i);
	}
}
