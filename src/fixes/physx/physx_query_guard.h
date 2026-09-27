#ifndef KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_H

// Validates every shape GameWorld::getObjectsWithinBox got back from the
// scene's overlap query, immediately before the walk calls getActor() through
// it, and skips an entry that does not survive validation.
//
// The failure this removes is a use-after-free: the query can return a shape
// whose block has already been freed, and calling slot 1 of the abstract
// vtable a destructor leaves behind lands in PhysX's own _purecall, which
// calls abort(). That is a CRT exit, not an SEH exception, so no handler in
// the process can intercept it and no crash record is written.
//
// This is not an ordinary hook. The two instructions to guard sit in the
// middle of the function, so there is no prologue to detour and no original
// to call: the six bytes are replaced with a jump to a stub that validates,
// then either re-issues the original call or jumps to the loop's own
// "next entry" label. The stub has to be within +-2 GB of the exe for its
// rel32 branches to reach, which the mod DLL is not, so the installer
// allocates its own page near the image. Verification is exact and fails
// closed: mid-function, an already-detoured site is not a shared site, it is
// an unknown binary.
//
// Call once from startPlugin. PhysXCore64.dll supplies the module range the
// validation compares against and is usually not loaded yet at that point, so
// a first failure only defers; PhysQueryGuardTick retries.
void InstallPhysQueryGuard(bool enabled);

// Main-thread, once per frame, with the caller's own ElapsedSec() clock.
// Finishes a deferred arm, drains the rejected-vptr reports (module
// resolution needs the main thread), and prints the periodic "PhysQ:" line.
void PhysQueryGuardTick(double now);

// Points the stub's gate slot at an accept-all thunk inside the stub's own
// page, so nothing in the patched path calls into this DLL any more. Call
// from DLL_PROCESS_DETACH. The six patched bytes are deliberately NOT
// restored: a thread can be inside the stub, and there is no way to know it
// is not.
void NeutralizePhysQueryGuard();

#endif // KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_H
