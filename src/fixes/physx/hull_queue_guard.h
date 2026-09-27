#ifndef KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_H

// Detour on PhysicsActual::updateUT, the one place hullsToDestroy's main list
// is flushed to the physics thread. Before the flush it drops (sets to NULL)
// every entry that queues an object a second time: already earlier in the
// list, still in an unconsumed back list, or deleted by the physics thread's
// last batch and not made again since. The physics thread would otherwise
// run that object's deleting destructor twice, the second time on freed
// memory. The delete loop skips NULL entries.
//
// Five read-only detours on the virtual pushers (vtable slot 1 of the hull
// classes) record each address's last two callers, so a drop names both.
//
// Installed in every build whenever the prologues verify. hullDoublePushGuard
// chooses only whether a duplicate is dropped or counted and left in place.
// updateUT runs on the main thread with the physics thread idle.
void InstallHullQueueGuard(int* installed, int*);

// Main thread, once per frame: the 60-second heartbeat, printed whether or not
// the site was ever called.
void HullQueueGuardTick(double now);

#endif // KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_H
