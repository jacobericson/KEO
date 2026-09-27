#ifndef KENSHI_ZONE_OPT_ZONE_LIFECYCLE_HOOKS_H
#define KENSHI_ZONE_OPT_ZONE_LIFECYCLE_HOOKS_H

#include "base/config.h"

// The detours that carry zone ownership: one on ZoneManager::activateZoneMap,
// which both takes a privately prepared cell over when real demand asks for
// it and refuses the town refresh that would otherwise renew a town's whole
// coverage from nothing, and one on ZoneMap::update, which holds a cell the
// game has just taken over instead of letting it expire on the first pass
// that reaches it.
//
// Adds to *installed the number of detours that went in. Main thread,
// startPlugin.
void InstallZoneLifecycleHooks(int* installed, int*);

#endif
