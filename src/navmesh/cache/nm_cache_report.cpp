// nm_cache_report.cpp - L1/L2 cache and worker-window diagnostics.
// Main thread; reads counters without acquiring nmCacheCS or processJobCS.

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
#include "plugin/hook_manifest.h"

namespace nm_cache_core_detail {
static double lastNMLogTime = 0.0;

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
	   << " fill=" << navmesh::g_nmL1.nmCacheFill << "/" << NM_CACHE_SIZE;

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
			rej[r] = InterlockedCompareExchange(&navmesh::g_nmCache.l2RejCount[r], 0, 0);
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
		ss << " l2Cap=" << InterlockedCompareExchange(&navmesh::g_nmCache.l2CapEvicted, 0, 0);
		// Zero-face entries the blob builder refused. L1 refuses
		// them first today, so this should never print.
		long l2Zero = InterlockedCompareExchange(&navmesh::g_nmCache.nmL2ZeroFaceSkip, 0, 0);
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
		long reconFail = InterlockedCompareExchange(&navmesh::g_nmCache.nmReconFailCount, 0, 0);
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
		long zeroFace = InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceCount, 0, 0);
		ss << " zeroFace=" << zeroFace;
		if (zeroFace)
		{
			ss << "(tri=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastTri, 0, 0)
			   << " vert=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastVert, 0, 0)
			   << " things=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastThings, 0, 0)
			   << " grid=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastGridX, 0, 0)
			   << "," << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastGridY, 0, 0)
			   << " type=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceLastType, 0, 0) << ")";
		}
		ss << " zfEmptyIn=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceEmptyInput, 0, 0)
		   << " zfAbort=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmZeroFaceAbort, 0, 0);
	}
}

// Last job, busy, clone and late-hit counts.
static void AppendWorkerCounts(std::ostringstream& ss, const CacheStatsWindowCtx& ctx)
{
	long jobs = ctx.jobs, gx = ctx.gx, gy = ctx.gy, jt = ctx.jt, step = ctx.step;
	if (jobs > 0)
		ss << " lastGrid=(" << gx << "," << gy << ") type=" << jt;

	ss << " step=" << step;
	ss << " busy=" << InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0);
#ifdef KEO_DEBUG
	ss << " busyViol=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmBusyBridgeViolCount, 0, 0);
#endif
	long cloneOk = InterlockedCompareExchange(&navmesh::g_nmCache.nmCloneConstructCount, 0, 0);
	long cloneFail = InterlockedCompareExchange(&navmesh::g_nmCache.nmCloneConstructFailCount, 0, 0);
	if (cloneOk || cloneFail)
		ss << " clone=" << cloneOk << "/" << (cloneOk + cloneFail);

	long workerMiss = InterlockedCompareExchange(&navmesh::g_nmCache.nmWorkerMissCount, 0, 0);
	long bgMiss = InterlockedCompareExchange(&navmesh::g_nmCache.nmBgMissCount, 0, 0);
	if (workerMiss || bgMiss)
		ss << " miss=w" << workerMiss << "/bg" << bgMiss;

	long lateHits = InterlockedCompareExchange(&navmesh::g_nmCache.nmLateHitCount, 0, 0);
	if (lateHits)
		ss << " lateHit=" << lateHits;
}

// Fresh work-buffer override counts.
static void AppendWorkBufferOverrides(std::ostringstream& ss)
{
	{
		long slopeBad = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbOverrideSlopeBad, 0, 0);
		long ovSkip   = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbOverrideSkipped, 0, 0);
		ss << " wbOv=" << InterlockedCompareExchange(&navmesh::g_nmCache.g_wbOverrideInstalled, 0, 0);
