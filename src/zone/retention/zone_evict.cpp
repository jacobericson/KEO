// zone_evict.cpp - Stale tracking eviction, zombie retries and reload accounting.
// Main thread; enters the native-unload fence with no mod lock held.
// The unavailable fallback clears +176 and records ZL_DOUBLE as needed.

#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"
#include "navmesh/nm_workers.h"
#include "zone/transition.h"

using namespace zone_life_detail;

// =========================================================================
// Track until unloaded: the unload pass
// =========================================================================
//
// Main thread, from ZoneLifeTick (hook_updateCameraZone, after
// PreloadCheckSaveLoad and the destroyListOE replay), only with zoneLifeUnload
// on. Once a second it rebuilds the retention set (ZlBuildRetention), stamps
// lastInRadius on every retained record and lists the candidates: records
// continuously outside the set for zoneLifeIdleSeconds. Then, at most once
// per frame, at least ZL_SPACING_SEC after the previous mod unload and never
// in a frame that adopted a zone, it tries the candidate idle longest
// through UnloadModZone. Deferrals are expected and retried:
//   pj    the zero-wait processJobCS try lost to a MISS: retry in 0.1 s
//   job / claim  navmesh work for that zone: that zone waits 1 s, the next
//         candidate may go on the next frame
//   state the zone manager is not at rest, or the zone's sections are
//         still draining: retry in 0.5 s, that zone in 1 s
// Each unload sets _needCalculateIslands and zeroes zone+0x20 in the game, so
// the spacing also keeps island recalculation off consecutive frames.
static const double ZL_RELOAD_SEC   = 60.0;   // zlReload= window after an unload
static const int    ZL_RECENT_CAP   = 32;


struct ZlRecentUnload
{
	int    cell;
	double t;
};
static ZlRecentUnload g_zlRecent[ZL_RECENT_CAP];


namespace zone_life_detail {

void ZlNoteUnloaded(int cell, double now)
{
	if (cell < 0)
		return;
	if (g_zlRecentCount >= ZL_RECENT_CAP)
	{
		for (int k = 1; k < ZL_RECENT_CAP; ++k)
			g_zlRecent[k - 1] = g_zlRecent[k];
		g_zlRecentCount = ZL_RECENT_CAP - 1;
	}
	g_zlRecent[g_zlRecentCount].cell = cell;
	g_zlRecent[g_zlRecentCount].t    = now;
	g_zlRecentCount++;
}

} // namespace
using namespace zone_life_detail;



namespace zone_life_detail {

// zlReload=: a zone the mod unloaded that has a content again within 60 s --
// a town activation, playerActivate or the mod's own preload. Those judge
// zoneLifeIdleSeconds: a steady count means T is too short for the route.
void ZlCheckReloads(void* zoneMgr, double now)
{
	int n = 0;
	for (int k = 0; k < g_zlRecentCount; ++k)
	{
		ZlRecentUnload e = g_zlRecent[k];
		bool keep = (now - e.t) <= ZL_RELOAD_SEC;
		if (keep)
		{
			void* ze = GetZoneEntry(zoneMgr, e.cell / (ZONE_GRID_MAX + 1), e.cell % (ZONE_GRID_MAX + 1));
			if (ze && *(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT)) != NULL)
			{
				g_zlReload++;
				if (g_zl[e.cell].flags != 0)
					g_zlReloadMod++;
				keep = false;
			}
		}
		if (keep)
			g_zlRecent[n++] = e;
	}
	g_zlRecentCount = n;
}

} // namespace
using namespace zone_life_detail;


