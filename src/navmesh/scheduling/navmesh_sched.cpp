// NavMesh thread priority boost and 5-tier job queue prioritization.
// Extracted from preload.cpp with refactored interface — no preload/tracking globals.
// All classification data passed via parameters.

#include "navmesh/scheduling/navmesh_sched.h"
#include "movement/mover_policy.h"
#include "zone/preload/coverage_stats.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include <new>


// =========================================================================
// NMG thread priority state (file-static, NMG bg thread only)
// =========================================================================

static HANDLE navMeshThreadHandle  = NULL;
static int    savedThreadPriority  = THREAD_PRIORITY_NORMAL;


// =========================================================================
// Job-conservation stats -- PrioritizeNavMeshQueue runs on the
// main thread only (called from transition_hook.cpp), so these are plain statics,
// no Interlocked needed. drops is a tripwire that must stay 0: a dropped
// job would mean the classify pass lost a node relative to the incoming
// count, which the rebuild refuses to commit (see PrioritizeNavMeshQueue).
// spill/maxQ are cumulative/running-max informational counters.
//
// The spill list itself lives in this growable file-static buffer instead
// of a per-call heap allocation, because PrioritizeNavMeshQueue must never
// allocate (or throw) while holding the game's queue lock: `new[]` can
// throw std::bad_alloc, and a throw there would propagate out without
// releasing the lock, freezing all navmesh dispatch. The buffer is only
// ever grown OUTSIDE the lock (nothrow, NULL-checked); if a pass needs
// more capacity than it currently has, that pass leaves the queue
// untouched (spillShort++) and the next pass gets the bigger buffer.
// =========================================================================

static long   g_schedDrops           = 0;
static long   g_schedSpillTotal      = 0;
static long   g_schedSpillShort      = 0;
static int    g_schedMaxQ            = 0;

static uintptr_t* g_spillBuf         = NULL;
static int         g_spillCap        = 0;

static double g_schedLastLogTime     = -1.0e9;
static long   g_schedLastLoggedDrops = -1;
static long   g_schedLastLoggedSpill = -1;
static int    g_schedLastLoggedMaxQ  = -1;
static long   g_schedLastLoggedReprio = -1;

// Emits "Sched: drops=<n> spill=<m> maxQ=<k>" at most once per interval
// (30s prod / 10s under ZONEOPT_DEBUG), and only when a counter changed
// since the last emission. Called outside the queue lock (no logging
// while the lock is held).
static void MaybeLogSchedStats()
{
	long rpOrder = InterlockedCompareExchange(&reprioOrderFires, 0, 0);
	long rpTimer = InterlockedCompareExchange(&reprioTimerFires, 0, 0);
	long rpTotal = rpOrder + rpTimer;

	if (g_schedDrops == g_schedLastLoggedDrops &&
	    g_schedSpillTotal == g_schedLastLoggedSpill &&
	    g_schedMaxQ == g_schedLastLoggedMaxQ &&
	    rpTotal == g_schedLastLoggedReprio)
		return;  // nothing changed -- nothing to report

#ifdef ZONEOPT_DEBUG
	const double kSchedLogIntervalSec = 10.0;
#else
	const double kSchedLogIntervalSec = 30.0;
#endif

	double now = ElapsedSec();
	if (now - g_schedLastLogTime < kSchedLogIntervalSec)
		return;  // rate-limited

	g_schedLastLogTime     = now;
	g_schedLastLoggedDrops = g_schedDrops;
	g_schedLastLoggedSpill = g_schedSpillTotal;
	g_schedLastLoggedMaxQ  = g_schedMaxQ;
	g_schedLastLoggedReprio = rpTotal;

	std::ostringstream ss;
	ss << "Sched: drops=" << g_schedDrops << " spill=" << g_schedSpillTotal
	   << " maxQ=" << g_schedMaxQ
	   << " reprio=" << rpOrder << "ord/" << rpTimer << "tmr";
	LogMsg(ss.str());
}


// =========================================================================
// Reprio flag + counters (declared in navmesh_sched.h)
// =========================================================================
volatile long reprioTimerFires  = 0;
volatile long reprioOrderFires  = 0;


// =========================================================================
// Thread priority boost (NMG background thread only)
// =========================================================================

