#include <cstdio>
#include <cstring>
#include "fixes/streaming/navmesh_guard_policy.h"

#include "check.h"

static NavMeshGuardFacts Facts(bool unwindOk, bool inChangeCs, bool read, bool held)
{
	NavMeshGuardFacts f;
	f.unwindOk        = unwindOk;
	f.inChangeCs      = inChangeCs;
	f.changeMutexRead = read;
	f.changeMutexHeld = held;
	return f;
}

int main()
{
	// The one outcome that leaves a lock held: a frame inside the locked region
	// proves this thread is the owner, and the word confirms the lock is set.
	Check(NavMeshGuardDecide(Facts(true, true, true, true)) == NMGUARD_LATCH,
	      "a fault inside the change section on a held lock latches");

	// The walk is what proves ownership, and it proves it the moment it sees a
	// region frame. A walk that faulted afterwards has not retracted that, so it
	// is classified on the proof it did produce.
	Check(NavMeshGuardDecide(Facts(false, true, true, true)) == NMGUARD_LATCH,
	      "a partial walk that already found a region frame still latches");

	// No region frame: the fault belongs to some other crash, and this guard has
	// nothing to say about the lock.
	Check(NavMeshGuardDecide(Facts(true, false, true, true)) == NMGUARD_DECLINE_OUTSIDE_CS,
	      "a fault outside the change section is declined even with the lock held");
	Check(NavMeshGuardDecide(Facts(true, false, true, false)) == NMGUARD_DECLINE_OUTSIDE_CS,
	      "a fault outside the change section is declined");

	// A walk that faulted without reaching a region frame proves nothing either
	// way, and says so rather than borrowing the outside-CS answer.
	Check(NavMeshGuardDecide(Facts(false, false, true, false)) == NMGUARD_DECLINE_UNWIND_FAILED,
	      "a failed walk with no region frame is its own outcome");
	Check(NavMeshGuardDecide(Facts(false, false, true, true)) == NMGUARD_DECLINE_UNWIND_FAILED,
	      "a failed walk is not rescued by the lock word");

	// Inside the region with the lock not actually held is not the orphaned-lock
	// family, and must not be reported as a lock left held.
	Check(NavMeshGuardDecide(Facts(true, true, true, false)) == NMGUARD_DECLINE_LOCK_FREE,
	      "a free lock inside the change section is declined");

	// "Could not read the lock word" and "the lock is free" are different facts
	// for whoever reads the record, so they are different outcomes.
	Check(NavMeshGuardDecide(Facts(true, true, false, false)) == NMGUARD_DECLINE_LOCK_UNREADABLE,
	      "an unreadable lock word inside the change section is its own outcome");

	// Every outcome carries a distinct, non-empty token: the record is the only
	// place the decision survives.
	{
		NavMeshGuardOutcome all[] = { NMGUARD_LATCH,
		                              NMGUARD_DECLINE_OUTSIDE_CS,
		                              NMGUARD_DECLINE_LOCK_FREE,
		                              NMGUARD_DECLINE_LOCK_UNREADABLE,
		                              NMGUARD_DECLINE_UNWIND_FAILED };
		const int n = (int)(sizeof(all) / sizeof(all[0]));
		for (int i = 0; i < n; ++i)
		{
			const char* a = NavMeshGuardOutcomeName(all[i]);
			Check(a != 0 && a[0] != '\0', "every outcome has a name");
			for (int j = i + 1; j < n; ++j)
				Check(strcmp(a, NavMeshGuardOutcomeName(all[j])) != 0,
				      "outcome names are distinct");
		}
		Check(strcmp(NavMeshGuardOutcomeName(NMGUARD_LATCH), "latch-in-change-cs") == 0,
		      "the latch token names the action and the reason");
	}

	return CheckExit("navmesh_guard_units");
}