void EvictStaleZones(void* zoneMgr, double now)
{
	bool zombieTried = false;   // at most one zombie unload attempt per pass
	for (int i = numPreloaded - 1; i >= 0; --i)
	{
		void* ze = preloadedZones[i].zoneEntry;
		if (!ze)
		{
			numPreloaded--;
			if (i < numPreloaded)
			{
				preloadedZones[i] = preloadedZones[numPreloaded];
				++i;
			}
			continue;
		}

		bool loading = IsZoneLoading(ze);
		bool accessible = IsZoneAccessible(ze);

		if (!loading && !accessible && !preloadedZones[i].pending)
		{
			numPreloaded--;
			if (i < numPreloaded)
			{
				preloadedZones[i] = preloadedZones[numPreloaded];
				++i;
			}
			continue;
		}

		// Registration cannot run while a transition
		// is open (camera_zone_hook.cpp gates it), so the stall timer does not run then
		// either; CompactPreloadZones gives the entry its bracket time back.
		if (preloadedZones[i].pending && !isTransitionActive)
		{
			double timeout = preloadedZones[i].registered ? 10.0 : 5.0;
			if (now - preloadedZones[i].loadTimeSec > timeout)
			{
				{
					std::ostringstream ss;
					ss << "Stall timeout: zone (" << preloadedZones[i].gridX
					   << "," << preloadedZones[i].gridY << ") pending "
					   << std::fixed << std::setprecision(1)
					   << (now - preloadedZones[i].loadTimeSec) << "s"
					   << " registered=" << (preloadedZones[i].registered ? 1 : 0)
					   << " load=" << (IsZoneLoading(preloadedZones[i].zoneEntry) ? 1 : 0)
					   << " access=" << (IsZoneAccessible(preloadedZones[i].zoneEntry) ? 1 : 0);
					LogMsg(ss.str());
				}
				preloadedZones[i].pending = false;
				if (pendingCount > 0) pendingCount--;
				continue;
			}
		}

		// Zombie: a stalled +176 zone. Unload it through the game (or, where
		// that can never pass, clear +176 so the game sees it as unloaded).
		if (loading && !accessible
		    && !preloadedZones[i].pending
		    && !preloadedZones[i].gameOwned)
		{
			double age = now - preloadedZones[i].loadTimeSec;
			if (age > 10.0)
			{
				int zgx = preloadedZones[i].gridX;
				int zgy = preloadedZones[i].gridY;
				int zcell = ZoneCell(zgx, zgy);
				bool reg = preloadedZones[i].registered;
				bool clearFallback = (ZlUnloadUnavailable() != NULL);
				if (!clearFallback)
				{
					// Never leave a content alive at +176 =
					// +177 = 0 (a finalized one would be finalized
					// again). Unload the zone through deactivateZoneMap,
					// discarding its state (save = false: never accessible).
					// EvictStaleZones also runs during transitions; no
					// attempt, and so no zlDefer count, until the zone manager
					// is back at rest.
					if (GetZoneState(zoneMgr) != 0 || isTransitionActive
					    || InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0)
						continue;

					// One attempt per pass, spaced like every mod unload; a
					// zombie still backing off from a deferral lets the
					// next one have the attempt. A deferred zone
					// keeps its entry; a pj deferral is retried per frame
					// (ZlZombieFastRetry).
					if (zombieTried || now - g_zlLastUnloadSec < ZL_SPACING_SEC
					    || (zcell >= 0 && g_zl[zcell].nextTry > now))
						continue;
					zombieTried = true;
					bool gone = UnloadModZone(zoneMgr, ze, false, "zombie");
					if (gone)
						g_zlZombieUnloads++;
					if (gone || g_zlLastOutcome == ZLO_RELEASED)
					{
						std::ostringstream ss;
						ss << "Zombie eviction: zone (" << zgx << "," << zgy << ") age="
						   << std::fixed << std::setprecision(1) << age << "s"
						   << " reg=" << (reg ? 1 : 0)
						   << (gone ? " unloaded" : " released (no longer the mod's)");
						LogMsg(ss.str());
						if (g_zlZombieRetryCell == zcell)
							g_zlZombieRetryCell = -1;
						// UnloadModZone NULLed this slot (or the game owns the
						// zone): compact it as the clear did.
						numPreloaded--;
						if (i < numPreloaded)
						{
							preloadedZones[i] = preloadedZones[numPreloaded];
							++i;
						}
						continue;
					}
					if (g_zlLastOutcome == ZLO_UNAVAILABLE)
						clearFallback = true;   // became unavailable at Begin
					else
					{
						if (g_zlLastOutcome == ZLO_DEFERRED && g_zlLastDefer == ZLD_PJ)
							g_zlZombieRetryCell = zcell;
						else if (zcell >= 0 && g_zl[zcell].flags)
							g_zl[zcell].nextTry = now + 4.0;   // two passes: the others get a turn
#ifdef ZONEOPT_DEBUG
						static double lastZombieDeferLog = 0.0;
						if (now - lastZombieDeferLog > 10.0)
						{
							lastZombieDeferLog = now;
							static const char* const kDeferName[ZLD_COUNT] = { "job", "claim", "pj", "state" };
							std::ostringstream ss;
							ss << "Zombie unload deferred: zone (" << zgx << "," << zgy << ") age="
							   << std::fixed << std::setprecision(1) << age << "s why="
							   << ((g_zlLastOutcome == ZLO_DEFERRED) ? kDeferName[g_zlLastDefer] : "anomaly");
							LogDebug(ss.str());
						}
#endif
						continue;   // kept: retried later
					}
				}
				// Fallback: the unload protocol can never pass here, so
				// the zombie gets +176 cleared, entry
				// dropped. A finalized content is flagged (ZL_DOUBLE) exactly
				// since it must never be saved later.
				if (!ContentUntouched(*(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT))))
					ZlTouchZone(ze, ZL_DOUBLE);
				g_zlZombieClear++;
				if (g_zlZombieRetryCell == zcell)
					g_zlZombieRetryCell = -1;
				{
					std::ostringstream ss;
					ss << "Zombie eviction: zone (" << zgx << "," << zgy << ") age="
					   << std::fixed << std::setprecision(1) << age << "s"
					   << " reg=" << (reg ? 1 : 0) << " +176 cleared (unload unavailable)";
					LogMsg(ss.str());
				}
				*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_stateT_mainThreadData__zoneBeingLoaded, OFF_ZONE_IS_LOADING)) = 0;
				numPreloaded--;
				if (i < numPreloaded)
				{
					preloadedZones[i] = preloadedZones[numPreloaded];
					++i;
				}
				continue;
			}
		}

		if (preloadedZones[i].gameOwned && (now - preloadedZones[i].loadTimeSec > 30.0))
		{
			int zx = preloadedZones[i].gridX;
			int zy = preloadedZones[i].gridY;

			if (preloadedZones[i].owner == OWNER_CAMERA && lastCameraGX >= 0)
			{
				int cdx = (zx > lastCameraGX) ? (zx - lastCameraGX) : (lastCameraGX - zx);
				int cdy = (zy > lastCameraGY) ? (zy - lastCameraGY) : (lastCameraGY - zy);
				if (cdx <= 1 && cdy <= 1)
					continue;
			}

			bool nearPlayer = false;
			uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
			if (playerIntf)
			{
				unsigned int scCount = GetPlayerCharCount(playerIntf);
				uintptr_t* scStuff = GetPlayerCharStuff(playerIntf);
				const unsigned int playerCap = ZL_PLAYER_CAP;
				if (scStuff && scCount > 0 && scCount <= playerCap)
				{
					for (unsigned int j = 0; j < scCount; ++j)
					{
						if (!scStuff[j])
							continue;
						float cx = GetCharPosX(scStuff[j]);
						float cz = GetCharPosZ(scStuff[j]);
						int cgx, cgy;
						if (WorldToZoneGrid(cx, cz, &cgx, &cgy))
						{
							int dx = (zx > cgx) ? (zx - cgx) : (cgx - zx);
							int dy = (zy > cgy) ? (zy - cgy) : (cgy - zy);
							if (dx <= 1 && dy <= 1)
							{
								nearPlayer = true;
								break;
							}
						}
					}
				}
			}
			if (!nearPlayer)
			{
				numPreloaded--;
				if (i < numPreloaded)
				{
					preloadedZones[i] = preloadedZones[numPreloaded];
					++i;
				}
			}
			continue;
		}
	}

	if (cameraQueueNext >= cameraQueueCount)
		FlushCameraQueue();
	if (charQueueNext >= charQueueCount)
	{
		charQueueNext = 0;
		charQueueCount = 0;
	}
}



