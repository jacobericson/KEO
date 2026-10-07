#ifndef KEO_FIXES_THROWOUT_INTERNAL_H
#define KEO_FIXES_THROWOUT_INTERNAL_H

#include "fixes/guard_report.h"

// Shared between the throw-out drop side (throwout.cpp) and its finder (throwout_finder.cpp).

namespace fixes {

// Any thread: the in-game hours (GameWorld::getTimeStamp_inGameHours). Valid once
// InstallThrowout has checked and resolved the clock, which it does before the finder installs.
double ThrowoutNowHours();
// Main thread, from InstallThrowout: checks the finder's callees and installs its row. NULL on
// success, else the refusal's reason (a literal).
const char* ThrowoutFinderInstall(int* installed);
// Main thread, ThrowoutTick's first call only: chain the original instead of replacing it.
void ThrowoutFinderChainOriginal(bool on);
// Main thread: the finder's counters, for the heartbeat; *count receives the row count.
const GuardCounter* ThrowoutFinderCounterRows(int* count);

} // namespace fixes

#endif // KEO_FIXES_THROWOUT_INTERNAL_H
