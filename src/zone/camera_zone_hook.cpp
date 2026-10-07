// camera_zone_hook.cpp - Main-thread frame phases, camera focus and preloading.
// Replay deferred destroy-list inserts immediately after PreloadCheckSaveLoad,
// before any early return; the replay releases deferCS before calling the original.

#include "fixes/world/destroy_list_defer.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include "zone/preload/preload.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "navmesh/construction/wall_splice.h"
#include "inventory/backpack_sidecar.h"
#include "inventory/job_counters.h"
#include "zone/camera_zone_hook.h"
#include "zone/transition_hook.h"
#include "navmesh/cache/nm_force_rebuild.h"
#include "zone/hooks_internal.h"
#include "diag/mem_probe.h"
#include "zone/preload/coverage_stats.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "pathfind/pathfinding.h"
#include "pathfind/player_repath_tier.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "movement/mover_policy.h"
#include "movement/islands.h"
#include "pathfind/path_pool.h"
#include "pathfind/gate_pass.h"
#include "render/render_levers.h"
#include "bench/bench_runner.h"
#include "bench/bench_sweep.h"
#include "fixes/physx/purecall_record.h"
#include "fixes/physx/physx_query_guard.h"
#include "fixes/streaming/create_instance_guard.h"
#include "fixes/physx/hull_queue_guard.h"
#include "fixes/stitch/stitch_source.h"
#include "fixes/stitch/stitch_byte_guard.h"
#include "diag/physx_pool_probe.h"
#include "fixes/world/corpse_pin.h"
#include "fixes/world/throwout.h"
#include "zone/handoff/zone_handoff.h"
#include "zone/geometry/zone_geometry_epoch.h"
#include "zone/retention/zone_retention.h"
#include "zone/preload/camera_focus.h"
#include "planner/coarse_graph_base.h"
#include "planner/planner_tick.h"
#include "plugin/profiler_image.h"

// =========================================================================
// NavMesh scheduling helpers (builds context from preload/tracking state)
// =========================================================================

void BuildSchedContext(SchedContext* ctx)
{
	// Resolve camera: use transition target if active, else lastCameraGX/GY
	ctx->camX = lastCameraGX;
	ctx->camY = lastCameraGY;
	if (isTransitionActive && g_cachedZoneMgr)
	{
		void* tz = *(void**)(KLIB_MEMBER(2, (uintptr_t)g_cachedZoneMgr, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE));
		if (tz) { ctx->camX = GetZoneGridX(tz); ctx->camY = GetZoneGridY(tz); }
	}

	int moverCount = numWatched;
	if (moverCount > MAX_WATCHED) moverCount = MAX_WATCHED;
	for (int i = 0; i < moverCount; ++i)
	{
		ctx->movers[i].currentX = watchedChars[i].currentZoneX;
		ctx->movers[i].currentY = watchedChars[i].currentZoneY;
		ctx->movers[i].destX    = watchedChars[i].destZoneX;
		ctx->movers[i].destY    = watchedChars[i].destZoneY;
		ctx->movers[i].hasMoveOrder = watchedChars[i].hasMoveOrder;
	}
	ctx->moverCount = moverCount;

	int zoneCount = numPreloaded;
	if (zoneCount > MAX_PRELOADED) zoneCount = MAX_PRELOADED;
	for (int i = 0; i < zoneCount; ++i)
	{
		ctx->zones[i].gridX = preloadedZones[i].gridX;
		ctx->zones[i].gridY = preloadedZones[i].gridY;
	}
	ctx->zoneCount = zoneCount;
}

// =========================================================================
// Camera focus (bug report: zoomed out and pitched behind the squad, the
// camera's own position sits back over the zone the squad is leaving, so the
// eye-position prediction below never sees the upcoming zone). Replaces the
// eye position with the game's orbit/follow anchor (CameraClass::center,
// read through PlayerInterface::getCameraCenter -- the same point
// SectionManager::perFrameUpdate anchors streaming on), clamped near the
// squad. The resulting point leads the eye by the follow-anchor's offset, so
// prediction can fire slightly earlier even with the camera close in --
// intended, not a regression. Pure decision and hysteresis logic:
// camera_focus.h/.cpp (host-tested).
// =========================================================================