#ifdef KEO_DEBUG
		ss << "/" << InterlockedCompareExchange(&navmesh::g_nmCache.g_wbOverrideAfterPop, 0, 0);
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
#ifdef KEO_DEBUG
	{
		long bits = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbQualityLast, 0, 0);
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
		if (InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneSeen, 0, 0) == 2)
		{
			long ab = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneAreaBits, 0, 0);
			long sb = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneSeedBits, 0, 0);
			long bb = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneBorderBits, 0, 0);
			long fl = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneFlags, 0, 0);
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
		long pruneBad = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbPruneBad, 0, 0);
		long xvBad    = InterlockedCompareExchange(&navmesh::g_nmCache.g_wbExtraVertexBad, 0, 0);
		if (pruneBad)
			ss << " pruneBad=" << pruneBad;
		if (xvBad)
			ss << " xvBad=" << xvBad;
		// L2 off for the session because the real WB's values
		// differ from the settings hash's tables (nm_cache_core.h). Absent
		// normally; r/w are the reads and writes refused since.
		if (L2Bypassed())
			ss << " l2Bypass=on(r" << InterlockedCompareExchange(&navmesh::g_nmCache.nmL2BypassReads, 0, 0)
			   << "/w" << InterlockedCompareExchange(&navmesh::g_nmCache.nmL2BypassWrites, 0, 0) << ")";
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
	else if (InterlockedCompareExchange(&navmesh::g_nmCache.g_nbrSeedHookState, 0, 0) == NBRSEED_HOOK_FAILED)
		ss << " nbrSeed=off(hook)";
	else
		ss << " nbrSeed=live" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrSeedLive, 0, 0)
		   << "/temp" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrSeedTemp, 0, 0)
		   << "/none" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrSeedNone, 0, 0)
		   << "/zero" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrSeedZero, 0, 0);

}

// Stand-in seed and load outcomes.
static void AppendStandIn(std::ostringstream& ss)
{
	// The stand-in inject (nm_cache_core.h): what the stand-in did where the
	// game added nothing. Printed while the feature is on; off(...) says why not.
	if (NmNbrSeedHookWanted() && InterlockedCompareExchange(&navmesh::g_nmCache.g_nbrSeedHookState, 0, 0) != NBRSEED_HOOK_FAILED)
	{
		if (InterlockedCompareExchange(&navmesh::g_nmCache.g_nbrSeedStandInRefused, 0, 0))
		{
			ss << " standIn=off(callee)";
		}
		else
		{
			long ship = InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrStandInShip, 0, 0);
			ss << " standIn=ship" << ship
			   << "/place" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrStandInPlace, 0, 0)
			   << "/nofile" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrStandInNoFile, 0, 0)
			   << "/late" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrStandInLate, 0, 0)
			   // Generations with a late stand-in kept out of L2.
			   << " l2LateSkip=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrL2LateSkip, 0, 0);
			double seedsAvg = ship > 0
				? (double)InterlockedCompareExchange64(&navmesh::g_nmCache.nmNbrStandInSeeds, 0, 0) / (double)ship : 0.0;
			long   loads = InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrLoadCount, 0, 0);
			double loadAvg = loads > 0
				? (double)InterlockedCompareExchange64(&navmesh::g_nmCache.nmNbrLoadTotalUs, 0, 0) / (1000.0 * loads) : 0.0;
			double loadMax = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrLoadMaxUs, 0, 0) / 1000.0;
			ss << std::fixed << std::setprecision(1)
			   << " siSeeds=" << seedsAvg
			   << " siMs=" << loadAvg << "/" << loadMax
			   << " siRec=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrRecordCount, 0, 0);
			long hBad = InterlockedCompareExchange(&navmesh::g_nmCache.nmNbrStandInHBad, 0, 0);
			if (hBad)
				ss << " siHBad=" << hBad;
		}
	}
}

