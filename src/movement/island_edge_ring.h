#ifndef KENSHI_ZONE_OPT_ISLAND_EDGE_RING_H
#define KENSHI_ZONE_OPT_ISLAND_EDGE_RING_H

// The ring filter installed inside hook_getIsland (island_hooks.cpp).
// It narrows the island list handed back to NavMesh::getZoneEdge --
// identified by its own return address, not by any new hook site -- to the
// cells within Chebyshev 1 of the character's own cell, so getZoneEdge's ray
// can only find a leg 1-2 cells long instead of one spanning the whole
// island.
//
// Depends on: config_values.h (islandEdgeRingEnabled).
//
// Threading contract, same as the rest of island_hooks.cpp's hooks: called from
// hook_getIsland on any thread that reaches ZoneManager::getIsland (main, AI,
// the smell picker). No lock, no allocation, no logging; it only shrinks the
// lektor's own count, never its capacity, and it grows nothing.

#include "base/config.h"
#include <sstream>


// Armed once at install, whenever both island hooks installed -- independent
// of islandEdgeRingEnabled, so the key never disables the counters, only the
// write. Also independent of IslandHooksLive()/islandFix, so either being
// off can never silently disable this filter's counters either.
void IslandEdgeRingArm(bool installed);
bool IslandEdgeRingArmed();

// Called once per hook_getIsland invocation, right before it returns, with
// the caller's own return address (_ReturnAddress(), captured at the hook's
// entry so it survives any nested calls), the ZoneManager*, the queried zone
// t, and the lektor<ZoneMap*>* that already holds the answer (from the
// original, the overlay's append, or both). Classifies the return address on
// every call; compacts the list in place only for the getZoneEdge caller,
// while armed, with a resolvable start cell, a bounded list, and every
// in-grid neighbour of the start cell present in it.
void IslandEdgeRingApply(void* ra, uintptr_t zm, uintptr_t t, uintptr_t lektorOut);

void IslandEdgeRingAppendSummary(std::ostringstream& ss);   // IslandSpan: suffix


// Always available (reads islandEdgeRingEnabled directly), for the startup
// banner -- mirrors how islandFarSpan's own value prints unconditionally.
// Returns "true"/"false".
const char* IslandEdgeRingModeStr();

#endif // KENSHI_ZONE_OPT_ISLAND_EDGE_RING_H
