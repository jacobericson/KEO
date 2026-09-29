// zone_unload.cpp - Native mod-zone unload, retention and the idle save rule.
// Main thread, entered with no mod lock: ownership/readiness/claim checks precede
// a zero-wait processJobCS try. The queue lock is released before the game call.
// Geometry scope ends before PJ unlock and the end of the unload publication.
// Accessible content is saved or kept; an inaccessible zone may be discarded.

#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "movement/tracking.h"
#include "movement/islands.h"
#include "zone/geometry/zone_geometry_epoch.h"

using namespace zone_life_detail;

// =========================================================================
// Mod-zone unload primitive
// =========================================================================
//
// UnloadModZone(zm, zone, save, why) unloads one zone the mod loaded, through
// ZoneManager::deactivateZoneMap (0xA09BB0, via its thunk 0x365ED: the reset
// hook's fn_unloadZoneFromReset), never _dactivateMT directly: only
// deactivateZoneMap also erases the zone from Set B and releases its water
// scene node. Main thread only. Preconditions, in order, each failure
// deferring the zone to a later frame and counting it in
// zlDefer=<job>/<claim>/<pj>/<state>:
//   1. the protocol can pass in this build and session
//      (NavMeshZoneUnloadUnavailable: caching on, or caching off with no
//      mod NavMesh thread) -- not a
//      deferral: ZLO_UNAVAILABLE, never retried. The zombie handler
//      then clears +176 and the idle pass stands down; one PROD
//      line names the reason (ZlUnloadUnavailable)
//   2. the zone has a content (without one, _dactivateMT only clears the
//      island label) -- not a deferral: a gone
//      zone releases its record, flags without a content count nullFlags
//   3. not in Set A or Set B -- not a deferral: the record is released,
//      the game owns the zone
//   4. zone-manager loadingPhase (ZM+0x1681D8) == 0, no transition open
//      or pending, justLoadedAGame (ZM+8) clear                         -> state
//   5. a registered +176 zone: its sections have drained (NavMesh
//      addList empty, or the original isContentPending ready for it),
//      waited for at most 30 s, after which nothing beyond vanilla
//      applies                                                          -> state
//   5b. a +176 zone: terrain collision loaded, or its record at least
//      60 s old (the optional soft precondition)                        -> state
//   6. NavMeshUnloadFence::TryBegin(zone): refused for a queued job -> job, a
//      claim in flight -> claim, anything else (no dispatch yet, an
//      un-ended publication) -> state; NM_FENCE_UNAVAILABLE -> as 1
//   7. then, inside TryBegin, one zero-wait TryEnterCriticalSection;
//      NM_FENCE_DEFER_PJ while any MISS holds processJobCS              -> pj
//      (TryBegin has asked NavMeshRequestPjPriority: the MISS entry points
//      stand aside for up to 2 s, so a retry on the next frames can win)
// then deactivateZoneMap(zm, zone, save) and NavMeshUnloadFence::Release,
// which unlocks processJobCS (only after NM_FENCE_HELD) and then ends the
// publication; its destructor does both on an unwind. No mod lock is held on
// entry, NMG+152 is never held across the unload (TryBegin releases it before
// returning), and neither NavMeshGenerator::hasJob nor NMG+232 is read.
// After the unload: the record is released, any working-table entry dropped,
// the island overlay rebuilt, and the unload timed (zlMs=). Deferrals are
// expected: a zero-timeout try fails for the whole of any navmesh MISS (2-10 s
// on a cold cache).


static const double ZL_DRAIN_WAIT_CAP_SEC = 30.0;   // precondition 5's bound
static const double ZL_TERRAIN_WAIT_SEC   = 60.0;   // precondition 5b's bound (record age)

static long   g_zlDefer[ZLD_COUNT]  = { 0, 0, 0, 0 };
static long   g_zlUnloads           = 0;     // zlUnload=
static long   g_zlSaved             = 0;     // zlSave=
static long   g_zlAnomaly           = 0;     // zlAnomaly= (printed when non-zero)
static double g_zlMsTotal           = 0.0;
static double g_zlMsMax             = 0.0;
static bool   g_zlUnavailLogged     = false;


