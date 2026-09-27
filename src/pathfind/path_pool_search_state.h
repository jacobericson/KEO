// path_pool_search_state.h - search, slow-window and NPC wait counters.
// Included through path_pool_internal.h; each counter has one owning definition.

#ifndef KENSHI_ZONE_OPT_PATH_POOL_SEARCH_STATE_H
#define KENSHI_ZONE_OPT_PATH_POOL_SEARCH_STATE_H

#include "pathfind/path_pool_internal.h"

namespace path_pool_detail {
extern volatile LONG g_gateSearchCount;
extern volatile LONG g_gateIterLimit;
extern volatile LONG g_gateStateFull;
extern volatile LONG g_slowResetRequested;
void PPSlowConsider(LONGLONG svcUs, LONG status, LONG priority, LONG iterations, float sx, float sz, float gx, float gz);
int PPSlowSnapshot(PPSlowEntry* out);
extern PPClassStats g_classStats[PP_CLASS_COUNT];
extern volatile LONG g_pathSearchOk;
extern volatile LONG g_pathSearchFail;
extern volatile LONG g_pathTermIterLimit;
extern volatile LONG g_pathTermOpenSetFull;
extern volatile LONG g_pathTermStateFull;
extern volatile LONG g_pathTermOther;
extern volatile LONG g_boostByTag[3][2];
extern volatile LONG g_boostByReq[3][3];
extern volatile LONG g_boostDisagree;
extern PPHist g_finishedWaitHist;
extern volatile LONG g_reissueSamples;
extern volatile LONG g_reissueSamplesPlayer;
extern NpcWaitWalkResult g_lastWalk;
extern bool g_haveLastWalk;
} // namespace path_pool_detail

#endif // KENSHI_ZONE_OPT_PATH_POOL_SEARCH_STATE_H
