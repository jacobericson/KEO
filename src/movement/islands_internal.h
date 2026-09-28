// islands_internal.h - Private glue for island routing and tracking.
// Overlay state and entry points live in island_overlay_internal.h; the
// adapters and tracker entry points remain here. Inline field/grid helpers
// are in islands_inline.h, inside islands_detail.
//
// Used by the entry, components, hooks, stuck diagnostics, reissue tracker
// and edge-ring filter. Shared helpers and state use the named namespace,
// while the adapters and tracker entry points have global linkage.
// Thread and lifetime requirements stay with each declaration or definition.

#ifndef KENSHI_ZONE_OPT_ISLANDS_INTERNAL_H
#define KENSHI_ZONE_OPT_ISLANDS_INTERNAL_H

#include "base/config.h"
#include "movement/island_span_policy.h"
#include <sstream>
#include "movement/islands_inline.h"


// Defined in islands.cpp: adapters for zone accessibility (+177) and the rebuild
// generation / Set B signature the K7 tracker's staleness check compares
// against (o.seenGen/o.seenSig in islands_reissue.cpp).
bool         IslandOverlayZoneAccessible(uintptr_t z);
unsigned int IslandOverlayGen();
unsigned int IslandOverlaySetBSig();

// Defined in islands.cpp: the zone-grid index (idx = gx*64+gy) of a ZoneMap*,
// or -1 if it is not an entry of zm's zone array. island_edge_ring.cpp uses
// it to decode the lektor's ZoneMap* entries (and the ring's start cell) into
// the grid coordinates the pure predicate compares.
int IslandZoneIndexOf(uintptr_t zm, uintptr_t z);


// Defined in islands_reissue.cpp: called from islands.cpp's IslandReset()
// and IslandTick().
void IslandReissueReset();          // IslandTick's save-load path (both tables)
void IslandReissueResetChecks();    // IslandReset()'s path (checks table only)
void IslandReissuePollTick(uintptr_t zm, double now);
// Defined in island_reissue_diag.cpp.
void IslandReissueAppendDiag(std::ostringstream& ss);

// delRefuse=/k7Hold=/gameDropWhy=, appended to the PROD-visible IslandSpan:
// line (islands.cpp LogIslandSpan) as well as the DEV-only Islands: line
// above, since Islands: is compiled out in PROD.
void IslandReissueAppendSpanDiag(std::ostringstream& ss);


#endif // KENSHI_ZONE_OPT_ISLANDS_INTERNAL_H
