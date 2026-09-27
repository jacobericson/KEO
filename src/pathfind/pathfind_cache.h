// pathfind_cache.h -- path-request priority tier state (Layer 3)
// Depends on: config.h

#ifndef KENSHI_ZONE_OPT_PATHFIND_CACHE_H
#define KENSHI_ZONE_OPT_PATHFIND_CACHE_H

#include "base/config.h"


// =========================================================================
// Priority boost state (requestPath and pathReqSubmit run on the
// main thread for a player's own order, and on the AI back thread whenever
// character multithreading is on -- see hook_requestPath)
// =========================================================================

// The tier hook_requestPath picked for the request hook_pathReqSubmit is
// about to write. Thread-local: requestPath can run on the main thread or
// the AI back thread depending on the caller, and a plain global hand-off
// between hook_requestPath and hook_pathReqSubmit would let one thread's
// tier leak into the other's submit. 0 = leave the game's own priority alone.
extern __declspec(thread) int squadBoostTier;

// Which rule set squadBoostTier: true for a mid-walk re-request matched
// against the published player-owned set (player_repath_tier.h), false for
// the order-priority path. Read once by hook_pathReqSubmit to split the
// boost counter below.
extern __declspec(thread) bool squadBoostFromRepathTier;

extern volatile long p12DiagSubmitBoostsOrder;   // priority>=2 (the order itself)
extern volatile long p12DiagSubmitBoostsState6;  // mid-walk re-request match

void LogPhase12Stats(double now);


#endif // KENSHI_ZONE_OPT_PATHFIND_CACHE_H
