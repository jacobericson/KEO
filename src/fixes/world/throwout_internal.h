#ifndef KEO_FIXES_THROWOUT_INTERNAL_H
#define KEO_FIXES_THROWOUT_INTERNAL_H

#include "fixes/guard_report.h"
#include <stddef.h>

// Shared between the throw-out drop side (throwout.cpp) and its finder (throwout_finder.cpp).

namespace fixes {

// Any thread: the in-game hours (GameWorld::getTimeStamp_inGameHours). Valid once
// InstallThrowout has checked and resolved the clock, which it does before the finder installs.
double ThrowoutNowHours();
// Main thread, from InstallThrowout: checks the finder's callees and installs its row. NULL on
// success, else the refusal's reason (a literal).
const char* ThrowoutFinderInstall(int* installed);
// Main thread, from InstallThrowout before the row goes in and from ThrowoutTick's first call:
// chain the original instead of replacing it.
void ThrowoutFinderChainOriginal(bool on);
// Main thread, ThrowoutTick's first call in chain mode only: walks the finder's entry chain once
// and records whether a live foreign detour sits in it (until then it counts as live, so a held
// chained result is answered as no candidate). Returns that verdict.
bool ThrowoutFinderDecideFallback();
// Main thread: an SEH-guarded copy of n bytes; false when the source cannot be read.
bool ThrowoutSafeRead(const void* addr, void* out, size_t n);
// Main thread: the finder's counters, for the heartbeat; *count receives the row count.
const GuardCounter* ThrowoutFinderCounterRows(int* count);

} // namespace fixes

#endif // KEO_FIXES_THROWOUT_INTERNAL_H
