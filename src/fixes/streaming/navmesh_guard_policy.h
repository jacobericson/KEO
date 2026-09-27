#ifndef KENSHI_ZONE_OPT_FIXES_NAVMESH_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_NAVMESH_GUARD_POLICY_H

// The classification behind the NavMesh::update crash guard
// (navmesh_update_guard.h). Pure, no Windows header and no I/O, so it is
// host-testable under tools/tests/ and cannot drift from the rule the fault
// path actually applies: the filter calls nothing else to decide.

// What the guard concludes about a fault. Every outcome lets the fault stand:
// the guard records and classifies, it never releases a lock and never resumes
// the thread. The outcomes differ in what they assert about the SectionManager's
// change lock, which is the one thing a later reader of the record cannot
// reconstruct from anywhere else.
//
// A latch is deliberate: the faulting thread is known to hold changeMutex
// exclusively, and leaving it held is what keeps every other reader out of the
// data the fault just tripped over.
enum NavMeshGuardOutcome
{
	NMGUARD_LATCH,                   // proven owner of a held changeMutex: leave it held
	NMGUARD_DECLINE_OUTSIDE_CS,      // no frame inside the locked region: a different crash
	NMGUARD_DECLINE_LOCK_FREE,       // inside the region, yet the lock word is not held
	NMGUARD_DECLINE_LOCK_UNREADABLE, // inside the region, lock word unreadable
	NMGUARD_DECLINE_UNWIND_FAILED    // the walk faulted without reaching a region frame
};

// What the frame walk and the guarded reads established about the faulting
// thread, at the instant of the fault.
struct NavMeshGuardFacts
{
	bool unwindOk;        // the frame walk ran to completion without faulting
	bool inChangeCs;      // a walked frame sits inside update's changeMutex region
	bool changeMutexRead; // the mutex word was readable
	bool changeMutexHeld; // ... and carried the exclusive bit
};

// inChangeCs is the ownership proof and it is monotone: the walk sets it the
// moment it sees a frame in the region, so a walk that faulted afterwards has
// still proved ownership and is classified on that, not on its own failure.
NavMeshGuardOutcome NavMeshGuardDecide(const NavMeshGuardFacts& facts);

// The token the record carries for an outcome. Static storage, no allocation:
// the caller writes it from inside an exception filter.
const char* NavMeshGuardOutcomeName(NavMeshGuardOutcome outcome);

#endif // KENSHI_ZONE_OPT_FIXES_NAVMESH_GUARD_POLICY_H