static long   s_camFocusCounts[6]  = { 0, 0, 0, 0, 0, 0 };  // indexed by CameraFocusReason
static double s_camFocusLastLog    = 0.0;
static long   s_camFocusHeldCount  = 0;   // frames the prediction axis hysteresis held a border
static long   s_camFocusSrcResets  = 0;   // frames the point source flipped focus<->eye

// Per-axis prediction state for hook_updateCameraZone's threshold check
// below (-1/0/+1 relative to the current zone); reset wherever
// lastCameraGX/GY also reset, on a squad-switch jump, and whenever
// CameraFocusApply's point source changes (see below).
static int s_predOffsetX = 0;
static int s_predOffsetZ = 0;

// Whether the previous frame's point was the continuous focus or the eye
// fallback; CameraFocusApply resets the hysteresis above the frame this
// changes, in either direction, so a hold from one source never carries
// into the other. No prior frame yet -> no reset, nothing to carry over.
static bool s_camFocusHaveSource   = false;
static bool s_camFocusWasFocusSrc  = false;

void ResetCameraFocusState()
{
	s_predOffsetX = 0;
	s_predOffsetZ = 0;
}

// The point to use in place of the raw camera position this frame, fed
// straight into the threshold-based prediction math below: continuous, never
// quantized to a zone center. Every fallback case (detached, invalid, far)
// hands back eyeX/eyeZ completely unmodified, so the threshold math sees
// exactly what it always has on those frames.
static void CameraFocusApply(float eyeX, float eyeZ, double now, float* outX, float* outZ)
{
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));

	CameraFocusInput fin;
	fin.eyeX = eyeX;
	fin.eyeZ = eyeZ;
	fin.haveFocus = GetCameraFocusXZ(playerIntf, &fin.focusX, &fin.focusZ);
	fin.detached  = IsCameraDetached(playerIntf);

	float refX = fin.haveFocus ? fin.focusX : eyeX;
	float refZ = fin.haveFocus ? fin.focusZ : eyeZ;
	fin.haveAnchor = FindNearestPlayerCharacterXZ(refX, refZ, &fin.anchorX, &fin.anchorZ);

	float zoneWidth = (zoneStepX > 0.0f && zoneStepZ > 0.0f) ? (zoneStepX + zoneStepZ) * 0.5f : 8192.0f;
	float maxDist  = (zone::g_zoneCfg.cfg_camFocusMaxDist > 0.0f) ? zone::g_zoneCfg.cfg_camFocusMaxDist : zoneWidth;
	float hardDist = maxDist * zone::g_zoneCfg.cfg_camFocusHardMult;

	CameraFocusResult res = ComputeCameraFocusPoint(fin, maxDist, hardDist);

	// Reject a result far outside the world grid outright (coordinate-space
	// mismatch or a stray huge distance) instead of letting WorldToZoneGrid
	// silently clamp it onto a border cell.
	float relX = (res.x - zoneOriginX) / zoneStepX;
	float relZ = (res.z - zoneOriginZ) / zoneStepZ;
	if (!CameraFocusPointInGrid(relX, relZ, 63))
	{
		res.reason = CAMFOCUS_INVALID;
		res.x = eyeX;
		res.z = eyeZ;
	}

	s_camFocusCounts[res.reason]++;
	*outX = res.x;
	*outZ = res.z;

	// The point handed to the prediction math below just switched between
	// the continuous focus and the raw eye (or back): any offset the axis
	// hysteresis is holding was computed against the other source's
	// geometry and must not carry over. Reset before this frame's
	// prediction check runs.
	bool isFocusSrc = CameraFocusReasonIsFocusSource(res.reason);
	if (s_camFocusHaveSource && isFocusSrc != s_camFocusWasFocusSrc)
	{
		ResetCameraFocusState();
		s_camFocusSrcResets++;
	}
	s_camFocusWasFocusSrc = isFocusSrc;
	s_camFocusHaveSource  = true;

	if (now - s_camFocusLastLog > zone::g_zoneCfg.cfg_camLogInterval)
	{
		s_camFocusLastLog = now;
		std::ostringstream ss;
		ss << "camFocus: used=" << s_camFocusCounts[CAMFOCUS_USED]
		   << " clamped=" << s_camFocusCounts[CAMFOCUS_CLAMPED]
		   << " far=" << s_camFocusCounts[CAMFOCUS_FAR]
		   << " horizon=" << s_camFocusCounts[CAMFOCUS_HORIZON]
		   << " invalid=" << s_camFocusCounts[CAMFOCUS_INVALID]
		   << " detached=" << s_camFocusCounts[CAMFOCUS_DETACHED]
		   << " held=" << s_camFocusHeldCount
		   << " srcReset=" << s_camFocusSrcResets;
		LogDebug(ss.str());
	}
}