void BoostNavMeshThread()
{
	if (!priorityBoostEnabled)
		return;

	uintptr_t mgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!mgr)
		return;

	uintptr_t navMeshGen = *(uintptr_t*)(KLIB_MEMBER(4, mgr, NavMesh_generator, OFF_MGR_NAVMESH_GEN));
	if (!navMeshGen)
		return;

	HANDLE h = *(HANDLE*)(KLIB_MEMBER(4, navMeshGen, ThreadClass_threadHandle, OFF_NAVMESH_THREAD));
	if (h && h != INVALID_HANDLE_VALUE)
	{
		savedThreadPriority = GetThreadPriority(h);
		if (SetThreadPriority(h, THREAD_PRIORITY_HIGHEST))
		{
			navMeshThreadHandle = h;
		}
	}
}

void RestoreNavMeshThread()
{
	if (navMeshThreadHandle)
	{
		SetThreadPriority(navMeshThreadHandle, savedThreadPriority);
		navMeshThreadHandle = NULL;
	}
	savedThreadPriority = THREAD_PRIORITY_NORMAL;
}


// =========================================================================
// 5-tier priority computation (all inputs explicit)
// =========================================================================
//   Tier 1: Camera zones (3x3 around camera)
//   Tier 2: Character path zones (current zone + immediate next in path)
//   Tier 3: Ahead of moving characters (same direction, beyond next zone)
//   Tier 4: Other preloaded zones (stationary characters, behind)
//   Tier 5: Everything else (game's own jobs, unknown zones)

int ComputeZonePriority(int gridX, int gridY,
                        int camGridX, int camGridY,
                        const SchedMoverInfo* movers, int moverCount,
                        const SchedZoneInfo* preloaded, int preloadedCount)
{
	// --- Route tier: the zone a mover with a move order stands in ---
	// Any watched mover's current zone already ranks tier 2 below, so this
	// lifts an order-carrying mover's zone from 2 to 1 wherever the camera
	// 3x3 does not already cover it. ComputeZonePriority also orders
	// registration and adoption, and a zone can only move up here, so
	// nothing it displaces is dropped.
	if (routeTierEnabled)
	{
		for (int w = 0; w < moverCount; ++w)
		{
			if (IsRouteTierMatch(movers[w].hasMoveOrder,
			                     movers[w].currentX, movers[w].currentY,
			                     gridX, gridY))
				return 1;
		}
	}

	// --- Tier 1: Camera zones (3x3 around camera) ---
	if (camGridX >= 0 && camGridY >= 0)
	{
		int dx = gridX - camGridX;
		int dy = gridY - camGridY;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dx <= 1 && dy <= 1)
			return 1;
	}

	// --- Tier 2/3: Character path analysis ---
	bool isTier2 = false;
	bool isTier3 = false;
	for (int w = 0; w < moverCount; ++w)
	{
		int cx = movers[w].currentX;
		int cy = movers[w].currentY;
		int destX = movers[w].destX;
		int destY = movers[w].destY;
		if (cx < 0 || cy < 0) continue;

		// Current zone = Tier 2
		if (gridX == cx && gridY == cy)
		{ isTier2 = true; break; }

		int dirX = 0, dirY = 0;
		if (destX > cx) dirX = 1; else if (destX < cx) dirX = -1;
		if (destY > cy) dirY = 1; else if (destY < cy) dirY = -1;

		// Stationary character: skip path analysis
		if (dirX == 0 && dirY == 0) continue;

		// Next zone in path direction = Tier 2
		if ((dirX != 0 && gridX == cx + dirX && gridY == cy) ||
		    (dirY != 0 && gridX == cx && gridY == cy + dirY) ||
		    (dirX != 0 && dirY != 0 && gridX == cx + dirX && gridY == cy + dirY))
		{ isTier2 = true; break; }

		// Ahead of character (in movement direction) = Tier 3
		if (!isTier3)
		{
			bool aheadX = (dirX == 0) ? (gridX == cx) : ((gridX - cx) * dirX >= 0);
			bool aheadY = (dirY == 0) ? (gridY == cy) : ((gridY - cy) * dirY >= 0);
			if (aheadX && aheadY)
				isTier3 = true;
		}
	}

	if (isTier2) return 2;
	if (isTier3) return 3;

	// --- Tier 4: Any zone in the preload list ---
	// Same ranking as the branch above, and counted the same way, so the
	// cov= tier field means one thing in every build rather than reading
	// zero wherever this arm is the one compiled.
	for (int i = 0; i < preloadedCount; ++i)
	{
		if (preloaded[i].gridX == gridX && preloaded[i].gridY == gridY)
		{
			CoverageNoteTier4();
			return 4;
		}
	}

	CoverageNoteTier5();
	return 5;
}