namespace zone_life_detail {

// EvictStaleZones tries a zombie once per 2 s pass. After one of those
// tries lost the zero-wait processJobCS try (pj), UnloadModZone raised the
// priority request; while it stands this retries that zombie every frame, so
// the try lands in the gap the backing-off MISS threads leave. It stops on
// success, on any other outcome, or when the request expires (the 2 s pass
// resumes). Same gates as the pass: at rest, spacing.
void ZlZombieFastRetry(void* zoneMgr, double now)
{
	int cell = g_zlZombieRetryCell;
	if (cell < 0)
		return;
	if (!NavMeshPjPriorityActive())
	{
		g_zlZombieRetryCell = -1;
		return;
	}
	if (now - g_zlLastUnloadSec < ZL_SPACING_SEC
	    || GetZoneState(zoneMgr) != 0 || isTransitionActive
	    || InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0)
		return;

	int gx = cell / (ZONE_GRID_MAX + 1);
	int gy = cell % (ZONE_GRID_MAX + 1);
	int i = -1;
	for (int k = 0; k < numPreloaded; ++k)
	{
		if (preloadedZones[k].zoneEntry && preloadedZones[k].gridX == gx && preloadedZones[k].gridY == gy)
		{
			i = k;
			break;
		}
	}
	if (i < 0)
	{
		g_zlZombieRetryCell = -1;
		return;
	}
	void* ze = preloadedZones[i].zoneEntry;
	// Still a zombie (EvictStaleZones' own test).
	if (!IsZoneLoading(ze) || IsZoneAccessible(ze) || preloadedZones[i].pending
	    || preloadedZones[i].gameOwned)
	{
		g_zlZombieRetryCell = -1;
		return;
	}
	bool reg = preloadedZones[i].registered;
	double age = now - preloadedZones[i].loadTimeSec;
	bool gone = UnloadModZone(zoneMgr, ze, false, "zombie");
	if (gone)
		g_zlZombieUnloads++;
	if (gone || g_zlLastOutcome == ZLO_RELEASED)
	{
		// UnloadModZone NULLed the slot on success; EvictStaleZones compacts it.
		if (!gone)
			DropTrackedZone(i);
		std::ostringstream ss;
		ss << "Zombie eviction: zone (" << gx << "," << gy << ") age="
		   << std::fixed << std::setprecision(1) << age << "s"
		   << " reg=" << (reg ? 1 : 0)
		   << (gone ? " unloaded" : " released (no longer the mod's)")
		   << " (priority retry)";
		LogMsg(ss.str());
		g_zlZombieRetryCell = -1;
		return;
	}
	if (!(g_zlLastOutcome == ZLO_DEFERRED && g_zlLastDefer == ZLD_PJ))
		g_zlZombieRetryCell = -1;   // any other outcome: back to the 2 s pass
}

} // namespace
using namespace zone_life_detail;
