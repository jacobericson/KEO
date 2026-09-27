// nm_adjacency_counters.h - Adjacency counters, one definition each in nm_adjacency.cpp. Interlocked, any thread.
#ifndef KENSHI_ZONE_OPT_NM_ADJACENCY_COUNTERS_H
#define KENSHI_ZONE_OPT_NM_ADJACENCY_COUNTERS_H
#include <windows.h>

namespace nm_adjacency_detail {
extern volatile LONG s_calls;
extern volatile LONG s_claims;
extern volatile LONG s_skips;
extern volatile LONG s_skipClaims;
extern volatile LONG s_would;
extern volatile LONG s_deferred;
extern volatile LONG64 s_deferMaxUs;
extern volatile LONG s_bgWaits;
extern volatile LONG64 s_bgWaitUs;
extern volatile LONG64 s_bgWaitMaxUs;
extern volatile LONG s_bgWaitHist[8];
extern volatile LONG s_bgSkip;
extern volatile LONG s_bgIdle;
extern volatile LONG s_bgPassFull;
extern volatile LONG64 s_bgoMaxUs;
extern volatile LONG s_bgoClaims;
extern volatile LONG s_full;
extern volatile LONG s_resAge;
extern volatile LONG s_resDrop;
extern volatile LONG s_obsFreed;
extern volatile LONG s_obsOverflow;
extern volatile LONG s_obsOtherNmg;
extern volatile LONG s_liveMax;
extern volatile LONG s_pubMax;
extern volatile LONG s_checks;
extern volatile LONG s_viol;
extern volatile LONG s_violCell;
extern volatile LONG s_spanOut;
extern volatile LONG s_unowned;
extern volatile LONG s_torn;
extern volatile LONG s_violLines;
} // namespace nm_adjacency_detail
#endif // KENSHI_ZONE_OPT_NM_ADJACENCY_COUNTERS_H
