// island_overlay_internal.h - Private component-overlay state and entry points.
#ifndef KENSHI_ZONE_OPT_ISLAND_OVERLAY_INTERNAL_H
#define KENSHI_ZONE_OPT_ISLAND_OVERLAY_INTERNAL_H
#include "movement/islands_internal.h"

namespace islands_detail {

// The hook census: the isInIsland and getIsland hooks write it on any thread,
// Interlocked* only, and the main-thread Islands: and IslandSpan: reporters
// read it. Cumulative and never reset; each counter is read on its own, so
// mixed values across counters are a tolerated diagnostic. g_hooksInstalled
// is set once by the hook install on the main thread and read on any thread.
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

#endif // KENSHI_ZONE_OPT_ISLAND_OVERLAY_INTERNAL_H
