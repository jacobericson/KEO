#ifndef KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_PROBE_H
#define KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_PROBE_H

// Pass-through counter on loadPhysXResource (0x7E4850), DEV builds only.
//
// The NxuStream string-intern pool this call reaches publishes a new node to
// its list head before writing the node's key pointer, with no lock and no
// barrier. Whether that window is reachable depends entirely on whether two
// threads are ever inside the pool at once, and no dump so far has been able
// to answer that: the question needs a live count, not more static reading.
//
// So this detour answers exactly three things and changes nothing:
//
//   * how deep the call can nest across threads, and which callers were
//     inside together when it did -- a max depth of 1 over a whole session
//     kills the concurrency explanation outright, and a depth of 2 naming two
//     different callers confirms the window is reachable;
//   * how many distinct resources, scales, and (resource, scale) pairs the
//     session offers, which is the growth cardinality of the pool, because
//     the engine interns a name built from the scale's decimal spelling;
//   * whether our detour is the outermost one on the site, since a return
//     address from outside the executable means another module's detour wraps
//     ours and the counts would then omit whatever it answers itself.
//
// Callers are attributed by return address, not by thread id: all four call
// sites are direct and known, while the thread that matters most here has no
// identity the plugin already latches and would land in an "unknown" bucket.
//
// Inside the pass-through: interlocked operations only, no lock, no
// allocation, no logging, no CRT stream. Reporting happens on the main
// thread, from the tick below.
void InstallPhysXPoolProbe(int* installed, int*);

// Main thread, every frame. Prints the readout periodically; a no-op in a
// build without the probe.
void PhysXPoolTick(double now);

#endif // KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_PROBE_H
