#ifndef KEO_FIXES_CREATE_INSTANCE_GUARD_H
#define KEO_FIXES_CREATE_INSTANCE_GUARD_H

// Detour on NavMesh::createInstance. The guard skips two redundant calls, and
// in both n keeps the instance it has. One hands over a NavInstance already in
// the add list by pointer with a live instance: the original frees it and
// pushes the freed pointer back, and the add-list drain faults reading it. The
// other hands over a NavInstance that is not queued but whose instance is
// already in the world: the original replaces the instance without taking the
// old one out, the drain refuses the new one as a duplicate uid, and the old
// one keeps its collection slot after n's teardown deletes what it uses.
//
// Installed in every build whenever the prologue verifies. createInstanceGuard
// chooses only whether such a call is skipped or counted and passed through
// unchanged. Path thread only; no lock, no allocation, logging deferred.
void InstallCreateInstanceGuard(int* installed, int*);

// Main thread, once per frame: the 60-second heartbeat, printed whether or not
// the site was ever called.
void CreateInstanceGuardTick(double now);

#endif // KEO_FIXES_CREATE_INSTANCE_GUARD_H
