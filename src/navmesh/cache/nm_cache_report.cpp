// nm_cache_report.cpp - L1/L2 cache and worker-window diagnostics.
// Main thread; reads counters without acquiring nmCacheCS or processJobCS.

#include "navmesh/cache/nm_cache_core_internal.h"
#include "fixes/world/destroy_list_defer.h"
#include "navmesh/cache/nm_cache_core.h"
#include "diag/mem_probe.h"
#include "navmesh/generation/nm_misspar.h"      // MissParAppendStats (missN=/missT=/genOv=/pjHeld= tokens)
#include "navmesh/generation/nm_quality.h"      // NmVanillaPruningActive (prune= token, settings lines)
#include "navmesh/jobs/nm_buildlock.h"    // BuildLockStatsSuffix (bcMode= ... stitch= tokens)
#include "navmesh/cache/nm_disk_cache.h"   // L2 format constants for the startup self-check
#include "navmesh/cache/nm_l2_writer.h"    // l2WrFail= token, the not-banking test
#include "navmesh/workers/nm_worker_gate_policy.h"
#include "navmesh/cache/nm_key_hash.h"
using namespace nm_cache_core_detail;

namespace nm_cache_core_detail {

struct CacheStatsWindowCtx
{
    long jobs, hits, misses, skips, totalMs, savedMs;
    long gx, gy, jt, step, hitGr;
    long diskHits, diskMiss, diskWrites, diskReadUs, diskWriteUs;
};

// Each Append* adds one run of fields; LogNavMeshCacheStats calls them in the shipped stats line's field order;
// do not reorder. Main thread.
// Memory, job totals and hit-rate fields.
static void AppendCacheSummary(std::ostringstream& ss, const CacheStatsWindowCtx& ctx, const char* memTok)
{
	long jobs = ctx.jobs, hits = ctx.hits, misses = ctx.misses, skips = ctx.skips;
	long totalMs = ctx.totalMs, savedMs = ctx.savedMs;
	long diskHits = ctx.diskHits, diskReadUs = ctx.diskReadUs, diskWrites = ctx.diskWrites;
	ss << "NM cache: mem=" << memTok
	   << " jobs=" << jobs
	   << " L1hit=" << hits
	   << " L2hit=" << diskHits
	   << " miss=" << misses
	   << " skips=" << skips
	   << " fill=" << nmCacheFill << "/" << NM_CACHE_SIZE;

	if (misses > 0)
	{
		double avgMs = (double)totalMs / (10.0 * misses);
		ss << " avgMiss=" << std::fixed << std::setprecision(1) << avgMs << "ms";
	}
	if (hits > 0)
	{
		double avgHitMs = (double)savedMs / (10.0 * hits);
		ss << " avgL1Hit=" << std::fixed << std::setprecision(1) << avgHitMs << "ms";
	}
	if (diskHits > 0)
	{
		double avgL2Ms = (double)diskReadUs / (1000.0 * diskHits);
		ss << " avgL2=" << std::fixed << std::setprecision(1) << avgL2Ms << "ms";
	}
	int totalHits = hits + (int)diskHits;
	int totalAttempts = totalHits + (int)misses;
	int hitPct = (totalAttempts > 0) ? (int)(100 * totalHits / totalAttempts) : 0;
	ss << " hitRate=" << hitPct << "%";
	ss << " diskWrites=" << diskWrites;
}

// L2 rejection, cap, write and banking fields.
static void AppendL2WriteState(std::ostringstream& ss, const CacheStatsWindowCtx& ctx)
{
	long misses = ctx.misses, diskWrites = ctx.diskWrites;
	{
		long rej[L2REJ_REASON_COUNT];
		long rejTotal = 0;
		for (int r = 0; r < L2REJ_REASON_COUNT; ++r)
		{
			rej[r] = InterlockedCompareExchange(&l2RejCount[r], 0, 0);
			rejTotal += rej[r];
		}
		ss << " l2Rej=" << rejTotal
		   << "(m" << rej[L2REJ_MAGIC]
		   << "/v" << rej[L2REJ_VERSION]
		   << "/h" << rej[L2REJ_SETTINGS]
		   << "/l" << rej[L2REJ_LENGTH]
		   << "/c" << rej[L2REJ_CRC]
		   << "/b" << rej[L2REJ_BOUNDS]
		   << "/i" << rej[L2REJ_IO]
		   << "/a" << rej[L2REJ_ALLOC]
		   << "/p" << rej[L2REJ_PATHLONG] << ")";
		ss << " l2Cap=" << InterlockedCompareExchange(&l2CapEvicted, 0, 0);
		// Zero-face entries the blob builder refused. L1 refuses
		// them first today, so this should never print.
		long l2Zero = InterlockedCompareExchange(&nmL2ZeroFaceSkip, 0, 0);
		if (l2Zero)
			ss << " l2ZeroSkip=" << l2Zero;

		// Generations that never reached disk, split by cause. Always printed,
		// so "no failures" and "the token was not built" cannot be confused.
		char wrTok[256];
		L2WriteFormatToken(wrTok, sizeof(wrTok));
		ss << wrTok;

		// The cause-agnostic net. A session can lose every generation without
		// any counter above moving — a cause nobody has thought of yet still
		// shows up here as meshes generated and nothing banked. It says so
		// while the session is still running, not at shutdown, because the
		// point is to let the run be abandoned before it is spent.
		// Zero-face generations are refused before a blob is ever built, so
		// they are not evidence that writing is broken; l2ZeroSkip is their
		// own signal. Only the generations that could have banked count here.
		long bankable = misses - l2Zero;
		if (bankable < 0) bankable = 0;
		if (!L2Bypassed() && L2NotBanking(bankable, diskWrites))
		{
			ss << " l2NoBank=" << bankable;
			if (L2NoBankFirstReport())
			{
				std::ostringstream ns;
				ns << "L2 disk cache: " << bankable << " navmesh generations and 0 files"
				   << " banked this session - the cache is not persisting."
				   << " Any run started after this one will be cold.";
				LogMsg(ns.str());
			}
		}
	}
}

// Reconstruction failures.
static void AppendReconstructFailures(std::ostringstream& ss)
{
	{
		long reconFail = InterlockedCompareExchange(&nmReconFailCount, 0, 0);
		if (reconFail)
			ss << " reconFail=" << reconFail;
	}
}

// Zero-face generation evidence.
static void AppendZeroFace(std::ostringstream& ss)
{
	// tri= is the last zero-face generation's input triangle count (-1 when it
	// was not captured). zfEmptyIn / zfAbort split every zero-face generation by
	// that count; see NoteZeroFaceMesh.
	{
		long zeroFace = InterlockedCompareExchange(&nmZeroFaceCount, 0, 0);
		ss << " zeroFace=" << zeroFace;
		if (zeroFace)
		{
			ss << "(tri=" << InterlockedCompareExchange(&nmZeroFaceLastTri, 0, 0)
			   << " vert=" << InterlockedCompareExchange(&nmZeroFaceLastVert, 0, 0)
			   << " things=" << InterlockedCompareExchange(&nmZeroFaceLastThings, 0, 0)
			   << " grid=" << InterlockedCompareExchange(&nmZeroFaceLastGridX, 0, 0)
			   << "," << InterlockedCompareExchange(&nmZeroFaceLastGridY, 0, 0)
			   << " type=" << InterlockedCompareExchange(&nmZeroFaceLastType, 0, 0) << ")";
		}
		ss << " zfEmptyIn=" << InterlockedCompareExchange(&nmZeroFaceEmptyInput, 0, 0)
		   << " zfAbort=" << InterlockedCompareExchange(&nmZeroFaceAbort, 0, 0);
	}
}

// Last job, busy, clone and late-hit counts.
static void AppendWorkerCounts(std::ostringstream& ss, const CacheStatsWindowCtx& ctx)
{
	long jobs = ctx.jobs, gx = ctx.gx, gy = ctx.gy, jt = ctx.jt, step = ctx.step;
	if (jobs > 0)
		ss << " lastGrid=(" << gx << "," << gy << ") type=" << jt;

	ss << " step=" << step;
	ss << " busy=" << InterlockedCompareExchange(&workerBusyCount, 0, 0);
#ifdef ZONEOPT_DEBUG
	ss << " busyViol=" << InterlockedCompareExchange(&nmBusyBridgeViolCount, 0, 0);
#endif
	long cloneOk = InterlockedCompareExchange(&nmCloneConstructCount, 0, 0);
	long cloneFail = InterlockedCompareExchange(&nmCloneConstructFailCount, 0, 0);
	if (cloneOk || cloneFail)
		ss << " clone=" << cloneOk << "/" << (cloneOk + cloneFail);

	long workerMiss = InterlockedCompareExchange(&nmWorkerMissCount, 0, 0);
	long bgMiss = InterlockedCompareExchange(&nmBgMissCount, 0, 0);
	if (workerMiss || bgMiss)
		ss << " miss=w" << workerMiss << "/bg" << bgMiss;

	long lateHits = InterlockedCompareExchange(&nmLateHitCount, 0, 0);
	if (lateHits)
		ss << " lateHit=" << lateHits;
}

// Fresh work-buffer override counts.
static void AppendWorkBufferOverrides(std::ostringstream& ss)
{
	{
		long slopeBad = InterlockedCompareExchange(&g_wbOverrideSlopeBad, 0, 0);
		long ovSkip   = InterlockedCompareExchange(&g_wbOverrideSkipped, 0, 0);
		ss << " wbOv=" << InterlockedCompareExchange(&g_wbOverrideInstalled, 0, 0);
#ifdef ZONEOPT_DEBUG
		ss << "/" << InterlockedCompareExchange(&g_wbOverrideAfterPop, 0, 0);
#endif
		if (slopeBad)
			ss << " wbOvSlope=" << slopeBad;
		if (ovSkip)
			ss << " wbOvSkip=" << ovSkip;
	}
}

// The DEV work-buffer quality value.
static void AppendWorkBufferQuality(std::ostringstream& ss)
{
#ifdef ZONEOPT_DEBUG
	{
		long bits = InterlockedCompareExchange(&g_wbQualityLast, 0, 0);
		float q;
		memcpy(&q, &bits, sizeof(q));
		ss << " wbQ=" << std::fixed << std::setprecision(2) << q;
	}
#endif
}

// Fresh work-buffer pruning settings.
static void AppendPruning(std::ostringstream& ss)
{
	// Fresh-WB pruning proof token (nm_cache_core.h): on/off is the build and INI
	// state, the values are what the session's first fresh work buffer carried.
	// Expected on(area=100000000.0,seed=0.40,border=0.00,pvb=0,pbt=1); off
	// shows Havok's area=5.0,seed=1.00,border=0.10.
	{
		ss << " prune=" << (NmVanillaPruningActive() ? "on" : "off");
		if (InterlockedCompareExchange(&g_wbPruneSeen, 0, 0) == 2)
		{
			long ab = InterlockedCompareExchange(&g_wbPruneAreaBits, 0, 0);
			long sb = InterlockedCompareExchange(&g_wbPruneSeedBits, 0, 0);
			long bb = InterlockedCompareExchange(&g_wbPruneBorderBits, 0, 0);
			long fl = InterlockedCompareExchange(&g_wbPruneFlags, 0, 0);
			float area, seed, border;
			memcpy(&area, &ab, sizeof(area));
			memcpy(&seed, &sb, sizeof(seed));
			memcpy(&border, &bb, sizeof(border));
			ss << std::fixed
			   << "(area=" << std::setprecision(1) << area
			   << ",seed=" << std::setprecision(2) << seed
			   << ",border=" << border
			   << ",pvb=" << (fl & 0xFF)
			   << ",pbt=" << ((fl >> 8) & 0xFF) << ")";
		}
		else
		{
			ss << "(-)";   // no fresh work buffer built yet this session
		}
		long pruneBad = InterlockedCompareExchange(&g_wbPruneBad, 0, 0);
		long xvBad    = InterlockedCompareExchange(&g_wbExtraVertexBad, 0, 0);
		if (pruneBad)
			ss << " pruneBad=" << pruneBad;
		if (xvBad)
			ss << " xvBad=" << xvBad;
		// L2 off for the session because the real WB's values
		// differ from the settings hash's tables (nm_cache_core.h). Absent
		// normally; r/w are the reads and writes refused since.
		if (L2Bypassed())
			ss << " l2Bypass=on(r" << InterlockedCompareExchange(&nmL2BypassReads, 0, 0)
			   << "/w" << InterlockedCompareExchange(&nmL2BypassWrites, 0, 0) << ")";
	}
}

// Neighbour-seed classifications.
static void AppendNeighbourSeed(std::ostringstream& ss)
{
	// Neighbour seeds (nm_cache_core.h): the four disjoint classes of every
	// getSeedPointsFromAdjacentZone call. off(ini) with navmeshNeighbourSeeds
	// false, off(hook) when the hook was refused (nothing counted then).
	if (!NmNbrSeedHookWanted())
		ss << " nbrSeed=off(ini)";
	else if (InterlockedCompareExchange(&g_nbrSeedHookState, 0, 0) == 2)
		ss << " nbrSeed=off(hook)";
	else
		ss << " nbrSeed=live" << InterlockedCompareExchange(&nmNbrSeedLive, 0, 0)
		   << "/temp" << InterlockedCompareExchange(&nmNbrSeedTemp, 0, 0)
		   << "/none" << InterlockedCompareExchange(&nmNbrSeedNone, 0, 0)
		   << "/zero" << InterlockedCompareExchange(&nmNbrSeedZero, 0, 0);

}

// Stand-in seed and load outcomes.
static void AppendStandIn(std::ostringstream& ss)
{
	// The stand-in inject (nm_cache_core.h): what the stand-in did where the
	// game added nothing. Printed while the feature is on; off(...) says why not.
	if (NmNbrSeedHookWanted() && InterlockedCompareExchange(&g_nbrSeedHookState, 0, 0) != 2)
	{
		if (InterlockedCompareExchange(&g_nbrSeedStandInRefused, 0, 0))
		{
			ss << " standIn=off(callee)";
		}
		else
		{
			long ship = InterlockedCompareExchange(&nmNbrStandInShip, 0, 0);
			ss << " standIn=ship" << ship
			   << "/place" << InterlockedCompareExchange(&nmNbrStandInPlace, 0, 0)
			   << "/nofile" << InterlockedCompareExchange(&nmNbrStandInNoFile, 0, 0)
			   << "/late" << InterlockedCompareExchange(&nmNbrStandInLate, 0, 0)
			   // Generations with a late stand-in kept out of L2.
			   << " l2LateSkip=" << InterlockedCompareExchange(&nmNbrL2LateSkip, 0, 0);
			double seedsAvg = ship > 0
				? (double)InterlockedCompareExchange64(&nmNbrStandInSeeds, 0, 0) / (double)ship : 0.0;
			long   loads = InterlockedCompareExchange(&nmNbrLoadCount, 0, 0);
			double loadAvg = loads > 0
				? (double)InterlockedCompareExchange64(&nmNbrLoadTotalUs, 0, 0) / (1000.0 * loads) : 0.0;
			double loadMax = (double)InterlockedCompareExchange(&nmNbrLoadMaxUs, 0, 0) / 1000.0;
			ss << std::fixed << std::setprecision(1)
			   << " siSeeds=" << seedsAvg
			   << " siMs=" << loadAvg << "/" << loadMax
			   << " siRec=" << InterlockedCompareExchange(&nmNbrRecordCount, 0, 0);
			long hBad = InterlockedCompareExchange(&nmNbrStandInHBad, 0, 0);
			if (hBad)
				ss << " siHBad=" << hBad;
		}
	}
}

// Neighbour lookups, build lock, tripwire and MISS data.
static void AppendBuildAndTrip(std::ostringstream& ss)
{
	ss << " nbrLookup=" << InterlockedCompareExchange(&nmPartialRealCount, 0, 0);

	ss << BuildLockStatsSuffix();

	if (InterlockedCompareExchange(&nmTripInstalled, 0, 0))
		ss << " trip=" << InterlockedCompareExchange(&nmTripCount, 0, 0);
	else
		ss << " trip=off";
	{
		long n = InterlockedCompareExchange(&nmT234Count, 0, 0);
		if (n > 0)
		{
			double avg = (double)InterlockedCompareExchange(&nmT234TotalMsTimes10, 0, 0) / (10.0 * n);
			double mx  = (double)InterlockedCompareExchange(&nmT234MaxMsTimes10, 0, 0) / 10.0;
			ss << " t234=" << std::fixed << std::setprecision(1) << avg
			   << "/" << std::fixed << std::setprecision(1) << mx << "ms";
		}
	}

	MissParAppendStats(ss);
}

// Process and clone handle counts.
static void AppendHandleCounts(std::ostringstream& ss)
{
	// handles= is the whole process's handle count, read on
	// the main thread; it should stay flat across a session.
	{
		DWORD handleCount = 0;
		if (!GetProcessHandleCount(GetCurrentProcess(), &handleCount))
			handleCount = 0;
		ss << " handles=" << handleCount;
	}
	ss << " hClosed=" << InterlockedCompareExchange(&nmCloneHandleClosed, 0, 0);
#ifdef ZONEOPT_DEBUG
	ss << " wbFreed=" << InterlockedCompareExchange64(&nmWbFreedBytes, 0, 0);
#endif
	{
		long skipped = InterlockedCompareExchange(&nmCloneHandleSkipped, 0, 0);
		if (skipped)
			ss << " hSkip=" << skipped;
	}
}

// Replacement, worker and edge-arm counts.
static void AppendCacheOwnership(std::ostringstream& ss)
{
	ss << " hitStale=" << InterlockedCompareExchange(&nmHitStaleCount, 0, 0)
	   << " l1Replaced=" << InterlockedCompareExchange(&nmL1ReplacedCount, 0, 0)
	   << " dupL2=" << InterlockedCompareExchange(&nmDupL2Avoided, 0, 0);
	const char* refused = NmPoolRefusalToken(
		(NmPoolDecision)InterlockedCompareExchange(&g_navMeshPoolRefusal, 0, 0));
	if (refused)
		ss << " workers=" << refused;
	else
		ss << " workers=" << InterlockedCompareExchange(&g_navMeshWorkersLive, 0, 0);
	{
		long full = InterlockedCompareExchange(&nmL2FlightFull, 0, 0);
		if (full)
			ss << " l2Full=" << full;
	}

	long edgeArmed = InterlockedCompareExchange(&g_edgeProcessArmedCount, 0, 0);
	long edgeUnarmed = InterlockedCompareExchange(&g_edgeProcessUnarmedCount, 0, 0);
	if (edgeArmed || edgeUnarmed)
		ss << " edge=a" << edgeArmed << "/u" << edgeUnarmed;
}

// Claim-age, stale, hash-race and unload counters.
static void AppendClaimAndUnload(std::ostringstream& ss)
{
	// Claim-age measurement (nm_cache_core.h). Always printed,
	// zeros included. Every value is ms with one decimal: the average is the
	// microsecond total over the count, the max the largest single sample; both
	// read 0.0 while the count is 0.
	{
		ss << std::fixed << std::setprecision(1);

		static const char* PJ_NAMES[PJWAIT_SITE_COUNT] = { "clone", "wMiss", "bgMiss" };
		ss << " pjWait";
		for (int s = 0; s < PJWAIT_SITE_COUNT; ++s)
		{
			long   n   = InterlockedCompareExchange(&nmPjWaitCount[s], 0, 0);
			double avg = n > 0 ? (double)InterlockedCompareExchange64(&nmPjWaitTotalUs[s], 0, 0) / (1000.0 * n) : 0.0;
			double mx  = (double)InterlockedCompareExchange(&nmPjWaitMaxUs[s], 0, 0) / 1000.0;
			ss << " " << PJ_NAMES[s] << "=" << avg << "/" << mx;
		}
		ss << "ms";
		{
			long yields = InterlockedCompareExchange(&nmPjYieldCount, 0, 0);
			if (yields)
				ss << " pjYield=" << yields;
		}

		long   mN   = InterlockedCompareExchange(&nmClaimAgeMissCount, 0, 0);
		double mAvg = mN > 0 ? (double)InterlockedCompareExchange64(&nmClaimAgeMissTotalUs, 0, 0) / (1000.0 * mN) : 0.0;
		double mMax = (double)InterlockedCompareExchange(&nmClaimAgeMissMaxUs, 0, 0) / 1000.0;
		long   hN   = InterlockedCompareExchange(&nmClaimAgeHitCount, 0, 0);
		double hAvg = hN > 0 ? (double)InterlockedCompareExchange64(&nmClaimAgeHitTotalUs, 0, 0) / (1000.0 * hN) : 0.0;
		double hMax = (double)InterlockedCompareExchange(&nmClaimAgeHitMaxUs, 0, 0) / 1000.0;
		ss << " claimAge miss=" << mAvg << "/" << mMax
		   << " hit=" << hAvg << "/" << hMax << "ms";

		ss << " [<10:"  << InterlockedCompareExchange(&nmClaimAgeMissBucket[0], 0, 0)
		   << " <100:"  << InterlockedCompareExchange(&nmClaimAgeMissBucket[1], 0, 0)
		   << " <1s:"   << InterlockedCompareExchange(&nmClaimAgeMissBucket[2], 0, 0)
		   << " <5s:"   << InterlockedCompareExchange(&nmClaimAgeMissBucket[3], 0, 0)
		   << " >=5s:"  << InterlockedCompareExchange(&nmClaimAgeMissBucket[4], 0, 0) << "]";

		long sw = InterlockedCompareExchange(&nmStaleCount[STALE_SITE_WMISS], 0, 0);
		long sb = InterlockedCompareExchange(&nmStaleCount[STALE_SITE_BGMISS], 0, 0);
		long sh = InterlockedCompareExchange(&nmStaleCount[STALE_SITE_HIT], 0, 0);
		long se = InterlockedCompareExchange(&nmStaleCount[STALE_SITE_EARLY], 0, 0);
		ss << " stale=w" << sw << "/bg" << sb << "/hit" << sh << "/early" << se;

		if (sw + sb + sh + se == 0)
		{
			ss << " staleLast=-";
		}
		else
		{
			// Torn reads across these five are possible and acceptable (see
			// nm_cache_core.h).
			static const char* REASONS[] = { "none", "noZone", "noContent", "noTerrain", "shutdown" };
			long r = InterlockedCompareExchange(&nmStaleLastReason, 0, 0);
			if (r < 0 || r > STALE_REASON_SHUTDOWN) r = STALE_REASON_NONE;
			ss << " staleLast=(" << InterlockedCompareExchange(&nmStaleLastGridX, 0, 0)
			   << "," << InterlockedCompareExchange(&nmStaleLastGridY, 0, 0)
			   << ")t" << InterlockedCompareExchange(&nmStaleLastType, 0, 0)
			   << "/" << REASONS[r]
			   << "@" << (double)InterlockedCompareExchange(&nmStaleLastAgeUs, 0, 0) / 1000.0 << "ms";
		}

		// Claim-time building hashes (and L1 stores) refused
		// because the zone's content changed or faulted under the read. Always
		// printed; expected 0 outside zone unloads (nm_cache_core.h).
		ss << " hashRace=" << InterlockedCompareExchange(&nmHashRaceCount, 0, 0);

		// Mod-unload protocol deferrals (nm_workers.h). Always printed.
		ss << " ulSkipJob="   << InterlockedCompareExchange(&nmUlSkipJob, 0, 0)
		   << " ulSkipClaim=" << InterlockedCompareExchange(&nmUlSkipClaim, 0, 0)
		   << " ulSkipPj="    << InterlockedCompareExchange(&nmUlSkipPj, 0, 0)
		   << " ulPrio="      << InterlockedCompareExchange(&nmUlPrio, 0, 0)
		   << " ulPrioWin="   << InterlockedCompareExchange(&nmUlPrioWin, 0, 0);
		long ulHeld = InterlockedCompareExchange(&nmUlHeld, 0, 0);
		if (ulHeld)
			ss << " ulHeld=" << ulHeld;
	}
}

// The destroy-list suffix, last in the line.
static void AppendDestroyList(std::ostringstream& ss)
{
	// destroyListOE diagnostic (destroy_list_defer.h). Also on the transition line, but this one is
	// periodic, so a session that dies before completing a transition still
	// leaves the inserter counters and the off-main-thread call sites behind.
	ss << DestroyListStatsSuffix();
}

// Prints one L2 miss or write diagnostic line after the cache line.
// Main thread, from the bounded miss-log window.
static void LogL2MissLine(long m)
{
			const L2MissEntry& e = l2MissLog[m];
			std::ostringstream ms;
			const char* tag = (e.thingsCount == -2) ? "L2write" : "L2miss";
			ms << tag << "[" << m << "] zone=(" << e.gridX << "," << e.gridY << ")"
			   << " tile=" << e.tileId << " type=" << e.jobType
			   << " aabb=" << std::hex << e.aabbHash
			   << " bldg=" << e.buildingHash << std::dec;
			if (e.thingsCount >= 0)
				ms << " things=" << e.thingsCount;
			LogMsg(ms.str());
}

// Prints new L2 miss/write entries before the one-time settings lines.
// Main thread; the reported watermark advances only after the loop.
static void LogL2MissLines()
{
	// L2 miss diagnostic
	long missLogN = InterlockedCompareExchange(&l2MissLogCount, 0, 0);
	long missReported = InterlockedCompareExchange(&l2MissLogReported, 0, 0);
	if (missLogN > missReported && missReported < L2_MISS_LOG_MAX)
	{
		long end = missLogN;
		if (end > L2_MISS_LOG_MAX) end = L2_MISS_LOG_MAX;
		for (long m = missReported; m < end; ++m)
		{
			LogL2MissLine(m);
		}
		InterlockedExchange(&l2MissLogReported, end);
	}
}

// Prints the first EdgeMatchParams line. Main thread; one-time settings window.
static void LogEdgeMatchParamsLine()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh EdgeMatchParams: "
			  << "maxStepH=" << probeEMP[0]
			  << " maxSep=" << probeEMP[1]
			  << " maxOverhang=" << probeEMP[2]
			  << " behindFaceTol=" << probeEMP[3]
			  << " cosPlanarAlign=" << probeEMP[4]
			  << " cosVertAlign=" << probeEMP[5]
			  << " minEdgeOverlap=" << probeEMP[6];
			LogMsg(e.str());
}

