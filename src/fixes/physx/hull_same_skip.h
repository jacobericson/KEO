#ifndef KEO_FIXES_HULL_SAME_SKIP_H
#define KEO_FIXES_HULL_SAME_SKIP_H

// The click-hull same-target skip. At startup PhysicsHullT's
// vtable slot 5 (the apply the physics thread calls on every registered hull,
// each frame) is swapped for a detour, refused unless the slot holds the
// expected jmp thunk to the apply; the slot is never restored. With
// hullSameSkip off the detour forwards through the saved thunk and does
// nothing else. On, it skips an apply whose target is bit for bit the one last
// forwarded for the same hull and actor, and forwards every create, teleport
// and changed target. The detour takes no lock, allocates nothing and logs
// nothing. A main-thread tick hands the key to the physics thread, requests a
// table clear on each switch to on and at save load. DEV builds also record
// counters and write the HullSame: line.
void InstallHullSameSkip(int* installed, int*);

// Main thread, every frame, save load included.
void HullSameSkipTick(double now, bool saveLoading);

#endif // KEO_FIXES_HULL_SAME_SKIP_H
