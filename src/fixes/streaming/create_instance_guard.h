#ifndef KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_H

// Detour on NavMesh::createInstance. When the NavInstance it is handed is
// already in the add list by pointer with a live instance, the original frees
// it and pushes the freed pointer back onto the list, and the add-list drain
// faults reading it. The guard skips that call: the instance is already queued,
// so the drain still adds it.
//
// Installed in every build whenever the prologue verifies. createInstanceGuard
// chooses only whether such a call is skipped or counted and passed through
// unchanged. Path thread only; no lock, no allocation, logging deferred.
void InstallCreateInstanceGuard(int* installed, int*);

// Main thread, once per frame: the 60-second heartbeat, printed whether or not
// the site was ever called.
void CreateInstanceGuardTick(double now);

#endif // KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_H
