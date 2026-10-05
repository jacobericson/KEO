#ifndef KEO_ZONE_EXPIRY_GUARD_H
#define KEO_ZONE_EXPIRY_GUARD_H

#include "base/config.h"
#include <string>

// The expiry guard: the ZoneMap::update prologue's fence for an expiring Set B
// cell that retention does not take, which is every expiring cell while
// retention is off. The original's deactivate frees the cell's content, and a
// navmesh job that claimed the cell reads that content with no lock, so the
// cell is held, one expiry at a time, for as long as a claim names it;
// otherwise the original runs with the unload publication up, so no claim can
// start for the cell while it does. A hold is a write to the town countdown
// and nothing else, and it never ends on a timeout.
//
// Main thread only.

#if ZONEHAND_STEP >= 3

typedef bool (__fastcall *ZoneMapUpdateFn)(void* zoneEntry);

void ZoneExpiryGuardInit();
// Hold stamps belong to the world that made them; the counters run on.
void ZoneExpiryGuardOnWorldReset();
void ZoneExpiryGuardBeginFrame();

// For a cell whose countdowns run out this frame and that retention does not
// take: holds it, or calls `original` with or without the publication.
// Returns the original's answer.
bool ZoneExpiryGuardUpdate(void* zoneEntry, ZoneMapUpdateFn original);

// " expGuard=<holds>/<fenced>/<idle>/<unfenced> maxHoldS=<s> maxFencedPerFrame=<n>"
std::string ZoneExpiryGuardStatsFragment();

#endif // ZONEHAND_STEP >= 3

#endif // KEO_ZONE_EXPIRY_GUARD_H