// =========================================================================
// Queue prioritization: 5-tier navmesh job reordering
// =========================================================================

void PrioritizeNavMeshQueue(int camGridX, int camGridY,
                            const SchedMoverInfo* movers, int moverCount,
                            const SchedZoneInfo* preloaded, int preloadedCount)
{
	if (!fn_pathBuilderInit || !fn_pathBuilderFinalize || !fn_readerUnlock)
		return;

	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!sectionMgr)
		return;
	uintptr_t navMeshGen = *(uintptr_t*)(KLIB_MEMBER(4, sectionMgr, NavMesh_generator, OFF_MGR_NAVMESH_GEN));
	if (!navMeshGen)
		return;

	// Quick check before acquiring lock
	uintptr_t head = *(uintptr_t*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_front, 136));
	if (!head)
		return;

	// Acquire input queue lock at navMeshGen+152
	char initBuf[16];
	void* initResult = fn_pathBuilderInit(initBuf);
	fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_mutex, 152)), initResult);

	head = *(uintptr_t*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_front, 136));
	// The list is rebuilt behind `anchor`: the queue's front, or the next
	// pointer of a head the bg thread has pinned (nm_adjacency.h), which stays
	// first because the job it registered must be the one it processes.
	uintptr_t* anchor = (uintptr_t*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_front, 136));
	if (head && head == NmAdjPinnedLocked())
	{
		anchor = (uintptr_t*)(KLIB_MEMBER(4, head, NavMeshGenerator__Task_next, 96));
		head = *anchor;
	}
	if (!head)
	{
		fn_readerUnlock((void*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_mutex, 152)));
		return;
	}

	// Pass 1: count nodes in the incoming list and tally uncapped per-tier
	// populations, so we know exactly how large the spill list must be.
	// Never dropped: whatever doesn't fit in a tier's 64 slots (or tier 5's,
	// after normal overflow) goes to a spill list sized to fit it exactly.
	const int MAX_PER_TIER = 64;
	int countIn = 0;
	int rawTierCount[5] = {0, 0, 0, 0, 0};

	uintptr_t job = head;
	while (job)
	{
		uintptr_t nextJob = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_next, 96));
		countIn++;

		int tier = 5;
		uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
		if (zone)
		{
			int gx = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
			int gy = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
			tier = ComputeZonePriority(gx, gy, camGridX, camGridY,
			                           movers, moverCount, preloaded, preloadedCount);
		}
		int t = tier - 1;  // 0-indexed
		if (t < 0) t = 0;
		if (t > 4) t = 4;
		rawTierCount[t]++;

		job = nextJob;
	}

	int overflowFromUpperTiers = 0;
	for (int t = 0; t < 4; ++t)
	{
		if (rawTierCount[t] > MAX_PER_TIER)
			overflowFromUpperTiers += rawTierCount[t] - MAX_PER_TIER;
	}
	int tier5Incoming = rawTierCount[4] + overflowFromUpperTiers;
	int spillCount = (tier5Incoming > MAX_PER_TIER) ? (tier5Incoming - MAX_PER_TIER) : 0;

	// No heap operation of any kind happens between the lock acquire above
	// and the release below -- see the g_spillBuf comment near the top of
	// this file. If this pass needs more spill capacity than the buffer
	// currently has, skip classification/rebuild for this pass (leave the
	// queue exactly as it is) and grow the buffer AFTER the unlock so the
	// next pass has room.
	bool spillCapacityOk = (spillCount <= g_spillCap);
	int neededSpillCap = spillCount;

	// Pass 2: classify jobs into tierJobs[][] / g_spillBuf[] in original
	// relative order. Node next-pointers are NOT touched here -- only the
	// pointer VALUES are recorded -- so the original list survives intact
	// if the tripwire below aborts the rebuild. Skipped entirely when the
	// spill buffer is too small this pass (spillCapacityOk above).
	uintptr_t tierJobs[5][MAX_PER_TIER];
	int tierCounts[5] = {0, 0, 0, 0, 0};
	int spillFill = 0;
	int countOut = 0;

	if (spillCapacityOk)
	{
		job = head;
		while (job)
		{
			uintptr_t nextJob = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_next, 96));

			int tier = 5;
			uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
			if (zone)
			{
				int gx = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
				int gy = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
				tier = ComputeZonePriority(gx, gy, camGridX, camGridY,
				                           movers, moverCount, preloaded, preloadedCount);
			}
			int t = tier - 1;  // 0-indexed
			if (t < 0) t = 0;
			if (t > 4) t = 4;

			if (tierCounts[t] < MAX_PER_TIER)
				tierJobs[t][tierCounts[t]++] = job;
			else if (t < 4 && tierCounts[4] < MAX_PER_TIER)
				tierJobs[4][tierCounts[4]++] = job;  // overflow -> lowest tier
			else if (g_spillBuf && spillFill < spillCount)
				g_spillBuf[spillFill++] = job;       // never dropped -- spill list

			job = nextJob;
		}

		// Tripwire: the number of jobs classified above must equal the
		// number of jobs we walked in. If it doesn't, leave the original
		// list (and its node next-pointers) completely untouched and count
		// a drop instead of rebuilding -- see the "no node mutation above" note.
		countOut = tierCounts[0] + tierCounts[1] + tierCounts[2] + tierCounts[3]
		         + tierCounts[4] + spillFill;
	}

	int totalJobs = 0;
	if (!spillCapacityOk)
	{
		// Buffer too small for this pass's spill list: leave the queue
		// exactly as it is (no rebuild). The buffer is grown below, after
		// the unlock, so the next pass has enough room.
		g_schedSpillShort++;
	}
	else if (countOut != countIn)
	{
		g_schedDrops++;
	}
	else
	{
		// Rebuild linked list: tier 1 first -> tier 5 -> spill (never dropped).
		uintptr_t newHead = 0;
		uintptr_t* linkPtr = &newHead;

		for (int t = 0; t < 5; ++t)
		{
			for (int i = 0; i < tierCounts[t]; ++i)
			{
				*linkPtr = tierJobs[t][i];
				linkPtr = (uintptr_t*)(KLIB_MEMBER(4, tierJobs[t][i], NavMeshGenerator__Task_next, 96));
			}
		}
		for (int i = 0; i < spillFill; ++i)
		{
			*linkPtr = g_spillBuf[i];
			linkPtr = (uintptr_t*)(KLIB_MEMBER(4, g_spillBuf[i], NavMeshGenerator__Task_next, 96));
		}
		*linkPtr = 0;  // terminate list

		totalJobs = countOut;

		// Write head (behind the pinned node, if any)
		*anchor = newHead;

		// Write tail: points to &lastJob->next, or the anchor if empty
		if (totalJobs > 0)
			*(uintptr_t*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_back, 144)) = (uintptr_t)linkPtr;
		else
			*(uintptr_t*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_back, 144)) = (uintptr_t)anchor;

		g_schedSpillTotal += spillFill;
		if (countIn > g_schedMaxQ)
			g_schedMaxQ = countIn;
	}

	// Release lock
	fn_readerUnlock((void*)(KLIB_MEMBER(4, navMeshGen, NavMeshGenerator_queue_mutex, 152)));

	// Grow the spill buffer OUTSIDE the lock if this pass needed more than
	// it had. Nothrow + NULL-checked: a failed grow just means the next
	// pass tries again with the same (too-small) capacity -- never a crash,
	// never a throw, never a hang holding the game's lock.
	if (!spillCapacityOk)
	{
		uintptr_t* grown = new (std::nothrow) uintptr_t[neededSpillCap];
		if (grown)
		{
			delete[] g_spillBuf;
			g_spillBuf = grown;
			g_spillCap = neededSpillCap;
		}
	}

	// Gated on any tier at all, not on T1-T3: a pass carrying only T4/T5 work
	// is exactly the case where the mod-prepared-vs-game ranking decides the
	// order, so it is the one that must not print nothing.
	if (tierCounts[0] + tierCounts[1] + tierCounts[2] +
	    tierCounts[3] + tierCounts[4] > 0)
	{
		std::ostringstream ss;
		ss << "NavMesh queue prioritized:"
		   << " T1=" << tierCounts[0] << " T2=" << tierCounts[1]
		   << " T3=" << tierCounts[2] << " T4=" << tierCounts[3]
		   << " T5=" << tierCounts[4];
		LogMsg(ss.str());
	}

	MaybeLogSchedStats();
}