// Neighbour lookups, build lock, tripwire and MISS data.
static void AppendBuildAndTrip(std::ostringstream& ss)
{
	ss << " nbrLookup=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmPartialRealCount, 0, 0);

	ss << BuildLockStatsSuffix();

	// Without the tripwire, trip= reads off: no violation counted is not the same as none possible.
	if (HookRowInstalled(HOOK_PROCESS_JOB_ALT))
		ss << " trip=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmTripCount, 0, 0);
	else
		ss << " trip=off";
	{
		long n = InterlockedCompareExchange(&navmesh::g_nmCache.nmT234Count, 0, 0);
		if (n > 0)
		{
			double avg = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmT234TotalMsTimes10, 0, 0) / (10.0 * n);
			double mx  = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmT234MaxMsTimes10, 0, 0) / 10.0;
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
	ss << " hClosed=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmCloneHandleClosed, 0, 0);
#ifdef KEO_DEBUG
	ss << " wbFreed=" << InterlockedCompareExchange64(&navmesh::g_nmCache.nmWbFreedBytes, 0, 0);
#endif
	{
		long skipped = InterlockedCompareExchange(&navmesh::g_nmCache.nmCloneHandleSkipped, 0, 0);
		if (skipped)
			ss << " hSkip=" << skipped;
	}
}

