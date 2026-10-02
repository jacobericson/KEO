#include "zone/preload/preload_queue.h"
#include "zone/preload/coverage_stats.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "navmesh/generation/nm_misspar.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "movement/islands.h"
#include "zone/camera_zone_hook.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/preload/preload_internal.h"
#include "zone/handoff/zone_handoff.h"
#include <cstdio>     // _snprintf_s (hook_resetUnloadZones builds its line without CRT streams)
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX only; the function is resolved at runtime


// Iteration order for registration: navmesh tiers 1-3 first (camera grid,
// movers' current and next zones, mover clusters), then camera-owned before
// character-owned, then index — so the zones a travelling squad is about to
// need are registered before speculative ones (shrinks the parked-at-edge
// window).
int BuildPreloadOrder(int* order)
{
	int n = 0;
	static SchedContext ctx;   // main thread only; ~1.3 KB kept off the stack
	BuildSchedContext(&ctx);
	int key[MAX_PRELOADED];
	for (int i = 0; i < numPreloaded; ++i)
	{
		int tier = ComputeZonePriority(preloadedZones[i].gridX, preloadedZones[i].gridY,
		                               ctx.camX, ctx.camY, ctx.movers, ctx.moverCount,
		                               ctx.zones, ctx.zoneCount);
		if (tier < 1) tier = 1;
		if (tier > 5) tier = 5;
		int ownerRank = (preloadedZones[i].owner == OWNER_CAMERA) ? 0 : 1;
		key[i] = tier * 2 + ownerRank;
		// Stable insertion sort by key (numPreloaded <= 45)
		int k = n++;
		while (k > 0 && key[order[k - 1]] > key[i])
		{
			order[k] = order[k - 1];
			--k;
		}
		order[k] = i;
	}
	return n;
}


static bool IsZoneQueued(int gx, int gy)
{
	for (int i = cameraQueueNext; i < cameraQueueCount; ++i)
		if (cameraQueue[i].gridX == gx && cameraQueue[i].gridY == gy)
			return true;
	for (int i = charQueueNext; i < charQueueCount; ++i)
		if (charQueue[i].gridX == gx && charQueue[i].gridY == gy)
			return true;
	for (int i = 0; i < numPreloaded; ++i)
		if (preloadedZones[i].gridX == gx && preloadedZones[i].gridY == gy)
			return true;
	return false;
}

bool EnqueueCameraZone(int gx, int gy)
{
	if (gx < 0 || gx > 63 || gy < 0 || gy > 63)
		return false;
	if (cameraQueueCount >= CAMERA_RESERVED)
		return false;
	if (IsZoneQueued(gx, gy))
		return false;

	cameraQueue[cameraQueueCount].gridX = gx;
	cameraQueue[cameraQueueCount].gridY = gy;
	cameraQueueCount++;
	return true;
}

bool EnqueueCharacterZone(int gx, int gy)
{
	if (gx < 0 || gx > 63 || gy < 0 || gy > 63)
		return false;
	if (charQueueCount >= MAX_PRELOADED)
	{
		if (charQueueNext > 0)
		{
			int remaining = charQueueCount - charQueueNext;
			for (int i = 0; i < remaining; ++i)
				charQueue[i] = charQueue[charQueueNext + i];
			charQueueCount = remaining;
			charQueueNext = 0;
		}
		if (charQueueCount >= MAX_PRELOADED)
			return false;
	}
	if (IsZoneQueued(gx, gy))
		return false;

	charQueue[charQueueCount].gridX = gx;
	charQueue[charQueueCount].gridY = gy;
	charQueueCount++;
	return true;
}

void FlushCameraQueue()
{
	cameraQueueCount = 0;
	cameraQueueNext = 0;
}

int EnqueueCameraGrid(int centerX, int centerY)
{
	// 3x3 around the predicted cell. Nine zones plus the three EnqueueAheadZones
	// adds is exactly CAMERA_RESERVED, the camera queue's own size -- this grid
	// is what that reserve was sized for, and it cannot crowd charQueue, which
	// is a separate array. The squad-switch swap calls this too, after flushing
	// the camera queue, so its nine go into an empty queue.
	int accepted = 0;
	for (int i = 0; i < 9; ++i)
	{
		int zx = centerX + ORDER_DX[i];
		int zy = centerY + ORDER_DY[i];
		if (EnqueueCameraZone(zx, zy))
			accepted++;
	}
	CoverageNoteCameraGrid(accepted);
	return accepted;
}

