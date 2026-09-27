// island_reissue_diag.cpp - main-thread reissue and K7 diagnostic appenders.
// Field and line order are the shipped log format for both output variants; do not reorder.
#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "zone/preload/preload.h"
#include "movement/formation.h"
#include "pathfind/player_task_policy.h"   // PT_OFF_* task-system offsets (for K7)
#include "movement/k7_swap_policy.h"       // K7ClassifySwap / K7SigOnsetStep
#include "movement/k7_arrival_policy.h"    // K7ArrivalShouldArm / K7ArrivalPoll
#include "zone/readiness/zone_readiness_classify.h"  // ClassifyZoneReadiness, ZR_*
#include "movement/order_outcome.h"            // OrderOutcomeNoteReissueSent
#include "movement/island_span_policy.h"       // IslandCellSpan (K7 arrival arm line)
#include "zone/zone_pause.h"               // ZonePauseIsPaused (pause gate)
#include <intrin.h>
#include <cstring>
#include "movement/islands_reissue_internal.h"
#include "movement/islands_reissue_counters.h"
using namespace islands_reissue_detail;
// IslandTick's ZONEOPT_DEBUG "Islands:" line: the reissue/K7 fields.
void IslandReissueAppendDiag(std::ostringstream& ss)
{
	ss << " reissue=" << InterlockedCompareExchange(&g_reissues, 0, 0)
	   << " reissuePost=s" << g_reissuePostSent << "/l" << g_reissuePostLast
	   << "/o" << g_reissuePostOther
	   << " reissueCheckDropped=" << g_reissueCheckDropped
	   << " reissueCheckEarly=" << g_reissueCheckEarly;
	// K7 (all 0 while the deleted-order form is off).
	ss << " delPark=" << g_k7DelPark
	   << " delReissue=" << g_k7DelReissue
	   << " cancelDrop=s" << g_k7CancelStop << "/j" << g_k7CancelJob << "/t" << g_k7CancelTask
	   << " gameDrop=" << g_k7GameDrop
	   << " gameDropWhy=u" << g_k7DropUnconcious
	   << "/h" << g_k7DropOrderHead
	   << "/x" << g_k7DropTaskSwap
	   << "/xo" << g_k7DropTaskSwapNonCombat
	   << "/xe" << g_k7DropTaskSwapExpired
	   << " apdDrop=" << g_k7ApdDrop;
	IslandReissueAppendSpanDiag(ss);
}

// Printed on the PROD-visible IslandSpan: line (islands.cpp) every mode, and
// folded into the DEV Islands: line above (which is compiled out entirely in
// PROD).
void IslandReissueAppendSpanDiag(std::ostringstream& ss)
{
	ss << " delRefuse=h" << g_k7RefuseHyst
	   << "/g" << g_k7RefuseSig
	   << "/d" << g_k7RefuseDest29
	   << "/n" << g_k7RefuseDestNow
	   << "/c" << g_k7RefuseCarried
	   << "/s" << g_k7RefuseInSomething
	   << "/a" << g_k7RefuseHit
	   << "/e" << g_k7RefuseEnemies
	   << "/t" << g_k7RefuseThreats
	   << "/o" << g_k7RefuseHold
	   << "/f" << g_k7RefuseNear
	   << "/z" << g_k7RefuseZones
	   << "/r" << g_k7RefuseDestReady
	   << "/k" << g_k7RefuseCooldown
	   << "/b" << g_k7RefuseBudget
	   << " k7Hold=s" << g_k7HoldStarted << "/r" << g_k7HoldResumedSend
	   << " k7HoldWould=" << g_k7HoldWould
	   << " k7DestReadyTimeout=" << g_k7DestReadyTimeout
	   << " k7Arrive=w" << g_k7ArrivalArmed
	   << "/s" << g_k7ArrivalSent
	   << "/x" << g_k7ArrivalExpired
	   << "/v" << g_k7ArrivalLiveSent
	   << "/r" << g_k7ArrivalResumed
	   << "/open" << K7ArrivalOpenWaits();
}