namespace zone_life_detail {
int    g_zlLastDefer         = ZLD_STATE;
int    g_zlLastOutcome       = ZLO_DEFERRED;
double g_zlLastUnloadSec     = -1.0e9;
static unsigned char g_zlRetain[ZONE_GRID_COUNT];


// Non-NULL when the unload protocol can never pass in this build and
// session (nm_workers.h). Logged once (PROD) the first time it is seen.
const char* ZlUnloadUnavailable()
{
	const char* why = NavMeshZoneUnloadUnavailable();
	if (why && !g_zlUnavailLogged)
	{
		g_zlUnavailLogged = true;
		std::ostringstream ss;
		ss << "ZoneLife: unload unavailable (" << why
		   << "); the zombie handler clears +176 as before K6 and the idle unload pass is off";
		LogMsg(ss.str());
	}
	return why;
}

} // namespace
using namespace zone_life_detail;


// A cell within Chebyshev radius r of a live anchor: the
// hook's camera cell, the zone manager's central zone, or a player
// character's zone (read now, not from the 1 s retention bitmap). An anchor
// that cannot be read counts as near (never act blind).
static bool ZlNearAnchors(void* zoneMgr, int gx, int gy, int r)
{
	if (!zoneMgr || !gridCalibrated)
		return true;
	if (lastCameraGX >= 0 && lastCameraGY >= 0)
	{
		int dx = gx - lastCameraGX; if (dx < 0) dx = -dx;
		int dy = gy - lastCameraGY; if (dy < 0) dy = -dy;
		if (dx <= r && dy <= r) return true;
	}
	void* central = *(void**)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE));
	if (central)
	{
		int dx = gx - GetZoneGridX(central); if (dx < 0) dx = -dx;
		int dy = gy - GetZoneGridY(central); if (dy < 0) dy = -dy;
		if (dx <= r && dy <= r) return true;
	}
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return true;
	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff   = GetPlayerCharStuff(playerIntf);
	if (scCount > ZL_PLAYER_CAP || (scCount > 0 && !scStuff))
		return true;
	for (unsigned int j = 0; j < scCount; ++j)
	{
		if (!scStuff[j])
			continue;
		int cgx, cgy;
		if (!WorldToZoneGrid(GetCharPosX(scStuff[j]), GetCharPosZ(scStuff[j]), &cgx, &cgy))
			continue;
		int dx = gx - cgx; if (dx < 0) dx = -dx;
		int dy = gy - cgy; if (dy < 0) dy = -dy;
		if (dx <= r && dy <= r) return true;
	}
	return false;
}


// Precondition 5. Main thread. A zone whose sections are still being added is
// in a state the save-load reset is the only other code to tear down; the
// navmesh side itself tolerates it (the unload item finds "Zone not
// loaded" and drops, the create retry sees content NULL and drops).
static bool ZlSectionsDrained(void* zone)
{
	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!sectionMgr)
		return true;
	if (*(int*)(KLIB_MEMBER(2, sectionMgr, NavMesh_addList_count, 632)) == 0)
		return true;
	if (game::g_hookOrig.orig_isContentPending)
	{
		int gridCoords[2] = { GetZoneGridX(zone), GetZoneGridY(zone) };
		if (game::g_hookOrig.orig_isContentPending((void*)sectionMgr, (void*)gridCoords))
			return true;
	}
	return false;
}

static void ZlDefer(int kind)
{
	g_zlLastOutcome = ZLO_DEFERRED;
	g_zlLastDefer   = kind;
	g_zlDefer[kind]++;
}