static bool EnqueueZoneByOwner(int gx, int gy, int owner)
{
	if (owner == OWNER_CAMERA)
		return EnqueueCameraZone(gx, gy);
	else
		return EnqueueCharacterZone(gx, gy);
}

void EnqueueAheadZones(int centerX, int centerY, int fromX, int fromY, int owner)
{
	int ddx = centerX - fromX;
	int ddy = centerY - fromY;
	if (ddx > 1) ddx = 1; else if (ddx < -1) ddx = -1;
	if (ddy > 1) ddy = 1; else if (ddy < -1) ddy = -1;

	// A pure diagonal step still means "ahead", so the caller reaching here
	// with a real step always ends in one of the three arms below and always
	// reports -- a zero-accept report is a queue refusal, not a skipped call.
	if (ddx == 0 && ddy == 0)
	{
		CoverageNoteAheadZones(owner == OWNER_CAMERA, false, 0);
		return;
	}

	int accepted = 0;
	if (ddx != 0 && ddy == 0)
	{
		int ax = centerX + 2 * ddx;
		if (EnqueueZoneByOwner(ax, centerY - 1, owner)) accepted++;
		if (EnqueueZoneByOwner(ax, centerY,     owner)) accepted++;
		if (EnqueueZoneByOwner(ax, centerY + 1, owner)) accepted++;
	}
	else if (ddx == 0 && ddy != 0)
	{
		int ay = centerY + 2 * ddy;
		if (EnqueueZoneByOwner(centerX - 1, ay, owner)) accepted++;
		if (EnqueueZoneByOwner(centerX,     ay, owner)) accepted++;
		if (EnqueueZoneByOwner(centerX + 1, ay, owner)) accepted++;
	}
	else
	{
		if (EnqueueZoneByOwner(centerX + 2 * ddx, centerY + ddy,     owner)) accepted++;
		if (EnqueueZoneByOwner(centerX + ddx,     centerY + 2 * ddy, owner)) accepted++;
		if (EnqueueZoneByOwner(centerX + 2 * ddx, centerY + 2 * ddy, owner)) accepted++;
	}
	CoverageNoteAheadZones(owner == OWNER_CAMERA, true, accepted);
}

