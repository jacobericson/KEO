// path_pool_report.cpp - path queue and search window reports.
// Main thread from PathPoolTickMain; resets each window's counters once.

#include "pathfind/path_pool_internal.h"

namespace path_pool_detail {
// =========================================================================
// Window printers (main thread only, called from PathPoolTickMain)
// =========================================================================

// PathQueue's gate= mirror and the GateRate: line must agree, so the
// gate counters are snapshotted-and-reset exactly once per window regardless
// of whether GateRate: itself is printed (gatePassDiagEnabled off just means
// every field stays 0, honestly, since the gate hook then isn't installed).
GateWindowStats SnapshotAndResetGateStats()
{
	GateWindowStats g;
	g.n            = InterlockedExchange(&g_gatePassCount, 0);
	g.totalUs      = InterlockedExchange64(&g_gatePassTotalUs, 0);
	g.maxUs        = InterlockedExchange(&g_gatePassMaxUs, 0);
	g.inTransition = InterlockedExchange(&g_gatePassInTransition, 0);
	g.iterLimit    = InterlockedExchange(&g_gateIterLimit, 0);
	g.stateFull    = InterlockedExchange(&g_gateStateFull, 0);
	g.searches     = InterlockedExchange(&g_gateSearchCount, 0);
	return g;
}

void PrintPathQueueLine(double windowSec, const GateWindowStats& gws, LONG* servedOut)
{
	LONG arrived = InterlockedExchange(&g_arrivedCount, 0);
	LONG served = InterlockedExchange(&g_servedCount, 0);
	*servedOut = served;
	LONG passes = InterlockedExchange(&g_passCount, 0);
	LONGLONG passUs = InterlockedExchange64(&g_passTotalUs, 0);

	LONG depthMax = InterlockedExchange(&g_depthMax, 0);
	LONGLONG depthSum = InterlockedExchange64(&g_depthSum, 0);
	LONG depthSamples = InterlockedExchange(&g_depthSamples, 0);
	double depthAvg = (depthSamples > 0) ? ((double)depthSum / (double)depthSamples) : 0.0;

	double waitP50 = PPHistPercentileUs(&g_waitHist, 0.50) / 1000.0;
	double waitP90 = PPHistPercentileUs(&g_waitHist, 0.90) / 1000.0;
	double waitP99 = PPHistPercentileUs(&g_waitHist, 0.99) / 1000.0;
	double waitMax = InterlockedExchange(&g_waitHist.maxUs, 0) / 1000.0;

	double svcP50 = PPHistPercentileUs(&g_svcHist, 0.50) / 1000.0;
	double svcP90 = PPHistPercentileUs(&g_svcHist, 0.90) / 1000.0;
	double svcP99 = PPHistPercentileUs(&g_svcHist, 0.99) / 1000.0;
	double svcMax = InterlockedExchange(&g_svcHist.maxUs, 0) / 1000.0;
	LONGLONG svcTotalUs = InterlockedExchange64(&g_svcTotalUs, 0);
	PPHistReset(&g_waitHist);
	PPHistReset(&g_svcHist);

	LONG npcN    = InterlockedExchange(&g_priNpcCount, 0);
	LONG playerN = InterlockedExchange(&g_priPlayerCount, 0);
	LONG tierN   = InterlockedExchange(&g_priTierCount, 0);

	LONG st0 = InterlockedExchange(&g_reqStatusCount[0], 0);
	LONG st1 = InterlockedExchange(&g_reqStatusCount[1], 0);
	LONG st2 = InterlockedExchange(&g_reqStatusCount[2], 0);
	LONG st3 = InterlockedExchange(&g_reqStatusCount[3], 0);
	LONG st4 = InterlockedExchange(&g_reqStatusCount[4], 0);
	LONG direct = InterlockedExchange(&g_directCount, 0);

	LONG termIterLimit   = InterlockedExchange(&g_pathTermIterLimit, 0);
	LONG termOpenSetFull = InterlockedExchange(&g_pathTermOpenSetFull, 0);
	LONG termStateFull   = InterlockedExchange(&g_pathTermStateFull, 0);
	LONG termOther       = InterlockedExchange(&g_pathTermOther, 0);
	LONG astarOk   = InterlockedExchange(&g_pathSearchOk, 0);
	LONG astarFail = InterlockedExchange(&g_pathSearchFail, 0);

	// preamble = (pass total) - (the tightened svc total). svc's serve-start
	// bound (hook_enqueueThreadSafe) is max(passStart, gatePassEnd,
	// lastDequeue this pass), so svc excludes the preamble (origin shift,
	// drainResults, work items, section adds, the gate pass, the request
	// drain) ONLY when a dequeue happened this pass
	// (IDA-confirmed: the drain loop is skipped entirely when the input
	// queue is empty, contentStream+0x848 area / 0x3AEB72-0x3AEB82) or a
	// gate pass ran (only when section adds finish, 0x3AEB30). A backlog
	// pass -- one that serves an older queued request with nothing new
	// arriving this pass -- or any PROD pass with the gate hook off
	// (gatePassDiag=false by default) still has svc == the whole pass, so
	// its preamble is folded into svc, not into this field, for that pass.
	double passMs = passUs / 1000.0;
	double preambleMs = (double)(passUs - svcTotalUs) / 1000.0;
	if (preambleMs < 0.0) preambleMs = 0.0;
	double busyPct = (windowSec > 0.0) ? (passMs / (windowSec * 1000.0) * 100.0) : 0.0;
	double preambleMsPerSec = (windowSec > 0.0) ? (preambleMs / windowSec) : 0.0;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "PathQueue: arrived=" << arrived << " served=" << served
	   << " replaced=- direct=" << direct
	   << " passes=" << passes
	   << " depth max/avg=" << depthMax << "/" << depthAvg
	   << " wait p50/p90/p99/max=" << waitP50 << "/" << waitP90 << "/" << waitP99 << "/" << waitMax << "ms\n"
	   << "  npc(10)=" << npcN << " player(20)=" << playerN << " tier(45+)=" << tierN
	   << " svc p50/p90/p99/max=" << svcP50 << "/" << svcP90 << "/" << svcP99 << "/" << svcMax << "ms\n"
	   << "  st0=" << st0 << " st1=" << st1 << " st2=" << st2 << " st3=" << st3 << " st4=" << st4
	   << " (0 path/1 noStartFace/2 noGoalFace/3 notConnected/4 fallbackFailed;"
	   << " st3 is always 0 because the cluster-graph bypass makes checkFaceConnectivity return 1)\n"
	   << "  astarOk=" << astarOk << " astarFail=" << astarFail
	   << " term=" << termIterLimit << "/" << termOpenSetFull << "/" << termStateFull << "/" << termOther
	   << " (iterLimit/openSetFull/stateFull/other, path-thread queue searches only)\n"
	   << "  busy=" << busyPct << "% preamble=" << preambleMsPerSec << "ms/s gate=";
	if (pathfind::g_pathfindCfg.gatePassDiagEnabled)
	{
		double gateTotalMs = gws.totalUs / 1000.0;
		double gateMaxMs = gws.maxUs / 1000.0;
		ss << gws.n << "/" << gateTotalMs << "ms/" << gateMaxMs << "ms";
	}
	else
	{
		ss << "-";
	}
	ss << " origShift=- drain=- workItems=- sections=-";
	// A latched session is otherwise invisible in a pasted stats line: nothing
	// stopped, the game is still running, and no order can be issued anywhere.
	if (NavMeshUpdateGuardLatched())
		ss << " nmGuard=latched";
	LogMsg(ss.str());
}

// PathBusy: the four-way split PathQueue's aggregate busy% cannot give:
// character search time spent on a status=3/cause=3 (search-state-full)
// termination, other character search time, gate search time (the same
// GateWindowStats total the GateRate: line prints, snapshotted once per
// window by PathPoolTickMain), and everything else in the pass (the
// preamble, and any caller class this window's searches did not include).
// remainder is clamped at 0 so a small timing skew between the two
// independently-reset pass totals never prints as negative.
void PrintPathBusyLine(const GateWindowStats& gws)
{
	LONGLONG passUs    = InterlockedExchange64(&g_busyPassTotalUs, 0);
	LONGLONG cause3Us  = QpcToUs(InterlockedExchange64(&g_busyCharCause3Ticks, 0));
	LONGLONG otherUs   = QpcToUs(InterlockedExchange64(&g_busyCharOtherTicks, 0));
	LONGLONG gateUs    = gws.totalUs;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "PathBusy:";

	// The two character buckets are only ever fed from findPathFull's own
	// hook, through PathSearchSample.callerClass. Without that hook they sit
	// at a true zero because nothing ever calls PathPoolNoteSearch with a
	// character class -- which reads as "no character search time this
	// window" rather than "unknown". Print "?" instead of a false zero.
	if (!AstarCostHookInstalled())
	{
		ss << " charCap3=? charOther=?";
	}
	else
	{
		ss << " charCap3=" << (cause3Us / 1000.0) << "ms"
		   << " charOther=" << (otherUs / 1000.0) << "ms";
	}

	LONGLONG remainderUs = passUs - cause3Us - otherUs - gateUs;
	if (remainderUs < 0) remainderUs = 0;

	ss << " gate=" << (gateUs / 1000.0) << "ms"
	   << " remainder=" << (remainderUs / 1000.0) << "ms"
	   << " (pass=" << (passUs / 1000.0) << "ms)";
	LogMsg(ss.str());
}

void PrintGateRateLine(const GateWindowStats& gws)
{
	double totalMs = gws.totalUs / 1000.0;
	double meanMs = (gws.n > 0) ? (totalMs / (double)gws.n) : 0.0;
	double maxMs = gws.maxUs / 1000.0;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "GateRate: n=" << gws.n << " total=" << totalMs << "ms mean=" << meanMs
	   << "ms max=" << maxMs << "ms searches=" << gws.searches
	   << " iterLimit=" << gws.iterLimit << " stateFull=" << gws.stateFull
	   << " inTransition=" << gws.inTransition;
	LogMsg(ss.str());
}

void PrintPathSlowLine(LONG servedThisWindow)
{
	// The working array's reset only runs on the path thread's next insert,
	// so a window with zero completions would otherwise reprint the
	// previous window's (still-published) top 5 as if it were current.
	// servedThisWindow is this window's real completion count (from
	// PrintPathQueueLine, computed in the same tick) -- skip entirely when
	// it's 0, regardless of what is still sitting in g_slowPublished.
	if (servedThisWindow <= 0)
	{
		InterlockedExchange(&g_slowResetRequested, 1);
		LogMsg("PathSlow: (no completions this window)");
		return;
	}

	PPSlowEntry entries[PP_SLOW_N];
	int count = PPSlowSnapshot(entries);
	InterlockedExchange(&g_slowResetRequested, 1);

	if (count <= 0)
	{
		LogMsg("PathSlow: (no completions this window)");
		return;
	}

	// Sort descending by svcUs (insertion sort, count <= 5).
	for (int i = 1; i < count; ++i)
	{
		PPSlowEntry key = entries[i];
		int j = i - 1;
		while (j >= 0 && entries[j].svcUs < key.svcUs)
		{
			entries[j + 1] = entries[j];
			--j;
		}
		entries[j + 1] = key;
	}

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "PathSlow: top " << count << " by svc ms (start/goal in Havok units, world x 0.1):";
	for (int i = 0; i < count; ++i)
	{
		ss << " #" << (i + 1) << "[svc=" << (entries[i].svcUs / 1000.0) << "ms"
		   << " status=" << entries[i].status
		   << " pri=" << entries[i].priority
		   << " iter=";
		if (entries[i].iterations < 0)
			ss << "-";
		else
			ss << entries[i].iterations;
		ss << " startHU=(" << entries[i].startX << "," << entries[i].startZ << ")"
		   << " goalHU=(" << entries[i].goalX << "," << entries[i].goalZ << ")]";
	}
	LogMsg(ss.str());
}

void PrintAstarCostLine()
{
	PPClassStats* path = &g_classStats[PP_CLASS_PATH];
	LONG pathN = InterlockedExchange(&path->count, 0);
	LONGLONG pathTicks = InterlockedExchange64(&path->totalTicks, 0);
	LONGLONG pathIters = InterlockedExchange64(&path->totalIterations, 0);
	double pathMeanMs = (pathN > 0) ? (QpcToUs(pathTicks) / 1000.0 / (double)pathN) : 0.0;
	double pathMeanIter = (pathN > 0) ? ((double)pathIters / (double)pathN) : 0.0;
	double pathLatP50 = PPHistPercentileUs(&path->latencyUs, 0.50) / 1000.0;
	double pathLatP90 = PPHistPercentileUs(&path->latencyUs, 0.90) / 1000.0;
	PPHistReset(&path->latencyUs);
	// Nanoseconds, not microseconds -- per-iteration cost is typically tens
	// to low thousands of ns, which whole-microsecond buckets would collapse
	// into bucket 0/1, always reading ~1.0/1.0.
	double pathNsP50 = (double)PPHistPercentileUs(&path->iterNsHist, 0.50);
	double pathNsP90 = (double)PPHistPercentileUs(&path->iterNsHist, 0.90);
	PPHistReset(&path->iterNsHist);

	// nav/main/other's totals (totalTicks/totalIterations) feed a mean
	// latency and mean iteration count per class.
	PPClassStats* nav = &g_classStats[PP_CLASS_NAVMESH];
	LONG navN = InterlockedExchange(&nav->count, 0);
	LONGLONG navTicks = InterlockedExchange64(&nav->totalTicks, 0);
	LONGLONG navIters = InterlockedExchange64(&nav->totalIterations, 0);
	double navMeanMs = (navN > 0) ? (QpcToUs(navTicks) / 1000.0 / (double)navN) : 0.0;
	double navMeanIter = (navN > 0) ? ((double)navIters / (double)navN) : 0.0;
	PPHistReset(&nav->latencyUs);
	PPHistReset(&nav->iterNsHist);

	PPClassStats* mainCls = &g_classStats[PP_CLASS_MAIN];
	LONG mainN = InterlockedExchange(&mainCls->count, 0);
	LONGLONG mainTicks = InterlockedExchange64(&mainCls->totalTicks, 0);
	LONGLONG mainIters = InterlockedExchange64(&mainCls->totalIterations, 0);
	double mainMeanMs = (mainN > 0) ? (QpcToUs(mainTicks) / 1000.0 / (double)mainN) : 0.0;
	double mainMeanIter = (mainN > 0) ? ((double)mainIters / (double)mainN) : 0.0;
	PPHistReset(&mainCls->latencyUs);
	PPHistReset(&mainCls->iterNsHist);

	PPClassStats* other = &g_classStats[PP_CLASS_OTHER];
	LONG otherN = InterlockedExchange(&other->count, 0);
	LONGLONG otherTicks = InterlockedExchange64(&other->totalTicks, 0);
	LONGLONG otherIters = InterlockedExchange64(&other->totalIterations, 0);
	double otherMeanMs = (otherN > 0) ? (QpcToUs(otherTicks) / 1000.0 / (double)otherN) : 0.0;
	double otherMeanIter = (otherN > 0) ? ((double)otherIters / (double)otherN) : 0.0;
	PPHistReset(&other->latencyUs);
	PPHistReset(&other->iterNsHist);

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "AstarCost: path-thread n=" << pathN
	   << " lat p50/p90/mean=" << pathLatP50 << "/" << pathLatP90 << "/" << pathMeanMs << "ms"
	   << " meanIter=" << pathMeanIter
	   << " ns/iter p50/p90=" << pathNsP50 << "/" << pathNsP90
	   << " ; nav n=" << navN << " meanMs=" << navMeanMs << " meanIter=" << navMeanIter
	   << " ; main n=" << mainN << " meanMs=" << mainMeanMs << " meanIter=" << mainMeanIter
	   << " ; other n=" << otherN << " meanMs=" << otherMeanMs << " meanIter=" << otherMeanIter;

	// Boost counters: outcome 0=successHigh(>32768 iter) 1=successLow 2=fail.
	static const char* kOutcome[3] = { "high", "low", "fail" };
	ss << " ; boost:";
	for (int o = 0; o < 3; ++o)
	{
		LONG tagNpc    = InterlockedExchange(&g_boostByTag[o][0], 0);
		LONG tagPlayer = InterlockedExchange(&g_boostByTag[o][1], 0);
		LONG reqNpc    = InterlockedExchange(&g_boostByReq[o][0], 0);
		LONG reqPlayer = InterlockedExchange(&g_boostByReq[o][1], 0);
		LONG reqUnk    = InterlockedExchange(&g_boostByReq[o][2], 0);
		ss << " " << kOutcome[o] << "(tag npc=" << tagNpc << "/player=" << tagPlayer
		   << " req npc=" << reqNpc << "/player=" << reqPlayer << "/unk=" << reqUnk << ")";
	}
	LONG disagree = InterlockedExchange(&g_boostDisagree, 0);
	ss << " disagree=" << disagree;
	LogMsg(ss.str());
}

void PrintNpcPathWaitLine(double windowSec)
{
	if (!g_haveLastWalk)
		return;

	NpcWaitWalkResult r = g_lastWalk;

	double finP50 = PPHistPercentileUs(&g_finishedWaitHist, 0.50) / 1000000.0;
	double finP90 = PPHistPercentileUs(&g_finishedWaitHist, 0.90) / 1000000.0;
	double finMax = InterlockedExchange(&g_finishedWaitHist.maxUs, 0) / 1000000.0;
	PPHistReset(&g_finishedWaitHist);

	LONG reissueSamples       = InterlockedExchange(&g_reissueSamples, 0);
	LONG reissueSamplesPlayer = InterlockedExchange(&g_reissueSamplesPlayer, 0);
	// reissue(6) is character-samples in state 6 across this window's
	// once-per-second polls (not a poll count: N characters seen in state 6
	// on one poll add N, not 1), normalised to a /10s rate using windowSec
	// (measured by PathPoolTickMain from the real interval between prints,
	// so this is correct in both DEV's 10s and PROD's 30s window, and on a
	// shorter first window). Split by player-owned, so this rate counts
	// only NPCs.
	double reissueRate10s       = (windowSec > 0.0) ? ((double)reissueSamples * 10.0 / windowSec) : 0.0;
	double reissueRate10sPlayer = (windowSec > 0.0) ? ((double)reissueSamplesPlayer * 10.0 / windowSec) : 0.0;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "NpcPathWait: waiting=" << r.waitingTotal
	   << " (4:" << r.waiting4 << " 5:" << r.waiting5 << ")"
	   << " finished p50/p90/max=" << finP50 << "/" << finP90 << "/" << finMax << "s"
	   << " longest=" << r.longestWaitSec << "s\n"
	   << "  failed(3,farDest)=" << r.failed3FarDest
	   << " reissue(6)=" << reissueRate10s << "/10s"
	   << " stopped(farDest,st0/1,noMove5s)=" << r.stoppedFarDestNoMove
	   << " navWait(st0/1,zoneNotReady)=" << r.navWaitZoneNotReady;
	if (r.faulted)
		ss << " partial=1";
	if (r.haveTop)
	{
		ss << "\n  top: (" << r.topPosX << "," << r.topPosZ << ")"
		   << " zone(" << r.topGx << "," << r.topGy << ")"
		   << " state=" << r.topState
		   << " destDist=" << r.topDestDist
		   << " zoneReady=" << r.topZoneReady;
	}
	ss << "\n  player: waiting=" << r.playerWaitingTotal
	   << " (4:" << r.playerWaiting4 << " 5:" << r.playerWaiting5 << ")"
	   << " reissue(6)=" << reissueRate10sPlayer << "/10s";
	LogMsg(ss.str());
}


} // namespace path_pool_detail