namespace zone_life_detail {

bool UnloadModZone(void* zm, void* zone, bool save, const char* why)
{
	if (!zm || !zone || !fn_unloadZoneFromReset || !IsMainThread())
	{
		ZlDefer(ZLD_STATE);
		return false;
	}

	// 1. The protocol can pass at all here. When it never can, the
	// callers fall back to the zombie handling instead of retrying
	// forever.
	if (ZlUnloadUnavailable())
	{
		g_zlLastOutcome = ZLO_UNAVAILABLE;
		return false;
	}

	int gx = GetZoneGridX(zone);
	int gy = GetZoneGridY(zone);
	int cell = ZoneCell(gx, gy);
	void* content = *(void**)(KLIB_MEMBER(2, (uintptr_t)zone, ZoneMap_mapContent, OFF_ZONE_CONTENT));
	bool loading = IsZoneLoading(zone);
	bool access  = IsZoneAccessible(zone);

	// 2. A content, or there is nothing to unload.
	if (!content)
	{
		if (!loading && !access)
		{
			ZlRelease(cell);
			g_zlRelGone++;
			g_zlLastOutcome = ZLO_RELEASED;
		}
		else
		{
			g_zlAnomaly++;
			g_zlLastOutcome = ZLO_ANOMALY;
		}
		return false;
	}

	// 3. The game's zone now.
	if (ZoneInSetA(zm, zone) || ZoneInSetB(zm, zone))
	{
		ZlRelease(cell);
		g_zlRelSetAB++;
		g_zlLastOutcome = ZLO_RELEASED;
		return false;
	}

	// 4. The zone manager at rest.
	if (GetZoneState(zm) != 0 || isTransitionActive
	    || InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0
	    || *(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)zm, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0)
	{
		ZlDefer(ZLD_STATE);
		return false;
	}

	double now = ElapsedSec();

	// 5. A registered +176 zone: its sections have drained (bounded wait).
	if (loading && cell >= 0 && (g_zl[cell].flags & ZL_REGISTERED) && !ZlSectionsDrained(zone))
	{
		if (g_zl[cell].drainWaitSince < 0.0)
			g_zl[cell].drainWaitSince = now;
		if (now - g_zl[cell].drainWaitSince < ZL_DRAIN_WAIT_CAP_SEC)
		{
			ZlDefer(ZLD_STATE);
			return false;
		}
	}

	// 5b. Soft precondition: a +176 zone has its terrain collision loaded
	// (ZoneMap::isTerrainCollisionLoaded 0xA08E10, our fn_isZoneReady), so it
	// is torn down in the state a vanilla-unloaded zone is in, or its record
	// is at least ZL_TERRAIN_WAIT_SEC old (a terrain that never loads).
	if (loading && fn_isZoneReady && !fn_isZoneReady(zone))
	{
		double recAge = (cell >= 0 && g_zl[cell].flags) ? now - g_zl[cell].firstSeen : 0.0;
		if (recAge < ZL_TERRAIN_WAIT_SEC)
		{
			ZlDefer(ZLD_STATE);
			return false;
		}
	}

	// 6-7. Keep every mod NavMesh thread off the zone, then processJobCS with
	// no wait: every MISS in flight has finished.
	NavMeshUnloadFence fence;
	NmFenceResult fr = fence.TryBegin(zone);
	if (fr == NM_FENCE_UNAVAILABLE)
	{
		ZlUnloadUnavailable();   // logs the reason once
		g_zlLastOutcome = ZLO_UNAVAILABLE;
		return false;
	}
	if (fr == NM_FENCE_DEFER_PJ)
	{
		// The fence ended the publication and asked the MISS threads to
		// stand aside (2 s, never extended; ulPrio= counts the raises). The
		// caller retries on the next frames while it stands.
		ZlDefer(ZLD_PJ);
		return false;
	}
	if (!NmFenceProceeds(fr))
	{
		ZlDefer(fr == NM_FENCE_REFUSED_JOB ? ZLD_JOB : fr == NM_FENCE_REFUSED_CLAIM ? ZLD_CLAIM : ZLD_STATE);
		return false;
	}

	unsigned char flagsBefore = (cell >= 0) ? g_zl[cell].flags : 0;
	double recordAge = (cell >= 0 && flagsBefore) ? now - g_zl[cell].firstSeen : -1.0;

	LARGE_INTEGER q0, q1;
	QueryPerformanceCounter(&q0);
	{
		// The removal half of the same boundary the finalize opens: this
		// takes the cell's objects, and their collision, back out of the
		// world.
		ZoneGeometryMutationScope geomScope;
		fn_unloadZoneFromReset(zm, zone, save ? 1 : 0);
	}
	fence.Release();
	QueryPerformanceCounter(&q1);
	double ms = QPCToMs(q0, q1);

	// The unload must have removed the content (it NULLs mapContent last but
	// one, before clearing +176). A content still there is not an unload.
	if (*(void**)(KLIB_MEMBER(2, (uintptr_t)zone, ZoneMap_mapContent, OFF_ZONE_CONTENT)) != NULL)
	{
		g_zlAnomaly++;
		g_zlLastOutcome = ZLO_ANOMALY;
		std::ostringstream ss;
		ss << "ZoneLife unload: zone (" << gx << "," << gy << ") why=" << why
		   << " -- content still present after deactivateZoneMap; record kept";
		LogMsg(ss.str());
		return false;
	}

	g_zlUnloads++;
	if (save)
		g_zlSaved++;
	g_zlMsTotal += ms;
	if (ms > g_zlMsMax)
		g_zlMsMax = ms;
	g_zlLastUnloadSec = now;
	g_zlLastOutcome   = ZLO_UNLOADED;

	ZlRelease(cell);
	for (int j = 0; j < numPreloaded; ++j)
	{
		if (preloadedZones[j].zoneEntry == zone)
			DropTrackedZone(j);    // slot NULLed; EvictStaleZones compacts it
	}
	// deactivateZoneMap zeroed zone+0x20 and raised _needCalculateIslands; the
	// overlay's mark for the zone drops at this rebuild (flags now 0/0).
	IslandRequestRebuild();
	ZlNoteUnloaded(cell, now);

	{
		std::ostringstream ss;
		ss << "ZoneLife unload: zone (" << gx << "," << gy << ") why=" << why
		   << " save=" << (save ? 1 : 0)
		   << " was=" << (loading ? 1 : 0) << "/" << (access ? 1 : 0)
		   << " flags=0x" << std::hex << (int)flagsBefore << std::dec
		   << " pj=" << ((fr == NM_FENCE_HELD) ? "held" : "none")
		   << std::fixed << std::setprecision(1)
		   << " ms=" << ms
		   << " age=";
		if (recordAge >= 0.0) ss << std::setprecision(0) << recordAge << "s";
		else                  ss << "-";
		LogMsg(ss.str());
	}
	return true;
}

} // namespace
using namespace zone_life_detail;