void ProcessPreloadQueue(void* zoneMgr)
{
	// One zone in flight: the last one this function loaded is still pending.
	// An entry carried across a transition end does not count: it
	// belongs to the old location, and the new one's zones must not wait for
	// its registration.
	if (numPreloaded > 0 && preloadedZones[numPreloaded - 1].pending
	    && !preloadedZones[numPreloaded - 1].carried)
		return;
	if (numPreloaded >= MAX_PRELOADED)
		return;

	int gx, gy, owner;
	void* zoneEntry = NULL;
	// First-time zones and town cells are the game's (the handoff rule): a
	// zone the mod would load itself is skipped when its finalize would read
	// it as first-time or the cell lists a town, and the next queued zone is
	// looked at in the same frame (at most a few: one map lookup each).
	for (int pops = 0; pops < 8; ++pops)
	{
		if (cameraQueueNext < cameraQueueCount)
		{
			gx = cameraQueue[cameraQueueNext].gridX;
			gy = cameraQueue[cameraQueueNext].gridY;
			cameraQueueNext++;
			owner = OWNER_CAMERA;
		}
		else if (charQueueNext < charQueueCount)
		{
			gx = charQueue[charQueueNext].gridX;
			gy = charQueue[charQueueNext].gridY;
			charQueueNext++;
			owner = OWNER_CHARACTER;
		}
		else
		{
			return;
		}

		zoneEntry = GetZoneEntry(zoneMgr, gx, gy);
		if (!zoneEntry)
			return;
		if (!IsZoneAccessible(zoneEntry) && !IsZoneLoading(zoneEntry)
		    && ZlFirstTimeRuleOn() && ZlPredictFirstTime(gx, gy) != 0)
		{
			g_ftSkip++;
#ifdef KEO_DEBUG
			static unsigned char ftLogged[ZONE_GRID_COUNT] = { 0 };
			int c = ZoneCell(gx, gy);
			if (c >= 0 && !ftLogged[c])
			{
				ftLogged[c] = 1;
				std::ostringstream ss;
				ss << "First-time zone (" << gx << "," << gy
				   << ") not preloaded: the game loads and populates it";
				LogDebug(ss.str());
			}
#endif
			zoneEntry = NULL;
			continue;
		}
		break;
	}
	if (!zoneEntry)
		return;

	// Common fields for all three paths
	PreloadedZone& pz = preloadedZones[numPreloaded];
	pz.zoneEntry = zoneEntry;
	pz.gridX = gx;
	pz.gridY = gy;
	pz.gameOwned = false;
	pz.pending = false;
	pz.registered = false;
	pz.registeredEmpty = false;
	pz.contentProcessed = false;
	pz.loadTimeSec = ElapsedSec();
	pz.owner = owner;
	pz.carried = false;
	// Default: this slot did not come from our own loadSingleZone call this
	// pass. Overwritten below only on the real load branch.
	pz.pipeLoadSec = -1.0;
	pz.pipeIsReadySeen = false;
	pz.pipe264Seen = false;

	if (IsZoneAccessible(zoneEntry))
	{
		pz.gameOwned = true;
		numPreloaded++;
		return;
	}

	if (IsZoneLoading(zoneEntry))
	{
		// Registry guard: adopting a loading zone leads straight to
		// processContent on its content, so refuse one whose registration is
		// not this content's (a save-load survivor). The slot is not committed.
		if (!RegistryGuardPasses(zoneEntry, REG_SITE_ADOPT))
		{
			pz.zoneEntry = NULL;
			pz.gridX = -1;
			pz.gridY = -1;
			return;
		}
		{
			std::ostringstream ss;
			ss << "Tracking loading zone (" << gx << "," << gy << ")"
			   << (owner == OWNER_CAMERA ? " [cam]" : " [char]")
			   << " content=" << (*(void**)zoneEntry ? "yes" : "NULL");
			LogDebug(ss.str());
		}
		pz.pending = true;
		pendingCount++;
		numPreloaded++;
		ZlTouchZone(zoneEntry, ZL_ADOPTED176);
		return;
	}

	// Save/restore ready flag: loadSingleZone clears PhysicsInterface::_queuesClear
	// (physics+0x320), which makes NavMeshGenerator sleep and causes ~750ms stalls.
	// The read stays a plain load (single byte, no lock); the restore goes through
	// the game's setter, which takes queuesClearMuto (+0x328) around the write.
	uintptr_t physics = *(uintptr_t*)((uintptr_t)GameAddr(RVA_PAUSESTATE_PHYSICS));
	unsigned char savedReady = 0;
	if (physics)
		savedReady = *(unsigned char*)(KLIB_MEMBER(2, physics, PhysicsInterface__queuesClear, 800));

	// Guard: never let a preload attempt touch a zone the game already
	// owns. loadSingleZone (0xA0D6A0) writes the keep-alive timer at
	// zoneEntry + 4*(timerIndex + 48) BEFORE its already-loaded early return:
	//
	//     if ( keepAliveSeconds > 0.0 )
	//       *((float *)zoneEntry + v4 + 48) = keepAliveSeconds;      // A0D6F4
	//     else
	//       *((_DWORD *)zoneEntry + v4 + 48) = g_zoneKeepAliveDefaultSeconds[v4];
	//     if ( zoneEntry && (*((_BYTE *)zoneEntry + 176)
	//                     || *((_BYTE *)zoneEntry + 177)) )
	//       return 0;                                                // A0D70F
	//
	// So a call on an already-loaded zone does nothing except reset that zone's
	// unload timer — which changes when the game unloads sectors, and sector
	// unloading was in flight on the path thread in the crash second.
	//
	// The two IsZone* returns above already cover exactly this condition (+176 is
	// OFF_ZONE_IS_LOADING, +177 is OFF_ZONE_IS_ACCESS), so this test should never
	// fire. It is here as the explicit, named guarantee, and preloadSkipLoaded on
	// the transition line is the evidence: a non-zero count would mean the two
	// early returns above have a hole in them.
	if (IsZoneLoading(zoneEntry) || IsZoneAccessible(zoneEntry))
	{
		preloadSkipLoaded++;
#ifdef KEO_DEBUG
		// One line per zone: this runs in the preload queue, which revisits the
		// same coordinates every transition.
		static unsigned char skipLogged[(ZONE_GRID_MAX + 1) * (ZONE_GRID_MAX + 1)] = { 0 };
		int slot = ZoneCell(gx, gy);
		if (slot >= 0 && slot < (int)sizeof(skipLogged) && !skipLogged[slot])
		{
			skipLogged[slot] = 1;
			std::ostringstream ss;
			ss << "Preload skip: zone (" << gx << "," << gy
			   << ") already loaded by the game";
			LogDebug(ss.str());
		}
#endif
		// Record the slot the way the two branches above would have: a zone the
		// game has finished loading is game-owned, one still loading is not. If
		// this guard ever does fire, it must not leave behind a third kind of
		// slot (a not-game-owned, not-pending entry for a zone the game owns)
		// that no other code path knows how to reason about.
		pz.gameOwned = IsZoneAccessible(zoneEntry);
		numPreloaded++;
		return;
	}

	// loadSingleZone takes four arguments and the last is a float in xmm3
	// (game.h): the keep-alive seconds written at zoneEntry + 4*(timerIndex + 48),
	// which decide when the game unloads this zone again. 0 is what the game's own
	// processState2 passes (xorps xmm6,xmm6 / movaps xmm3,xmm6 before the call at
	// 0xA0D928), i.e. take the per-timer default. The zones reaching this line are
	// ones the game does not have, so the default is the value the game itself
	// would have written for them.
	//
	// preloadKeepAliveSeconds makes it tunable so a change to preloaded zones'
	// unload timing can be A/B'd (0 vs 3600) without a rebuild.
	bool result = game::g_gameFn.fn_loadSingleZone(zoneEntry, 0, 0, zone::g_zoneCfg.cfg_preloadKeepAliveSeconds);
	if (physics && savedReady && game::g_gameFn.fn_setQueuesAreClear)
		game::g_gameFn.fn_setQueuesAreClear((void*)physics, true);

	pz.pending = (result != 0);
	if (result != 0)
	{
		pendingCount++;
		ZoneHandoffNoteLoaded(zoneEntry, gx, gy);
		ZlTouchZone(zoneEntry, ZL_MOD_LOADED);
		pz.pipeLoadSec = pz.loadTimeSec;  // stable base; loadTimeSec itself is reused later
		g_pipe.loaded++;
	}
	numPreloaded++;

	if (result)
	{
		std::ostringstream ss;
		ss << "Preloaded zone (" << gx << "," << gy << ")"
		   << (owner == OWNER_CAMERA ? " [cam]" : " [char]");
		LogDebug(ss.str());
	}
}