// Replacement, worker and edge-arm counts.
static void AppendCacheOwnership(std::ostringstream& ss)
{
	ss << " hitStale=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmHitStaleCount, 0, 0)
	   << " l1Replaced=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmL1ReplacedCount, 0, 0)
	   << " dupL2=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmDupL2Avoided, 0, 0);
	const char* refused = NmPoolRefusalToken(
		(NmPoolDecision)InterlockedCompareExchange(&navmesh::g_nmCache.g_navMeshPoolRefusal, 0, 0));
	if (refused)
		ss << " workers=" << refused;
	else
		ss << " workers=" << InterlockedCompareExchange(&navmesh::g_nmCache.g_navMeshWorkersLive, 0, 0);
	{
		long full = InterlockedCompareExchange(&navmesh::g_nmCache.nmL2FlightFull, 0, 0);
		if (full)
			ss << " l2Full=" << full;
	}

	long edgeArmed = InterlockedCompareExchange(&navmesh::g_nmCache.g_edgeProcessArmedCount, 0, 0);
	long edgeUnarmed = InterlockedCompareExchange(&navmesh::g_nmCache.g_edgeProcessUnarmedCount, 0, 0);
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
			long   n   = InterlockedCompareExchange(&navmesh::g_nmCache.nmPjWaitCount[s], 0, 0);
			double avg = n > 0 ? (double)InterlockedCompareExchange64(&navmesh::g_nmCache.nmPjWaitTotalUs[s], 0, 0) / (1000.0 * n) : 0.0;
			double mx  = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmPjWaitMaxUs[s], 0, 0) / 1000.0;
			ss << " " << PJ_NAMES[s] << "=" << avg << "/" << mx;
		}
		ss << "ms";
		{
			long yields = InterlockedCompareExchange(&navmesh::g_nmCache.nmPjYieldCount, 0, 0);
			if (yields)
				ss << " pjYield=" << yields;
		}

		long   mN   = InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissCount, 0, 0);
		double mAvg = mN > 0 ? (double)InterlockedCompareExchange64(&navmesh::g_nmCache.nmClaimAgeMissTotalUs, 0, 0) / (1000.0 * mN) : 0.0;
		double mMax = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissMaxUs, 0, 0) / 1000.0;
		long   hN   = InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeHitCount, 0, 0);
		double hAvg = hN > 0 ? (double)InterlockedCompareExchange64(&navmesh::g_nmCache.nmClaimAgeHitTotalUs, 0, 0) / (1000.0 * hN) : 0.0;
		double hMax = (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeHitMaxUs, 0, 0) / 1000.0;
		ss << " claimAge miss=" << mAvg << "/" << mMax
		   << " hit=" << hAvg << "/" << hMax << "ms";

		ss << " [<10:"  << InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissBucket[0], 0, 0)
		   << " <100:"  << InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissBucket[1], 0, 0)
		   << " <1s:"   << InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissBucket[2], 0, 0)
		   << " <5s:"   << InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissBucket[3], 0, 0)
		   << " >=5s:"  << InterlockedCompareExchange(&navmesh::g_nmCache.nmClaimAgeMissBucket[4], 0, 0) << "]";

		long sw = InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleCount[STALE_SITE_WMISS], 0, 0);
		long sb = InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleCount[STALE_SITE_BGMISS], 0, 0);
		long sh = InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleCount[STALE_SITE_HIT], 0, 0);
		long se = InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleCount[STALE_SITE_EARLY], 0, 0);
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
			long r = InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleLastReason, 0, 0);
			if (r < 0 || r > STALE_REASON_SHUTDOWN) r = STALE_REASON_NONE;
			ss << " staleLast=(" << InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleLastGridX, 0, 0)
			   << "," << InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleLastGridY, 0, 0)
			   << ")t" << InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleLastType, 0, 0)
			   << "/" << REASONS[r]
			   << "@" << (double)InterlockedCompareExchange(&navmesh::g_nmCache.nmStaleLastAgeUs, 0, 0) / 1000.0 << "ms";
		}

		// Claim-time building hashes (and L1 stores) refused
		// because the zone's content changed or faulted under the read. Always
		// printed; expected 0 outside zone unloads (nm_cache_core.h).
		ss << " hashRace=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmHashRaceCount, 0, 0);

		// Mod-unload protocol deferrals (nm_workers.h). Always printed.
		ss << " ulSkipJob="   << InterlockedCompareExchange(&navmesh::g_nmCache.nmUlSkipJob, 0, 0)
		   << " ulSkipClaim=" << InterlockedCompareExchange(&navmesh::g_nmCache.nmUlSkipClaim, 0, 0)
		   << " ulSkipPj="    << InterlockedCompareExchange(&navmesh::g_nmCache.nmUlSkipPj, 0, 0)
		   << " ulPrio="      << InterlockedCompareExchange(&navmesh::g_nmCache.nmUlPrio, 0, 0)
		   << " ulPrioWin="   << InterlockedCompareExchange(&navmesh::g_nmCache.nmUlPrioWin, 0, 0);
		long ulHeld = InterlockedCompareExchange(&navmesh::g_nmCache.nmUlHeld, 0, 0);
		if (ulHeld)
			ss << " ulHeld=" << ulHeld;
		long ulGuardClaim = InterlockedCompareExchange(&navmesh::g_nmCache.nmUlGuardClaim, 0, 0);
		if (ulGuardClaim)
			ss << " ulGuardClaim=" << ulGuardClaim;
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
			const L2MissEntry& e = navmesh::g_nmCache.l2MissLog[m];
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
	long missLogN = InterlockedCompareExchange(&navmesh::g_nmCache.l2MissLogCount, 0, 0);
	long missReported = InterlockedCompareExchange(&navmesh::g_nmCache.l2MissLogReported, 0, 0);
	if (missLogN > missReported && missReported < L2_MISS_LOG_MAX)
	{
		long end = missLogN;
		if (end > L2_MISS_LOG_MAX) end = L2_MISS_LOG_MAX;
		for (long m = missReported; m < end; ++m)
		{
			LogL2MissLine(m);
		}
		InterlockedExchange(&navmesh::g_nmCache.l2MissLogReported, end);
	}
}

// Prints the first EdgeMatchParams line. Main thread; one-time settings window.
static void LogEdgeMatchParamsLine()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh EdgeMatchParams: "
			  << "maxStepH=" << navmesh::g_nmCache.probeEMP[0]
			  << " maxSep=" << navmesh::g_nmCache.probeEMP[1]
			  << " maxOverhang=" << navmesh::g_nmCache.probeEMP[2]
			  << " behindFaceTol=" << navmesh::g_nmCache.probeEMP[3]
			  << " cosPlanarAlign=" << navmesh::g_nmCache.probeEMP[4]
			  << " cosVertAlign=" << navmesh::g_nmCache.probeEMP[5]
			  << " minEdgeOverlap=" << navmesh::g_nmCache.probeEMP[6];
			LogMsg(e.str());
}