namespace zone_life_detail {

// ZoneLeak: tokens for step 2 (the unload counters of step 3 follow them).
void ZlAppendStep2Tokens(std::ostringstream& ss)
{
	int dbl = 0;
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		if (g_zl[c].flags & ZL_DOUBLE)
			dbl++;
	ss << " zombieUl=" << g_zlZombieUnloads
	   << " dbl=" << dbl
	   << " zlUnload=" << g_zlUnloads
	   << " zlMs=";
	if (g_zlUnloads > 0)
		ss << std::fixed << std::setprecision(1)
		   << (g_zlMsTotal / (double)g_zlUnloads) << "/" << g_zlMsMax;
	else
		ss << "-/-";
	ss << " zlDefer=" << g_zlDefer[ZLD_JOB] << "/" << g_zlDefer[ZLD_CLAIM]
	   << "/" << g_zlDefer[ZLD_PJ] << "/" << g_zlDefer[ZLD_STATE];
	if (g_zlAnomaly)
		ss << " zlAnomaly=" << g_zlAnomaly;
	// The unload protocol can never pass here; zombies get the
	// +176 clear instead (printed only in that configuration).
	if (g_zlUnavailLogged)
		ss << " zlUnavailable=1 zombieClr=" << g_zlZombieClear;
}

} // namespace
using namespace zone_life_detail;



int PreloadZoneLeakOrphans()
{
	// The last ZoneLeak walk's orphan total (orphan176 + orphan177 + orphan00),
	// so up to one walk interval old; -1 before the first walk of a session.
	return g_zlOrphans;
}


static bool          g_zlRetainOk = false;
static bool ZlBuildRetentionImpl(void* zoneMgr);


