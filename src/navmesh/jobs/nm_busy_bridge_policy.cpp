#include "navmesh/jobs/nm_busy_bridge_policy.h"

bool BusyBridge(const BusyBridgeOps& ops, BusyBridgeOp op, bool lockHeld)
{
	if (op == BUSY_BRIDGE_CLEAR_IF_IDLE && !lockHeld && !ops.readFlag(ops.ctx))
		return true;

	const bool takeLock = !lockHeld;
	if (takeLock)
		ops.lockQueue(ops.ctx);

	switch (op)
	{
	case BUSY_BRIDGE_ENTER:
		ops.increment(ops.ctx);
		ops.writeFlag(ops.ctx, 1);
		break;
	case BUSY_BRIDGE_LEAVE:
		if (ops.decrement(ops.ctx) == 0)
			ops.writeFlag(ops.ctx, 0);
		break;
	case BUSY_BRIDGE_CLEAR_IF_IDLE:
		if (ops.readCount(ops.ctx) == 0)
			ops.writeFlag(ops.ctx, 0);
		break;
	}

	const bool held = !(ops.readFlag(ops.ctx) == 0 && ops.readCount(ops.ctx) != 0);
	if (takeLock)
		ops.unlockQueue(ops.ctx);
	return held;
}

bool BusyBridgeLeave(const BusyBridgeOps& ops)
{
	return BusyBridge(ops, BUSY_BRIDGE_LEAVE, false);
}