// Prints the second EdgeMatchParams line. Main thread; one-time settings window.
static void LogEdgeMatchParams2Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh EdgeMatchParams2: "
			  << "travHorizEps=" << navmesh::g_nmCache.probeEMP[7]
			  << " travVertEps=" << navmesh::g_nmCache.probeEMP[8]
			  << " cosClimbFace=" << navmesh::g_nmCache.probeEMP[9]
			  << " cosClimbEdge=" << navmesh::g_nmCache.probeEMP[10]
			  << " minAngleFaces=" << navmesh::g_nmCache.probeEMP[11]
			  << " edgeParallelTol=" << navmesh::g_nmCache.probeEMP[12]
			  << " pad=" << navmesh::g_nmCache.probeEMP[13];
			LogMsg(e.str());
}

// Prints the first generation-config line. Main thread; one-time settings window.
static void LogGenConfig336Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh GenConfig+336: ";
			for (int i = 0; i < 12; ++i)
				e << "[" << i << "]=" << navmesh::g_nmCache.probeGen[i] << " ";
			LogMsg(e.str());
}

// Prints the second generation-config line. Main thread; one-time settings window.
static void LogGenConfig384Line()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(5);
			e << "NavMesh GenConfig+384: ";
			for (int i = 12; i < 24; ++i)
				e << "[" << i << "]=" << navmesh::g_nmCache.probeGen[i] << " ";
			LogMsg(e.str());
}

// Prints the miscellaneous parameter line. Main thread; one-time settings window.
static void LogMiscParamsLine()
{
			std::ostringstream e;
			e << std::fixed << std::setprecision(4);
			e << "NavMesh Misc: "
			  << "quantGrid=" << navmesh::g_nmCache.probeMisc[0]
			  << " degenArea=" << navmesh::g_nmCache.probeMisc[1]
			  << " charWidth=" << navmesh::g_nmCache.probeMisc[2]
			  << " edgeFilterThresh=" << navmesh::g_nmCache.probeMisc[3]
			  << " maxEdgesPerFace=" << (int)navmesh::g_nmCache.probeMisc[4]
			  << " edgeMatchMetric=" << (int)navmesh::g_nmCache.probeMisc[5]
			  << " edgeConnIter=" << (int)navmesh::g_nmCache.probeMisc[6]
			  << " maxPartSize=" << (int)navmesh::g_nmCache.probeMisc[7];
			LogMsg(e.str());
}

// Opens the one-time settings window and prints its five lines in order.
// Main thread; the published flag is consumed before any line is emitted.
static void LogSettingsLines()
{
	// One-time settings dump
	if (InterlockedCompareExchange(&navmesh::g_nmCache.nmSettingsDumped, 3, 2) == 2)
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
	if (InterlockedCompareExchange(&navmesh::g_nmCache.nmSettingsVerified, 3, 2) == 2)
	{
		std::ostringstream e;
		e << std::fixed << std::setprecision(5);
		e << "NavMesh generation settings: the game's own (no mod tuning):"
		  << " maxStepH=" << navmesh::g_nmCache.verifyEMP[0]
		  << " maxSep=" << navmesh::g_nmCache.verifyEMP[1]
		  << " cosPlanar=" << navmesh::g_nmCache.verifyEMP[2]
		  << " minCorr=" << navmesh::g_nmCache.verifyEMP[3]
		  << " maxCorr=" << navmesh::g_nmCache.verifyEMP[4]
		  << " minChar=" << navmesh::g_nmCache.verifyEMP[5]
		  << " edgeIter=" << (int)navmesh::g_nmCache.verifyEMP[6]
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
		uintptr_t nmgAddr = ((uintptr_t)(unsigned long)navmesh::g_nmCache.probeNMGPtrHi << 32) | (unsigned long)navmesh::g_nmCache.probeNMGPtrLo;
		uintptr_t wbAddr = ((uintptr_t)(unsigned long)navmesh::g_nmCache.probeWBPtrHi << 32) | (unsigned long)navmesh::g_nmCache.probeWBPtrLo;
		uintptr_t havokAddr = ((uintptr_t)(unsigned long)navmesh::g_nmCache.probeHavokPtrHi << 32) | (unsigned long)navmesh::g_nmCache.probeHavokPtrLo;
		long match = InterlockedCompareExchange(&navmesh::g_nmCache.probeWBMatch, 0, 0);

		long fieldScan = InterlockedCompareExchange(&navmesh::g_nmCache.probeWBFieldScan, 0, 0);

		std::ostringstream e;
		e << "WorkBuffer probe: NMG=" << (void*)nmgAddr
		  << " wb(+256)=" << (void*)wbAddr
		  << " sectionMgr+136=" << (void*)havokAddr;
		if (match)
			e << " MATCH (size=688)";
		else if (wbAddr && havokAddr)
			e << " DIFFER offset=" << (__int64)((long long)wbAddr - (long long)havokAddr);
		long lastReadable = InterlockedCompareExchange(&navmesh::g_nmCache.probeWBHeapSize, 0, 0);
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
					a << " +" << navmesh::g_nmCache.wbArrayOffsets[i];
				LogMsg(a.str());
			}
}

