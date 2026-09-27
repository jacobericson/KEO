#include "navmesh/jobs/nm_buildlock_stall.h"

const double BUILD_LOCK_STALL_SECONDS  = 5.0;
const long   BUILD_LOCK_STALL_RUN      = 1000;
const int    BUILD_LOCK_STALL_SLEEP_MS = 20;

BuildLockStallAction BuildLockStallDecide(const BuildLockStallInputs& in)
{
	if (in.granted)
		return BL_STALL_RESET;
	if (in.runSeconds < BUILD_LOCK_STALL_SECONDS || in.runLength < BUILD_LOCK_STALL_RUN)
		return BL_STALL_PASS;
	return in.latched ? BL_STALL_THROTTLE : BL_STALL_LATCH;
}

bool BuildLockStallSiteMaySleep(BuildLockStallSite site)
{
	return site == BL_SITE_UNLOAD || site == BL_SITE_CREATE;
}

const char* BuildLockStallSiteName(BuildLockStallSite site)
{
	switch (site)
	{
	case BL_SITE_UPDATE_ADD: return "add";
	case BL_SITE_GEN_SAVE:   return "save";
	case BL_SITE_UNLOAD:     return "unload";
	case BL_SITE_CREATE:     return "create";
	default:                 return "other";
	}
}

BuildLockStallDecision BuildLockStallEvaluate(const BuildLockStallInputs& in, BuildLockStallSite site)
{
	BuildLockStallDecision out;
	out.action = BuildLockStallDecide(in);
	out.sleep  = (out.action == BL_STALL_LATCH || out.action == BL_STALL_THROTTLE)
	          && BuildLockStallSiteMaySleep(site);
	return out;
}
