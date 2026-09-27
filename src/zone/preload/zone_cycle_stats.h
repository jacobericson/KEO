#ifndef ZONE_CYCLE_STATS_H
#define ZONE_CYCLE_STATS_H

#include "base/config.h"

// Measurement of the zone manager's loading cycles. Every entry point is main
// thread only (processLoading has one caller, ZoneManager::updateMainThread)
// and every one is a no-op unless ZONEHAND_STEP >= 1 and zoneCycleStats is on.

#if ZONEHAND_STEP >= 1

// One processLoading call: the loadingPhase either side of it and how long the
// original took. Closes, opens and times cycles, and prints the per-cycle line.
void ZoneCycleOnProcessLoading(void* zoneMgr, int phaseBefore, int phaseAfter, double callMs);

// The escape-menu pause guard took a pause back. Counted, not logged (after
// the first); the count rides the per-cycle line.
void ZoneCyclePauseRestored();

// Set B's own size (activeZones.table_.size_), the divisor the ambient spawn
// deficit is divided by. -1 when it cannot be read.
int ZoneCycleSetBSize(void* zoneMgr);

// Periodic sample of player-faction characters standing in a cell that reads
// +176/+177 = 1/0. Prints at most one line per sample interval.
void ZoneCycleSampleLeases(void* zoneMgr, double now);

#else

inline void ZoneCycleOnProcessLoading(void*, int, int, double) {}
inline void ZoneCyclePauseRestored() {}
inline int  ZoneCycleSetBSize(void*) { return -1; }
inline void ZoneCycleSampleLeases(void*, double) {}

#endif // ZONEHAND_STEP >= 1

#endif // ZONE_CYCLE_STATS_H