// Prints one hkArray probe row in index order.
// Main thread; reads published probe slot i.
static void LogHkArrayEntryLine(int i)
{
				std::ostringstream d;
				d << "  wb+" << navmesh::g_nmCache.wbArrayProbes[i].offset
				  << " ptr=" << (void*)navmesh::g_nmCache.wbArrayProbes[i].ptr
				  << " count=" << navmesh::g_nmCache.wbArrayProbes[i].count
				  << " cap=" << (navmesh::g_nmCache.wbArrayProbes[i].capFlags & 0x3FFFFFFF)
				  << " flags=0x" << std::hex << ((unsigned int)navmesh::g_nmCache.wbArrayProbes[i].capFlags & 0xC0000000) << std::dec;
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
	if (InterlockedCompareExchange(&navmesh::g_nmCache.wbProbeDone, 3, 2) == 2)
	{
		LogWorkBufferProbeLine();
		long arrCount = InterlockedCompareExchange(&navmesh::g_nmCache.wbArrayCount, 0, 0);
		long wrtCount = InterlockedCompareExchange(&navmesh::g_nmCache.wbWritableCount, 0, 0);
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
	if (!navmesh::g_navmeshCfg.cachingEnabled)
		return;
#ifdef KEO_DEBUG
	if (now - lastNMLogTime < 10.0)
#else
	if (now - lastNMLogTime < 30.0)
#endif
		return;
	lastNMLogTime = now;

	CacheStatsWindowCtx ctx;
	ctx.jobs = InterlockedCompareExchange(&navmesh::g_nmCache.nmJobCount, 0, 0);
	ctx.hits = InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheHitCount, 0, 0);
	ctx.misses = InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheMissCount, 0, 0);
	ctx.skips = InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheSkipCount, 0, 0);
	ctx.totalMs = InterlockedCompareExchange(&navmesh::g_nmCache.nmTotalMsTimes10, 0, 0);
	ctx.savedMs = InterlockedCompareExchange(&navmesh::g_nmCache.nmSavedMsTimes10, 0, 0);
	ctx.gx = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiagLastGridX, 0, 0);
	ctx.gy = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiagLastGridY, 0, 0);
	ctx.jt = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiagLastType, 0, 0);
	ctx.step = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiagStep, 0, 0);
	ctx.hitGr = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiagHitGrid, 0, 0);

	ctx.diskHits = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiskHitCount, 0, 0);
	ctx.diskMiss = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiskMissCount, 0, 0);
	ctx.diskWrites = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiskWriteCount, 0, 0);
	ctx.diskReadUs = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiskReadUsTimes1, 0, 0);
	ctx.diskWriteUs = InterlockedCompareExchange(&navmesh::g_nmCache.nmDiskWriteUsTimes1, 0, 0);

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
