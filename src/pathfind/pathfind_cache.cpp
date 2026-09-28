// pathfind_cache.cpp -- path-request priority tier state and its reporter

#include "pathfind/pathfind_cache.h"
#include "pathfind/pathfinding.h"
#include "pathfind/player_repath_tier.h"


// =========================================================================
// Priority tier state + reporter
// =========================================================================


__declspec(thread) int  squadBoostTier = 0;
__declspec(thread) bool squadBoostFromRepathTier = false;

volatile long p12DiagSubmitBoostsOrder  = 0;
volatile long p12DiagSubmitBoostsState6 = 0;

static double lastP12LogTime = 0.0;

void LogPhase12Stats(double now)
{
	// Gated on pathfindDiagEnabled, the umbrella flag for every path diagnostic.
	if (!pathfind::g_pathfindCfg.pathfindDiagEnabled)
		return;
	if (now - lastP12LogTime < 30.0)
		return;
	lastP12LogTime = now;

	long subBoostsOrder  = InterlockedCompareExchange(&p12DiagSubmitBoostsOrder, 0, 0);
	long subBoostsState6 = InterlockedCompareExchange(&p12DiagSubmitBoostsState6, 0, 0);

	long fbCalls = InterlockedCompareExchange(&fallbackInvocations, 0, 0);


	if (subBoostsOrder == 0 && subBoostsState6 == 0 && !PlayerRepathTierAnyStats()
	    && fbCalls == 0
	   )
		return;

	std::ostringstream ss;
	ss << "Phase12:"
	   << " boost=o" << subBoostsOrder << "/s" << subBoostsState6;
	PlayerRepathTierAppendStats(ss);
	ss
	   << " fbCalls=" << fbCalls
	   ;
	LogMsg(ss.str());
}