// Prints the second EdgeMatchParams line. Main thread; one-time settings window.
static void LogEdgeMatchParams2Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh EdgeMatchParams2: "
			  << "travHorizEps=" << probeEMP[7]
			  << " travVertEps=" << probeEMP[8]
			  << " cosClimbFace=" << probeEMP[9]
			  << " cosClimbEdge=" << probeEMP[10]
			  << " minAngleFaces=" << probeEMP[11]
			  << " edgeParallelTol=" << probeEMP[12]
			  << " pad=" << probeEMP[13];
			LogMsg(e.str());
}

// Prints the first generation-config line. Main thread; one-time settings window.
static void LogGenConfig336Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh GenConfig+336: ";
			for (int i = 0; i < 12; ++i)
				e << "[" << i << "]=" << probeGen[i] << " ";
			LogMsg(e.str());
}

// Prints the second generation-config line. Main thread; one-time settings window.
static void LogGenConfig384Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh GenConfig+384: ";
			for (int i = 12; i < 24; ++i)
				e << "[" << i << "]=" << probeGen[i] << " ";
			LogMsg(e.str());
}

// Prints the miscellaneous parameter line. Main thread; one-time settings window.
static void LogMiscParamsLine()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(4);
			e << "NavMesh Misc: "
			  << "quantGrid=" << probeMisc[0]
			  << " degenArea=" << probeMisc[1]
			  << " charWidth=" << probeMisc[2]
			  << " edgeFilterThresh=" << probeMisc[3]
			  << " maxEdgesPerFace=" << (int)probeMisc[4]
			  << " edgeMatchMetric=" << (int)probeMisc[5]
			  << " edgeConnIter=" << (int)probeMisc[6]
			  << " maxPartSize=" << (int)probeMisc[7];
			LogMsg(e.str());
}