namespace zone_life_detail {

bool ZlBuildRetention(void* zoneMgr)
{
	g_zlRetainOk = ZlBuildRetentionImpl(zoneMgr);
	return g_zlRetainOk;
}

} // namespace
using namespace zone_life_detail;


// The same two levels, for callers outside this file (zone_life.h). Each
// rebuild replaces the map wholesale, so an extra caller on its own cadence
// costs a rebuild, never a wrong answer.
void ZlRetentionRefresh(void* zoneMgr) { ZlBuildRetention(zoneMgr); }
bool ZlRetentionReadable()             { return g_zlRetainOk; }
bool ZlRetentionNear(int cell)         { return cell >= 0 && g_zlRetain[cell] != 0; }
bool ZlAnchorsNearCell(void* zoneMgr, int gx, int gy, int r) { return ZlNearAnchors(zoneMgr, gx, gy, r); }

static void ZlStamp(int gx, int gy, int r)
{
	for (int x = gx - r; x <= gx + r; ++x)
	{
		if (x < 0 || x > ZONE_GRID_MAX)
			continue;
		for (int y = gy - r; y <= gy + r; ++y)
		{
			int cell = ZoneCell(x, y);
			if (cell >= 0)
				g_zlRetain[cell] = 1;
		}
	}
}

// The retention set: every cell within Chebyshev radius zoneLifeRetainRadius
// of the camera zone (the hook's own camera cell and the zone manager's central
// zone), of every player character's zone, of every watched mover's current
// and next zone (tracking.cpp), and of every zone queued, pending, registered
// or handed off in the working tables. Returns false when an anchor could not
// be read (no calibrated grid, no camera zone, no player interface, an
// implausible player list); the caller then treats every zone as retained.
static bool ZlBuildRetentionImpl(void* zoneMgr)
{
	memset(g_zlRetain, 0, sizeof(g_zlRetain));
	int r = zone::g_zoneCfg.cfg_zoneLifeRetainRadius;
	if (!zoneMgr || !gridCalibrated)
		return false;

	bool haveCamera = false;
	if (lastCameraGX >= 0 && lastCameraGY >= 0)
	{
		ZlStamp(lastCameraGX, lastCameraGY, r);
		haveCamera = true;
	}
	void* central = *(void**)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE));
	if (central)
	{
		ZlStamp(GetZoneGridX(central), GetZoneGridY(central), r);
		haveCamera = true;
	}
	if (!haveCamera)
		return false;

	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return false;
	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff   = GetPlayerCharStuff(playerIntf);
	ZlNotePlayerCap(scCount);
	if (scCount > ZL_PLAYER_CAP || (scCount > 0 && !scStuff))
		return false;
	for (unsigned int j = 0; j < scCount; ++j)
	{
		if (!scStuff[j])
			continue;
		int cgx, cgy;
		if (WorldToZoneGrid(GetCharPosX(scStuff[j]), GetCharPosZ(scStuff[j]), &cgx, &cgy))
			ZlStamp(cgx, cgy, r);
	}

	int mx[2 * MAX_WATCHED], my[2 * MAX_WATCHED];
	int mn = CollectMoverRetainZones(mx, my, 2 * MAX_WATCHED);
	for (int k = 0; k < mn; ++k)
		ZlStamp(mx[k], my[k], r);

	for (int k = cameraQueueNext; k < cameraQueueCount; ++k)
		ZlStamp(cameraQueue[k].gridX, cameraQueue[k].gridY, r);
	for (int k = charQueueNext; k < charQueueCount; ++k)
		ZlStamp(charQueue[k].gridX, charQueue[k].gridY, r);
	for (int k = 0; k < numPreloaded; ++k)
	{
		if (!preloadedZones[k].zoneEntry)
			continue;
		if (preloadedZones[k].pending || preloadedZones[k].registered)
			ZlStamp(preloadedZones[k].gridX, preloadedZones[k].gridY, r);
	}
	return true;
}

