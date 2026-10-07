#ifndef KEO_FIXES_LIGHT_CACHE_H
#define KEO_FIXES_LIGHT_CACHE_H

// The light-level cache, DEV builds only: an entry detour on
// GameWorld::getLightLevel, the night light walk Character::periodicUpdate
// runs for a character something asked about. Its one caller runs on the
// main thread, at most eight characters a frame.
//
// lightCache, live in the KEO tab and off by default: shadow runs every call
// and counts how often the exact key, and keys on 0.5- and 2-unit cells,
// would have found a value and how far that value was from the original's;
// on serves an exact-key hit, only while no character carries a light. The
// key is every input of the result (the position's bits, the floor, the
// outdoors argument, the sky's ambient factor) plus an epoch that moves when
// the loaded-cell set changes, on a save load and on a mode change. An entry
// lives 10 s, the bound on a building light whose power changed.
//
// The detour takes no lock, allocates nothing and logs nothing; the tables
// are static. A PROD build has no detour, and both functions are empty there.
void InstallLightCache(int* installed, int*);

// Main thread, every frame: the mode and save-load edges, and the LightCache:
// line every 60 s and at once on a mode change.
void LightCacheTick(double now, bool saveLoading);

#endif // KEO_FIXES_LIGHT_CACHE_H