bool TrySquadSwitchSwap(void* zoneMgr, int camGX, int camGY)
{
	int matches = 0;
	for (int i = 0; i < numPreloaded; ++i)
	{
		if (preloadedZones[i].owner != OWNER_CHARACTER)
			continue;
		int dx = preloadedZones[i].gridX - camGX;
		int dy = preloadedZones[i].gridY - camGY;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dx <= 1 && dy <= 1)
			matches++;
	}

	if (matches < 5)
		return false;

	for (int i = 0; i < numPreloaded; ++i)
	{
		if (preloadedZones[i].owner == OWNER_CAMERA)
			preloadedZones[i].owner = OWNER_CHARACTER;
	}

	int swapped = 0;
	for (int i = 0; i < numPreloaded; ++i)
	{
		if (preloadedZones[i].owner != OWNER_CHARACTER)
			continue;
		int dx = preloadedZones[i].gridX - camGX;
		int dy = preloadedZones[i].gridY - camGY;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dx <= 1 && dy <= 1)
		{
			preloadedZones[i].owner = OWNER_CAMERA;
			swapped++;
		}
	}

	FlushCameraQueue();
	predictedCenterX = camGX;
	predictedCenterY = camGY;

	// The same grid the camera prediction queues, through the same function:
	// a squad switch lands the camera somewhere new, and there is no reason
	// for the cells it needs to differ from the ones a walk into that cell
	// would have needed.
	int missing = EnqueueCameraGrid(camGX, camGY);

	std::ostringstream ss;
	ss << "Squad switch swap: (" << camGX << "," << camGY << ")"
	   << " " << swapped << " zones retagged"
	   << ", " << missing << " queued";
	LogMsg(ss.str());

	return true;
}
