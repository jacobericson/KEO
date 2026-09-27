// islands_internal.h - Private glue for island routing and tracking.
// Shared mutable state has one definition in islands.cpp; component-private
// state stays with island_components.cpp. Inline field/grid helpers are in
// islands_inline.h, inside islands_detail.
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


namespace islands_detail {

extern volatile long g_isInCalls;
extern volatile long g_isInFlips;
extern volatile long g_isInSeqFail;
extern volatile long g_isInFarTrue;
extern volatile long g_isInTrueZero;
extern volatile long g_isInTrueLabel;
extern volatile long g_isInMaxSpan;
extern volatile long g_isInSpanUnk;
extern volatile long g_isInVanTrue;
extern volatile long g_isInVanFalse;
extern volatile long g_isInRuleFlip;
extern volatile long g_isInRuleUnk;
extern volatile long g_isInFlipSpan[ISLAND_SPAN_BUCKETS];
extern volatile long g_getIslCalls;
extern volatile long g_getIslAppended;
extern volatile long g_getIslFallback;
extern volatile long g_hooksInstalled;
extern uintptr_t     g_builderZm;
extern unsigned int  g_snapGen;
extern unsigned int  g_setBSig;
extern bool          g_haveSig;
extern bool          g_rebuildRequested;
extern double        g_lastEligibility;
extern int            g_setBAccessible;
extern int            g_curCompCount;
extern int            g_curModZones;

bool SnapReadComps(uintptr_t a, uintptr_t b, int* ca, int* cb);
int SnapCopyMembers(uintptr_t t, int* outComp, uintptr_t* outZm, unsigned short* buf, int maxCount);
bool AppendLektor(uintptr_t lek, uintptr_t z);
void ResetBuilder();
void WalkSetB(uintptr_t zm);
void Rebuild(uintptr_t zm, double now);
int CountUnexplained(uintptr_t zm);
int CurComp(uintptr_t zm, uintptr_t z);
int RouterList(uintptr_t zm, uintptr_t t, unsigned short* out, int maxOut);

} // namespace

#endif // KENSHI_ZONE_OPT_ISLANDS_INTERNAL_H