// Opens the one-time settings window and prints its five lines in order.
// Main thread; the published flag is consumed before any line is emitted.
static void LogSettingsLines()
{
	// One-time settings dump
	if (InterlockedCompareExchange(&nmSettingsDumped, 3, 2) == 2)
	{
		LogEdgeMatchParamsLine();
		LogEdgeMatchParams2Line();
		LogGenConfig336Line();
		LogGenConfig384Line();
		LogMiscParamsLine();
	}
}

// Prints the one-time real-work-buffer settings statement.
// Main thread; consumes the verified flag before printing.
static void LogSettingsStatementLine()
{
	// One-time settings statement. The real WB's fields,
	// read at the first dispatch (VerifyNavMeshSettings): the game's own values,
	// since no mod code writes them. Expected maxSep=0.2
	// cosPlanar=0.99619 minCorr=0.4 maxCorr=0.6 minChar=0.9 edgeIter=2.
	if (InterlockedCompareExchange(&nmSettingsVerified, 3, 2) == 2)
	{
		std::ostringstream e;
		e << std::fixed << std::setprecision(5);
		e << "NavMesh generation settings: the game's own (no mod tuning):"
		  << " maxStepH=" << verifyEMP[0]
		  << " maxSep=" << verifyEMP[1]
		  << " cosPlanar=" << verifyEMP[2]
		  << " minCorr=" << verifyEMP[3]
		  << " maxCorr=" << verifyEMP[4]
		  << " minChar=" << verifyEMP[5]
		  << " edgeIter=" << (int)verifyEMP[6]
		  << " | fresh-WB pruning: ";
		if (NmVanillaPruningActive())
			e << "on (NMPRUNE_STEP " << 2
			  << ": region pruning + extra vertices copied from the real WB)";
		else
			e << "off (navmeshVanillaPruning=false: Havok's defaults)";
		LogMsg(e.str());
	}
}