static const double ZL_REFRESH_SEC  = 1.0;
static const int    ZL_CAND_CAP     = 256;
static int    g_zlCand[ZL_CAND_CAP];
static int    g_zlCandCount     = 0;
static double g_zlNextRefresh   = 0.0;
static double g_zlNextAttempt   = 0.0;
static long   g_zlKeep          = 0;       // zlKeep=: accessible zones the pass may not unload (unsaveable)
// The unload verdict for the idle pass, from the game's own fields.
// The mod never writes +177 itself: a cell it holds is either private
// (+176 only) or already handed to the game (adopted, or given back at the
// first-time handoff), so the accessible arm is a backstop rather than
// an expected path -- kept because a private incarnation can still read
// accessible if the game reused the same content behind the mod's back.
//   not accessible: never accessible (+177 is set only by the game's state
//     4->5, and _dactivateMT clears it before a content can come back), so
//     nothing the player did is in it: discard;
//   accessible, finalized (loaded 1), not first-time, not double-processed:
//     save;
//   accessible and anything else (first-time, never finalized, a double
//     finalize, a faulted read): no save = false is allowed and a save would
//     be wrong, so the zone is kept (zlKeep=). The first-time rule and the
//     loaded==1 test before registration make this unreachable; it is the
//     guard behind them.
// Returns false for "keep".
static bool ZlUnloadVerdict(void* zone, void* content, unsigned char flags, bool* save)
{
	*save = false;
	if (!IsZoneAccessible(zone))
		return true;
	int firstTime = 0, loaded = 0, activation = 0;
	if (!ReadContentLifeFlags(content, &firstTime, &loaded, &activation))
		return false;
	if (firstTime != 0 || loaded == 0 || (flags & ZL_DOUBLE))
		return false;
	*save = true;
	return true;
}

// The candidate's cell is queued, or in the working
// table as pending, registered or handed off (the retention set's own
// members), read now rather than from the 1 s bitmap.
static bool ZlCellInUse(int gx, int gy)
{
	for (int k = cameraQueueNext; k < cameraQueueCount; ++k)
		if (cameraQueue[k].gridX == gx && cameraQueue[k].gridY == gy)
			return true;
	for (int k = charQueueNext; k < charQueueCount; ++k)
		if (charQueue[k].gridX == gx && charQueue[k].gridY == gy)
			return true;
	for (int k = 0; k < numPreloaded; ++k)
	{
		if (!preloadedZones[k].zoneEntry || preloadedZones[k].gridX != gx || preloadedZones[k].gridY != gy)
			continue;
		if (preloadedZones[k].pending || preloadedZones[k].registered)
			return true;
	}
	return false;
}


namespace zone_life_detail {

void ZlClearStep3State()
{
	g_zlCandCount     = 0;
	g_zlNextRefresh   = 0.0;
	g_zlNextAttempt   = 0.0;
}

} // namespace
using namespace zone_life_detail;