static double lastCamLogTime = 0.0;

namespace hooks_detail
{

// Main thread: probe and pending completion precede the original; no lock spans phases.
static void CameraZoneFramePreamble(void* zoneMgr, void* cameraPos)
{
	// destroyListOE diagnostic (destroy_list_defer.h). The game calls the destroy-list drain earlier
	// in this same frame (GameWorld__mainLoop_GPUSensitiveStuff calls it at
	// +0x1B0 and updateCameraZone at +0x385), so this samples the container the
	// drain has just left, one frame before a stale count would fault.
	DestroyListProbeTick();

	// Finish a transition whose dismissal arrived on the path thread.
	// Runs before anything else so the stats line and the preload reset happen
	// on the first main-thread frame after the loading screen goes away.
	TransitionCompleteIfPending();

	// Always call original first (visual activate/deactivate)
	game::g_hookOrig.orig_updateCameraZone(zoneMgr, cameraPos);

	g_cachedZoneMgr = zoneMgr;

	// Per-state frame count for the open bracket (main thread).
	CountBracketFrame(zoneMgr);
}

// Main thread: check the save-load edge, then replay; deferCS is released before original inserts.
static bool CameraZoneSaveLoad(void* zoneMgr)
{
	// Drop all mod state across a save load, and do no preload work while
	// the game is loading one (every pointer we would cache is about to die).
	bool saveLoading = PreloadCheckSaveLoad(zoneMgr);
	planner::PlannerOnFrame(zoneMgr, saveLoading);

	// destroyListOE mitigation (destroy_list_defer.h): perform the inserts that background threads
	// queued instead of writing the container themselves. It runs after the
	// probe, so the probe still reads the state the frame's drain left behind;
	// after the frame's drain (GameWorld__mainLoop_GPUSensitiveStuff calls the
	// drain at +0x1B0 and updateCameraZone at +0x385), so a deferred object is
	// destroyed by the next frame's drain; and after the save-load check, whose
	// rising edge calls DestroyListDropDeferred — a world clear must empty the
	// queue before the flush could replay a pointer the clear freed. It runs
	// even while a load is in progress, so the queue cannot sit at its cap.
	DestroyListFlushDeferred();
	return saveLoading;
}

// Main thread: all periodic ticks precede the save-loading return; callees keep their own locks.
static bool CameraZoneTicks(void* zoneMgr, bool saveLoading)
{
	// Lines the NavMesh bg thread queued (lazy hook install results, the bg
	// TID, worker creation: core.h, LogMsgDeferrable). Every frame, ahead of
	// the save-load / preload / camera returns below, so they land within a
	// frame even when the first dispatch happens during a load.
	FlushDeferredLogLines();

	// Every frame, save load included: path-worker-pool instrumentation (it
	// detects save loads itself), then the
	// readiness config/target/Readiness lines (cumulative session counters,
	// no game pointers kept).
	double tickNow = ElapsedSec();
	PathPoolTickMain(tickNow);
	GatePassTickMain();
	ReadinessReportTick(tickNow);
	RenderLeversMainThreadTick(saveLoading);
	BenchMainThreadTick(saveLoading);
	BenchSweepMainThreadTick(tickNow);
	ProfilerImageTickMain(tickNow);
	PurecallRecordTick(tickNow);
	PhysQueryGuardTick(tickNow);
	CreateInstanceGuardTick(tickNow);
	HullQueueGuardTick(tickNow);
	StitchByteGuardTick(tickNow);
	PhysXPoolTick(tickNow);
	StitchSourceTick(tickNow);
	NavMeshAdjTick(tickNow);
	// The rebuild key's marks and panel hold; a dismissal it held is issued here
	// once the zone manager is idle, through the hooked entry.
	if (NmForceRebuildTick(zoneMgr, saveLoading))
		hook_showLoadingMessage(GameAddr(RVA_GLOBAL_GUI), false);
	// Ahead of every early return below, and gated by nothing: a memory figure
	// that stopped printing under the conditions it exists to describe -- a
	// save load, a transition, a disabled feature -- would answer nothing.
	LogMemoryStats(tickNow);
	// For the same reason, and it matters more here: navmesh generation runs
	// on its own threads, through save loads and transitions and whether or
	// not preloading is on. A report placed below the returns would be
	// silent about exactly the sessions worth reading.
	ZoneGeometryCertTick(tickNow);
	navmesh::WallSpliceTick(tickNow, saveLoading);
	fixes::ThrowoutTick(tickNow, saveLoading);
	keo_inventory::BackpackRekeyTick(tickNow, saveLoading);
	keo_inventory::OperatorTripsTick(tickNow, saveLoading);
	// Same placement, same reason: the coverage counters describe preload
	// paths that run through transitions and save loads, and the transition
	// line only prints at a bracket close. A session with no transition at
	// all would otherwise show nothing, which reads like the mod not being
	// loaded rather than like coverage never being reached.
	if (CoverageDueForPeriodicReport(tickNow))
		LogMsg("Coverage:" + CoverageStatsToken());

#if ZONEHAND_STEP >= 2
	// Ahead of every early return below: the readiness classifier's global
	// override is "a save is loading", the same condition that skips the
	// rest of this function's work.
	ZoneHandoffNoteWorldState(zoneMgr);
	// Also ahead of them, and for the same reason turned around: these open
	// the frame for work that happens later in it, in ZoneMap::update, which
	// the game calls whatever this function decides to skip.
	ZoneHandoffBeginFrame();
	ZoneRetentionBeginFrame(zoneMgr);
#endif

	if (saveLoading)
	{
		// Drop the previous session's HavokCharacter* set rather than let it
		// sit unrefreshed through the load: a reused address on the far side
		// could otherwise match a character it was never published for.
		PlayerRepathTierClear();
		return false;
	}
	return true;
}

// Main thread: sample reporting time after save-load refusal, before preload gates.
static double CameraZoneReports(void* zoneMgr)
{
	double now = ElapsedSec();

	// Everything from here to the preload return below observes and reports;
	// none of it preloads. It runs ahead of that return so a run with preload
	// off is still a run that can be read -- the control and the arm it is a
	// control for have to print the same tokens, or the control measures
	// nothing.

	// First, because world-to-zone conversion is what several of those
	// reports are made of, and each of them returns early while the grid is
	// uncalibrated. It reads the zone manager and nothing else, and does its
	// work once.
	if (!gridCalibrated)
		CalibrateZoneGrid(zoneMgr);

	// Island routing overlay: Set B signature, component rebuild, parked-squad
	// re-issue, diagnostics (main thread; hooks read the published snapshot)
	IslandTick(zoneMgr, now);
	planner::PlannerTick(zoneMgr, now);

	// Republish the player-owned HavokCharacter* set every frame (main
	// thread; hook_requestPath reads the published snapshot, main or AI
	// back thread)
	PlayerRepathTierPublish(now);

	// NavMesh cache diagnostic: report background thread job count
	LogNavMeshCacheStats(now);

	// Pathfinding diagnostic: report A* search budget stats
	LogPathfindDiagStats(now);
	LogPhase12Stats(now);
	LogPathGuardStats(now);

	// Player movement state: the engine's arrival flag is transient, so it is
	// latched every frame; the stuck report itself throttles to 1 s.
	SamplePlayerArrivals();
	PollPlayerMovementState(now);

	// Multi-call path probe: dump captured entries after window expires
	DumpPathProbe(now);
	return now;
}

// Main thread: preload and camera-pointer gates run after all reports.
static bool CameraZonePreloadGates(void* cameraPos)
{
	if (!zone::g_zoneCfg.preloadEnabled)
		return false;

	if (!cameraPos)
		return false;
	return true;
}

// Main thread: focus, adoption, eviction and queue priority precede the idle-only preload work.
static bool CameraZoneFocusAndPreload(void* zoneMgr, void* cameraPos, double now)
{
	// The raw eye position (today's behaviour, and the always-safe fallback).
	float eyeX = (*(const float*)KLIB_MEMBER(5, cameraPos, Ogre__Vector3_x, 0));
	float eyeZ = (*(const float*)KLIB_MEMBER(5, cameraPos, Ogre__Vector3_z, 8));
	float focusX = eyeX, focusZ = eyeZ;
	if (gridCalibrated && zone::g_zoneCfg.cfg_camFocusEnabled)
		CameraFocusApply(eyeX, eyeZ, now, &focusX, &focusZ);

	// Compute camera grid coords once for jump detection + debug logging
	int camGX = -1, camGY = -1;
	bool haveCamGrid = false;
	if (gridCalibrated)
		haveCamGrid = WorldToZoneGrid(focusX, focusZ, &camGX, &camGY);

	// Squad-switch detection: sudden camera zone jump (Manhattan > 2)
	bool swapped = false;
	if (haveCamGrid && lastCameraGX >= 0 && lastCameraGY >= 0)
	{
		int jumpX = camGX - lastCameraGX;
		int jumpY = camGY - lastCameraGY;
		if (jumpX < 0) jumpX = -jumpX;
		if (jumpY < 0) jumpY = -jumpY;
		if (jumpX + jumpY > 2)
		{
			swapped = TrySquadSwitchSwap(zoneMgr, camGX, camGY);
			// The jump itself (character switch, teleport) makes any held
			// prediction axis stale; the new reading above is already fresh.
			ResetCameraFocusState();
		}
	}
	if (haveCamGrid)
	{
		lastCameraGX = camGX;
		lastCameraGY = camGY;
	}

	// Register pending zones only outside transitions. Below ZONEHAND_STEP 2
	// a registered zone has no adoption path: it stalls out on the pending
	// timeout and the zombie handler unloads or hands it back.
	if (pendingCount > 0 && !isTransitionActive)
		TryRegisterPreloadedZones(zoneMgr, now);

#if ZONEHAND_STEP >= 2
	// The main-thread safe point the design names: after the AI join, after
	// the save-load check and the deferred destroy replay above, and outside
	// any native walk of the sets it touches.
	ZoneHandoffTick(zoneMgr, now);
	// After it, so a cell adopted this frame already has its lease.
	ZoneRetentionTick(zoneMgr, now);
#endif

	// Evict stale/unloaded zones periodically to free preload slots
	static double lastEvictTime = 0.0;
	if (numPreloaded > 0 && now - lastEvictTime > 2.0)
	{
		EvictStaleZones(zoneMgr, now);
		lastEvictTime = now;
	}

	// The ZoneLeak: line and the unload pass. After
	// PreloadCheckSaveLoad and the destroyListOE replay above, after this
	// frame's camera cell, registration and eviction; never during
	// a save load (returned above).
	ZoneLifeTick(zoneMgr, now);

	// Unconditional: prints whether the fix is on or off, so an off session
	// still shows the counters at zero rather than saying nothing.
	CorpsePinTick(now);

	// Periodic navmesh queue reprioritization (5-tier system).
	// Ensures character path zones and camera zones are always
	// processed first by the NavMesh background thread.
	static double lastReprioritizeTime = 0.0;
	{
		// Only the player-order case (above) reprioritizes immediately; the
		// backstop interval alone drives this pass. With the fast cadence it runs on
		// reprioritizeInterval whatever is tracked, because a pass over an
		// empty queue returns before taking the queue lock; without it the
		// slow backstop runs only while something is tracked or queued.
		bool hasWork = (numWatched > 0 || pendingCount > 0);
		double interval = navmesh::g_navmeshCfg.reprioFastEnabled ? navmesh::g_navmeshCfg.cfg_reprioritizeInterval : 3.0;
		int reason = ReprioDue(false, now, lastReprioritizeTime, interval,
		                       hasWork, navmesh::g_navmeshCfg.reprioFastEnabled);
		if (reason != REPRIO_NONE)
		{
			CallPrioritizeNavMeshQueue();
			lastReprioritizeTime = now;
			InterlockedIncrement(&reprioTimerFires);
		}
	}


	// Only preload during idle state
	int state = GetZoneState(zoneMgr);
	if (state != 0)
		return false;

	// Process one zone from the priority queue per frame
	ProcessPreloadQueue(zoneMgr);

	// --- Debug: log zone under camera every ~10 seconds ---
	if (haveCamGrid && now - lastCamLogTime > CAM_LOG_INTERVAL)
	{
		lastCamLogTime = now;
		void* camZone = GetZoneEntry(zoneMgr, camGX, camGY);
		if (camZone)
		{
			int loading = IsZoneLoading(camZone) ? 1 : 0;
			int accessible = IsZoneAccessible(camZone) ? 1 : 0;
			void* content = *(void**)camZone;

			const char* ourStatus = "not-ours";
			for (int i = 0; i < numPreloaded; ++i)
			{
				if (preloadedZones[i].gridX == camGX && preloadedZones[i].gridY == camGY)
				{
					ourStatus = preloadedZones[i].gameOwned ? "game-owned" :
					            preloadedZones[i].pending  ? "pending" : "tracked";
					break;
				}
			}

			std::ostringstream ss;
			ss << "CamZone (" << camGX << "," << camGY << ")"
			   << " load=" << loading << " access=" << accessible
			   << " content=" << (content ? "yes" : "NULL")
			   << " status=" << ourStatus;
			LogMsg(ss.str());
		}
	}

	// --- Camera-based prediction (skip if swap just handled it) ---
	if (!swapped)
	{
		void* currentZone = *(void**)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE));
		if (currentZone)
		{
			float playerX = focusX;
			float playerZ = focusZ;

			float centerX = GetZoneCenterX(currentZone);
			float centerZ = GetZoneCenterZ(currentZone);

			float dx = playerX - centerX;
			float dz = playerZ - centerZ;

			// Per-axis hysteresis only while camFocus is on; camFocus=false
			// reproduces the original plain-threshold check exactly (margin 0).
			float margin = zone::g_zoneCfg.cfg_camFocusEnabled ? zone::g_zoneCfg.cfg_camFocusHysteresis : 0.0f;
			CameraFocusAxisResult rx = CameraFocusAxisHysteresis(s_predOffsetX, dx, PRELOAD_THRESHOLD, margin);
			CameraFocusAxisResult rz = CameraFocusAxisHysteresis(s_predOffsetZ, dz, PRELOAD_THRESHOLD, margin);
			s_predOffsetX = rx.offset;
			s_predOffsetZ = rz.offset;
			if (rx.held || rz.held)
				s_camFocusHeldCount++;

			if (rx.offset == 0 && rz.offset == 0)
			{
				predictedCenterX = -1;
				predictedCenterY = -1;
			}
			else
			{
				int currentX = GetZoneGridX(currentZone);
				int currentY = GetZoneGridY(currentZone);

				int targetX = currentX + rx.offset;
				int targetY = currentY + rz.offset;

				if (targetX < 0) targetX = 0;
				if (targetX > 63) targetX = 63;
				if (targetY < 0) targetY = 0;
				if (targetY > 63) targetY = 63;

				if (targetX != predictedCenterX || targetY != predictedCenterY)
				{
					predictedCenterX = targetX;
					predictedCenterY = targetY;

					// Flush stale camera predictions before enqueueing new grid
					FlushCameraQueue();

					std::ostringstream ss;
					ss << "Preload triggered: predicting (" << targetX << "," << targetY
					   << ") from (" << currentX << "," << currentY << ")";
					LogDebug(ss.str());

					EnqueueCameraGrid(targetX, targetY);
					EnqueueAheadZones(targetX, targetY, currentX, currentY, OWNER_CAMERA);
				}
			}
		}
	}

	// --- Character polling (tiered, or a periodic scan fallback) ---
	if (gridCalibrated)
	{
		if (zone::g_zoneCfg.movementAwareEnabled)
			TieredCharacterPoll(zoneMgr, now);
		else if (now - lastCharScanTime > CHAR_SCAN_INTERVAL)
		{
			ScanCharacterZones(zoneMgr);
			lastCharScanTime = now;
		}
	}

	// --- Group cohesion: poll formation groups for arrival scatter ---
	if (movement::g_movementCfg.groupCohesionEnabled && scatterPatchApplied)
		PollFormationGroups();
	return true;
}

} // namespace hooks_detail
using namespace hooks_detail;

// =========================================================================
// Hook 3: updateCameraZone -- preloading + zone adoption trigger
// =========================================================================

void hook_updateCameraZone(void* zoneMgr, void* cameraPos)
{
	CameraZoneFramePreamble(zoneMgr, cameraPos);
	bool saveLoading = CameraZoneSaveLoad(zoneMgr);
	if (!CameraZoneTicks(zoneMgr, saveLoading))
		return;
	double now = CameraZoneReports(zoneMgr);
	if (!CameraZonePreloadGates(cameraPos))
		return;
	if (!CameraZoneFocusAndPreload(zoneMgr, cameraPos, now))
		return;
}