// Prints the work-buffer size probe line before the array scan.
// Main thread; reads the published probe fields without taking a mod lock.
static void LogWorkBufferProbeLine()
{
		uintptr_t nmgAddr = ((uintptr_t)(unsigned long)probeNMGPtrHi << 32) | (unsigned long)probeNMGPtrLo;
		uintptr_t wbAddr = ((uintptr_t)(unsigned long)probeWBPtrHi << 32) | (unsigned long)probeWBPtrLo;
		uintptr_t havokAddr = ((uintptr_t)(unsigned long)probeHavokPtrHi << 32) | (unsigned long)probeHavokPtrLo;
		long match = InterlockedCompareExchange(&probeWBMatch, 0, 0);

		long fieldScan = InterlockedCompareExchange(&probeWBFieldScan, 0, 0);

		std::ostringstream e;
		e << "WorkBuffer probe: NMG=" << (void*)nmgAddr
		  << " wb(+256)=" << (void*)wbAddr
		  << " sectionMgr+136=" << (void*)havokAddr;
		if (match)
			e << " MATCH (size=688)";
		else if (wbAddr && havokAddr)
			e << " DIFFER offset=" << (__int64)((long long)wbAddr - (long long)havokAddr);
		long lastReadable = InterlockedCompareExchange(&probeWBHeapSize, 0, 0);
		e << " lastContent=+" << fieldScan
		  << " lastReadable=+" << lastReadable
		  << " allocSize=" << (lastReadable + 8);
		LogMsg(e.str());
}