namespace zone_life_detail {

void ZoneLifeUnloadPass(void* zoneMgr, double now)
{
	// With the protocol unavailable the pass stands down entirely
	// (the zombie handler has its +176 fallback; records stay for ZoneLeak).
	if (ZlUnloadUnavailable())
		return;

	if (now >= g_zlNextRefresh)
	{
		g_zlNextRefresh = now + ZL_REFRESH_SEC;
		ZlCheckReloads(zoneMgr, now);
		bool retainOk = ZlBuildRetention(zoneMgr);
		g_zlCandCount = 0;
		for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		{
			if (!g_zl[c].flags)
				continue;
			// No anchor to judge by counts as retained: never unload blind.
			if (!retainOk || g_zlRetain[c])
			{
				g_zl[c].lastInRadius = now;
				continue;
			}
			if (now - g_zl[c].lastInRadius >= zone::g_zoneCfg.cfg_zoneLifeIdleSeconds && g_zlCandCount < ZL_CAND_CAP)
				g_zlCand[g_zlCandCount++] = c;
		}
	}

	if (g_zlCandCount == 0 || now < g_zlNextAttempt)
		return;
	if (now - g_zlLastUnloadSec < ZL_SPACING_SEC)
		return;
	// Precondition 4 is re-checked inside UnloadModZone; here it only keeps
	// the pass from making (and counting) attempts that must fail, e.g. for the
	// whole of every transition.
	if (GetZoneState(zoneMgr) != 0 || isTransitionActive
	    || InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0)
		return;
	// Never unload blind: the last retention rebuild (this pass's refresh or
	// the ZoneLeak walk's, which share the bitmap) must have read every anchor.
	if (!g_zlRetainOk)
		return;

	int best = -1;
	for (int k = 0; k < g_zlCandCount; ++k)
	{
		int c = g_zlCand[k];
		if (!g_zl[c].flags || g_zl[c].nextTry > now || g_zlRetain[c])
			continue;   // released since the refresh, backing off, or back in radius
		if (best < 0 || g_zl[c].lastInRadius < g_zl[best].lastInRadius)
			best = c;
	}
	if (best < 0)
		return;

	int bgx = best / (ZONE_GRID_MAX + 1);
	int bgy = best % (ZONE_GRID_MAX + 1);
	// The bitmap is up to 1 s old. A cell queued,
	// adopted or near a live anchor since then is back in use: count it as
	// retained now.
	if (ZlCellInUse(bgx, bgy) || ZlNearAnchors(zoneMgr, bgx, bgy, zone::g_zoneCfg.cfg_zoneLifeRetainRadius))
	{
		g_zl[best].lastInRadius = now;
		g_zlNextAttempt = now;
		return;
	}

	void* ze = GetZoneEntry(zoneMgr, bgx, bgy);
	if (!ze)
		return;
	void* content = *(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT));
	bool save = false;
	if (content && !ZlUnloadVerdict(ze, content, g_zl[best].flags, &save))
	{
		// The never-revert guard: an accessible zone that is not saveable is
		// kept (and looked at again in a minute).
		g_zlKeep++;
		g_zl[best].nextTry = now + 60.0;
		g_zlNextAttempt = now;
		std::ostringstream ss;
		ss << "ZoneLife keep: zone (" << bgx << "," << bgy
		   << ") accessible but not saveable (first-time, unfinalized or double-processed); not unloaded";
		LogMsg(ss.str());
		return;
	}

	if (UnloadModZone(zoneMgr, ze, save, "idle"))
	{
		g_zlNextAttempt = now + ZL_SPACING_SEC;
		return;
	}

	switch (g_zlLastOutcome)
	{
	case ZLO_RELEASED:
		g_zlNextAttempt = now;                      // next candidate next frame
		break;
	case ZLO_ANOMALY:
		if (g_zl[best].flags)
			g_zl[best].nextTry = now + 30.0;
		g_zlNextAttempt = now;
		break;
	case ZLO_UNAVAILABLE:
		break;                                      // the pass returns at its top from now on
	default:   // ZLO_DEFERRED
		if (g_zlLastDefer == ZLD_PJ)
		{
			// Every frame while the priority request stands (the
			// MISS entry points are backing off), else the old 0.1 s.
			g_zlNextAttempt = NavMeshPjPriorityActive() ? now : now + 0.1;
		}
		else if (g_zlLastDefer == ZLD_JOB || g_zlLastDefer == ZLD_CLAIM)
		{
			g_zl[best].nextTry = now + 1.0;
			g_zlNextAttempt = now;
		}
		else
		{
			g_zl[best].nextTry = now + 1.0;
			g_zlNextAttempt = now + 0.5;
		}
		break;
	}
}

} // namespace
using namespace zone_life_detail;



namespace zone_life_detail {

void ZlAppendStep3Tokens(std::ostringstream& ss)
{
	ss << " zlSave=" << g_zlSaved
	   << " ftHandoff=" << g_ftSkip << "/" << g_ftCleared << "/" << g_ftLate << "/" << g_ftKeptLate
	   << " zlKeep=" << g_zlKeep
	   << " zlReload=" << g_zlReload << "/" << g_zlReloadMod
	   << " cand=" << g_zlCandCount
	   << " zlUnloadKey=" << (zone::g_zoneCfg.zoneLifeUnloadEnabled ? "on" : "off");
}

} // namespace
using namespace zone_life_detail;
