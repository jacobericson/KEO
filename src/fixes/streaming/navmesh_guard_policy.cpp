#include "fixes/streaming/navmesh_guard_policy.h"

NavMeshGuardOutcome NavMeshGuardDecide(const NavMeshGuardFacts& facts)
{
	if (facts.inChangeCs)
	{
		if (!facts.changeMutexRead)
			return NMGUARD_DECLINE_LOCK_UNREADABLE;
		if (!facts.changeMutexHeld)
			return NMGUARD_DECLINE_LOCK_FREE;
		return NMGUARD_LATCH;
	}

	if (!facts.unwindOk)
		return NMGUARD_DECLINE_UNWIND_FAILED;

	return NMGUARD_DECLINE_OUTSIDE_CS;
}

const char* NavMeshGuardOutcomeName(NavMeshGuardOutcome outcome)
{
	switch (outcome)
	{
	case NMGUARD_LATCH:                   return "latch-in-change-cs";
	case NMGUARD_DECLINE_OUTSIDE_CS:      return "declined-outside-cs";
	case NMGUARD_DECLINE_LOCK_FREE:       return "declined-lock-free";
	case NMGUARD_DECLINE_LOCK_UNREADABLE: return "declined-lock-unreadable";
	case NMGUARD_DECLINE_UNWIND_FAILED:   return "declined-unwind-failed";
	}
	return "declined-unclassified";
}