// Prints the hkArray count and writable-offset line.
// Main thread; called before any per-array lines.
static void LogHkArraySummaryLine(long arrCount, long wrtCount)
{
			{
				std::ostringstream a;
				a << "hkArray scan: " << arrCount << " arrays found, "
				  << wrtCount << " classified writable. Offsets:";
				for (int i = 0; i < arrCount && i < WB_MAX_ARRAYS; ++i)
					a << " +" << wbArrayOffsets[i];
				LogMsg(a.str());
			}
}

// Prints one hkArray probe row in index order.
// Main thread; reads published probe slot i.
static void LogHkArrayEntryLine(int i)
{
				std::ostringstream d;
				d << "  wb+" << wbArrayProbes[i].offset
				  << " ptr=" << (void*)wbArrayProbes[i].ptr
				  << " count=" << wbArrayProbes[i].count
				  << " cap=" << (wbArrayProbes[i].capFlags & 0x3FFFFFFF)
				  << " flags=0x" << std::hex << ((unsigned int)wbArrayProbes[i].capFlags & 0xC0000000) << std::dec;
				LogMsg(d.str());
}

// Prints the empty hkArray result when no array was found.
// Main thread; this branch is mutually exclusive with the probe rows.
static void LogHkArrayEmptyLine()
{
			LogMsg("hkArray scan: 0 arrays found (workBuffer may be uninitialized)");
}

// Prints the work-buffer line and then the hkArray scan lines.
// Main thread; consumes the probe flag before any probe field read.
static void LogWorkBufferLines()
{
	// WorkBuffer size probe dump
	if (InterlockedCompareExchange(&wbProbeDone, 3, 2) == 2)
	{
		LogWorkBufferProbeLine();
		long arrCount = InterlockedCompareExchange(&wbArrayCount, 0, 0);
		long wrtCount = InterlockedCompareExchange(&wbWritableCount, 0, 0);
		if (arrCount > 0)
		{
			LogHkArraySummaryLine(arrCount, wrtCount);
			for (int i = 0; i < arrCount && i < 32; ++i)
			{
				LogHkArrayEntryLine(i);
			}
		}
		else
		{
			LogHkArrayEmptyLine();
		}
	}
}

} // namespace nm_cache_core_detail

using namespace nm_cache_core_detail;

void LogNavMeshCacheStats(double now)
{
	// The deferred NavMesh-thread lines (core.h, LogMsgDeferrable) are
	// flushed from the top of hook_updateCameraZone, before its early returns.
	if (!cachingEnabled)
		return;
#ifdef ZONEOPT_DEBUG
	if (now - lastNMLogTime < 10.0)
#else
	if (now - lastNMLogTime < 30.0)
#endif
		return;
	lastNMLogTime = now;

	CacheStatsWindowCtx ctx;
	ctx.jobs = InterlockedCompareExchange(&nmJobCount, 0, 0);
	ctx.hits = InterlockedCompareExchange(&nmCacheHitCount, 0, 0);
	ctx.misses = InterlockedCompareExchange(&nmCacheMissCount, 0, 0);
	ctx.skips = InterlockedCompareExchange(&nmCacheSkipCount, 0, 0);
	ctx.totalMs = InterlockedCompareExchange(&nmTotalMsTimes10, 0, 0);
	ctx.savedMs = InterlockedCompareExchange(&nmSavedMsTimes10, 0, 0);
	ctx.gx = InterlockedCompareExchange(&nmDiagLastGridX, 0, 0);
	ctx.gy = InterlockedCompareExchange(&nmDiagLastGridY, 0, 0);
	ctx.jt = InterlockedCompareExchange(&nmDiagLastType, 0, 0);
	ctx.step = InterlockedCompareExchange(&nmDiagStep, 0, 0);
	ctx.hitGr = InterlockedCompareExchange(&nmDiagHitGrid, 0, 0);

	ctx.diskHits = InterlockedCompareExchange(&nmDiskHitCount, 0, 0);
	ctx.diskMiss = InterlockedCompareExchange(&nmDiskMissCount, 0, 0);
	ctx.diskWrites = InterlockedCompareExchange(&nmDiskWriteCount, 0, 0);
	ctx.diskReadUs = InterlockedCompareExchange(&nmDiskReadUsTimes1, 0, 0);
	ctx.diskWriteUs = InterlockedCompareExchange(&nmDiskWriteUsTimes1, 0, 0);

	// The last published memory sample, not a fresh reading: the standalone
	// mem: line owns the sampling, and this token exists so a session's cache
	// activity and its memory state can be read off the same row. It carries
	// the sample's age, because the two cadences differ and a reprinted figure
	// must not read as an unchanged one.
	char memTok[160];
	MemProbeShortLastAged(memTok, sizeof(memTok), now);

	std::ostringstream ss;
	AppendCacheSummary(ss, ctx, memTok);
	AppendL2WriteState(ss, ctx);
	AppendReconstructFailures(ss);
	AppendZeroFace(ss);
	AppendWorkerCounts(ss, ctx);
	AppendWorkBufferOverrides(ss);
	AppendWorkBufferQuality(ss);
	AppendPruning(ss);
	AppendNeighbourSeed(ss);
	AppendStandIn(ss);
	AppendBuildAndTrip(ss);
	AppendHandleCounts(ss);
	AppendCacheOwnership(ss);
	AppendClaimAndUnload(ss);
	AppendDestroyList(ss);

	LogMsg(ss.str());
	LogL2MissLines();
	LogSettingsLines();
	LogSettingsStatementLine();
	LogWorkBufferLines();
}
