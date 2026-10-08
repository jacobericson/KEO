// hook_manifest.cpp — the build gate table and the per-row install state, both
// expanded from plugin/hook_manifest_rows.inc so they share one index, and
// InstallHooks, which runs every startup install in a fixed order.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/destroy_list_defer.h"
#include "game/game.h"
#include "base/config.h"   // ZONEHAND_STEP: the rows are gated like their installs
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <sstream>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include "base/klib_include_end.h"
#include "zone/transition_hook.h"
#include "zone/readiness/readiness_hook.h"
#include "zone/camera_zone_hook.h"
#include "pathfind/order_hook.h"
#include "zone/preload/preload.h"
#include "zone/zone_pause.h"
#include "zone/handoff/zone_lifecycle_hooks.h"
#include "navmesh/nm_workers.h"
#include "navmesh/cache/nm_force_rebuild.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include "navmesh/construction/wall_splice.h"
#include "navmesh/construction/splice_seam.h"
#include "movement/formation.h"
#include "movement/islands.h"
#include "pathfind/pathfinding.h"
#include "pathfind/path_pool.h"
#include "pathfind/astar_cost.h"
#include "pathfind/astar_hier_policy.h"
#include "pathfind/gate_pass.h"
#include "bench/bench_runner.h"
#include "fixes/world/corpse_pin.h"
#include "fixes/world/faction_relations.h"
#include "inventory/backpack_first.h"
#include "fixes/world/town_claim.h"
#include "fixes/world/throwout.h"
#include "inventory/backpack_sidecar.h"
#include "inventory/backpack_food.h"
#include "inventory/operator_trips.h"
#include "inventory/job_counters.h"
#include "inventory/backpack_window.h"
#include "movement/formation_pace.h"
#include "planner/coarse_graph_base.h"
#if ZONEHAND_STEP >= 2
#include "fixes/world/nest_validation.h"
#endif
#include "fixes/stitch/unstitch_guard.h"
#include "fixes/world/onscreen_stagger.h"
#include "fixes/world/paused_skip.h"
#include "fixes/stitch/stitch_source.h"
#include "fixes/search/graph_visitor_guard.h"
#include "fixes/search/graph_expand_guard.h"
#include "fixes/search/graph_position_guard.h"
#include "fixes/search/graph_heuristic_guard.h"
#include "fixes/search/cluster_cross_cost.h"
#include "planner/planner_hooks.h"
#include "fixes/streaming/create_instance_guard.h"
#include "fixes/physx/hull_queue_guard.h"
#include "fixes/physx/hull_same_skip.h"
#include "fixes/streaming/mesh_face_guard.h"
#include "fixes/streaming/navmesh_life.h"
#include "fixes/stitch/unstitch_probe.h"
#include "render/scene_levers.h"
#include "render/d3d_state_skip.h"
#include "fixes/streaming/section_key_probe.h"
#include "diag/physx_pool_probe.h"
#include "render/ogre_join_spin.h"

// `extern` is required: a const object at namespace scope has internal
// linkage in C++ without it, and plugin_entry.cpp needs this one.
extern const HookPrologue g_hookPrologues[] =
{
#define HOOK_ROW(id, name, rva, kind, installer, want, caps, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15) \
	{ name, rva, { b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15 }, kind == HOOK_DIAGNOSTIC },
#include "plugin/hook_manifest_rows.inc"
#undef HOOK_ROW
};
static_assert(sizeof(g_hookPrologues) / sizeof(g_hookPrologues[0]) == HOOK_ROW_COUNT, "one gate row per manifest row");

extern const int g_hookPrologueCount = (int)(sizeof(g_hookPrologues) / sizeof(g_hookPrologues[0]));

static const HookWant s_wants[] =
{
#define HOOK_ROW(id, name, rva, kind, installer, want, ...) want,
#include "plugin/hook_manifest_rows.inc"
#undef HOOK_ROW
};
static_assert(sizeof(s_wants) / sizeof(s_wants[0]) == HOOK_ROW_COUNT, "one want per manifest row");

static volatile LONG s_installed[HOOK_ROW_COUNT] = { 0 };
// Written once per row by the startup gate on the main thread before any
// install; read by HookInstallRow on any thread.
static volatile LONG s_gatePassed[HOOK_ROW_COUNT] = { 0 };

const char* HookInstallRow(HookRowId id, void* detour, void** orig, int* installed,
                           bool reverify)
{
	uintptr_t rva = g_hookPrologues[id].rva;
	bool reverifyOk = reverify && VerifyPrologueByRva(rva);
	if (HookInstallAdmit(reverify, reverifyOk,
	                     InterlockedCompareExchange(&s_gatePassed[id], 0, 0) != 0) != HOOK_ADMIT)
	{
		*orig = NULL;
		return "prologue";
	}
	if (KenshiLib::AddHook(GameAddr(rva), detour, orig) != KenshiLib::SUCCESS)
	{
		*orig = NULL;
		return "hook";
	}
	InterlockedExchange(&s_installed[id], 1);
	if (installed)
		++*installed;
	return NULL;
}

bool HookRowInstalled(HookRowId id)
{
	return InterlockedCompareExchange(&s_installed[id], 0, 0) != 0;
}

void HookRowNoteGateVerdict(HookRowId id, bool passed)
{
	InterlockedExchange(&s_gatePassed[id], passed ? 1 : 0);
}

HookWantInputs HookWantInputsNow()
{
	return HookWantInputsFromConfig();
}

bool HookRowWanted(HookRowId id)
{
	return HookWantEval(s_wants[id], HookWantInputsNow());
}


// =========================================================================
// The startup groups
// =========================================================================
//
// Main thread, startPlugin, once each, in kInstallSteps' order. A row whose
// want gates its install reads HookRowWanted at that moment, so a key an
// earlier failure cleared still skips the install; the banner's total is
// taken before any of them runs (InstallHooks).

static void InstallLoadingHooks(int* installed, int*)
{
	// Loading-message and readiness-bypass hooks
	if (HookInstall(HOOK_SHOW_LOADING_MESSAGE, hook_showLoadingMessage,
			&game::g_hookOrig.orig_showLoadingMessage, installed, false) != NULL)
		LogError("FAILED to hook showLoadingMessage");

	if (HookInstall(HOOK_IS_CONTENT_PENDING, hook_isContentPending,
			&game::g_hookOrig.orig_isContentPending, installed, false) != NULL)
	{
		LogError("FAILED to hook isContentPending");
		zone::g_zoneCfg.preloadEnabled = false;  // registration readiness requires isContentPending
	}

	// Camera-zone orchestrator hook
	if (HookInstall(HOOK_UPDATE_CAMERA_ZONE, hook_updateCameraZone,
			&game::g_hookOrig.orig_updateCameraZone, installed, false) != NULL)
		LogError("FAILED to hook updateCameraZone");
}

// GameWorld::destroyListOE inserter. Two jobs on one hook: the race
// mitigation (destroyListDefer — queue off-main inserts and replay them on
// the main thread, core.h) and the thread-id diagnostic (destroyListDiag).
// Either key alone justifies the install; the mitigation is on by default.
// VerifyPrologue before the patch, and the table row is fatal.
static void InstallDestroyListHook(int* installed, int*)
{
	SetDestroyListDefer(fixes::g_fixesCfg.destroyListDeferEnabled);
	if (HookRowWanted(HOOK_DESTROYLIST_INSERT))
	{
		if (HookInstall(HOOK_DESTROYLIST_INSERT, hook_destroyListInsert,
				&orig_destroyListInsert, installed, true) == NULL)
		{
			std::ostringstream dls;
			dls << "destroyListOE inserter hook installed: defer="
			    << (fixes::g_fixesCfg.destroyListDeferEnabled ? "on" : "off")
			    << " diag=" << (fixes::g_fixesCfg.destroyListDiagEnabled ? "on" : "off");
			LogMsg(dls.str());
		}
		else
		{
			orig_destroyListInsert = NULL;
			SetDestroyListDefer(false);
			LogError("FAILED to hook destroyListOE inserter (sub_140799BE0)");
		}
	}
}

// The save-load reset's Set A/B unload (sub_14036C1E0). The
// hook unloads the mod's zones the reset would otherwise leave alive with a
// wiped handle registry, then clears the
// mod's state there. Installed in every build: the zones it unloads exist
// only when the mod preloads, but with none it only scans and logs. The
// INI key saveLoadUnload chooses unload vs count-only, not the install.
// VerifyPrologue before the patch, and the table row is fatal.
static void InstallResetUnloadHook(int* installed, int*)
{
	if (HookInstall(HOOK_RESET_UNLOAD_ZONES, hook_resetUnloadZones,
			&orig_resetUnloadZones, installed, true) == NULL)
	{
		LogMsg(std::string("Save-load reset hook installed: saveLoadUnload=")
		       + (zone::g_zoneCfg.saveLoadUnloadEnabled ? "on" : "off"));
	}
	else
	{
		orig_resetUnloadZones = NULL;
		LogError("FAILED to hook the save-load reset unload (sub_14036C1E0); "
		         "the ZM+8 edge still clears the mod's state, and the registry guard "
		         "refuses surviving zones");
	}
}

// Movement-order tracking hook
static void InstallOrderHook(int* installed, int*)
{
	if (HookRowWanted(HOOK_ADD_ORDER_SELECTED))
	{
		if (HookInstall(HOOK_ADD_ORDER_SELECTED, hook_addOrderSelected,
				&game::g_hookOrig.orig_addOrderSelected, installed, false) != NULL)
			LogError("FAILED to hook addOrderSelectedCharacters");
	}
}

static void OrderAbortStep(int*, int*)
{
	BenchRunnerSetOrderAbort(game::g_hookOrig.orig_addOrderSelected != NULL);
}

// Island routing overlay. Both hooks or neither go live —
// either one alone leaves a stall symptom unfixed or parks a squad at the
// destination zone's boundary. islandFix=false keeps them passing through.
//
// Installed whatever preload does: the overlay seeds no components either
// way, so both hooks answer vanilla, and the census they keep is the only
// reading of what the game's own islands do -- which a preload=false
// control exists to take.
static void InstallIslandHooks(int* installed, int*)
{
	int islandInstalled = 0;

	if (HookInstall(HOOK_ISINISLAND_IMPL, hook_isInIsland,
			&game::g_hookOrig.orig_isInIsland, installed, false) == NULL)
		islandInstalled++;
	else
		LogError("FAILED to hook ZoneMap::isInIsland");

	if (HookInstall(HOOK_GETISLAND_IMPL, hook_getIsland,
			&game::g_hookOrig.orig_getIsland, installed, false) == NULL)
		islandInstalled++;
	else
		LogError("FAILED to hook ZoneManager::getIsland");

	IslandSetHooksInstalled(islandInstalled == 2);

	std::ostringstream is;
	is << "Island routing: "
	   << (islandInstalled == 2 ? "both hooks installed" : "PARTIAL install, overlay disabled")
	   << (IslandHooksLive() ? " (live)" : " (pass-through)")
	   << " step=" << 4;
	LogMsg(is.str());
}

// The player's own cancels (stop key, job orders, nearest-
// character task orders): each forwards to the original, then drops the
// cancelled characters' tracked orders (islands.cpp). All three rows are
// diagnostic (hook_manifest_rows.inc): a mismatch refuses that hook alone, and
// IslandSetCancelHooksInstalled tells the caller which ones it has. Independent of
// preloadEnabled: they only observe PlayerInterface calls.
static void InstallCancelHooks(int* installed, int*)
{
	bool stopOk = false;
	bool jobOk = false;
	bool taskOk = false;

	if (HookInstall(HOOK_STOP_CHARACTERS_MOVEMENT, hook_stopCharactersMovement,
			&game::g_hookOrig.orig_stopCharactersMovement, installed, true) == NULL)
		stopOk = true;
	else
	{
		game::g_hookOrig.orig_stopCharactersMovement = NULL;
		LogError("FAILED to hook PlayerInterface::stopCharactersMovement (island cancel)");
	}

	if (HookInstall(HOOK_ADD_JOB_SELECTED, hook_addJobSelected,
			&game::g_hookOrig.orig_addJobSelected, installed, true) == NULL)
		jobOk = true;
	else
	{
		game::g_hookOrig.orig_addJobSelected = NULL;
		LogError("FAILED to hook PlayerInterface::addJobSelectedCharacters (island cancel)");
	}

	if (HookInstall(HOOK_ADD_TASK_NEAREST, hook_addTaskNearest,
			&game::g_hookOrig.orig_addTaskNearest, installed, true) == NULL)
		taskOk = true;
	else
	{
		game::g_hookOrig.orig_addTaskNearest = NULL;
		LogError("FAILED to hook PlayerInterface::addTaskNearestSelectedCharacter (island cancel)");
	}

	IslandSetCancelHooksInstalled(stopOk, jobOk, taskOk);

	std::ostringstream cs;
	cs << "Island cancel hooks: stop=" << (stopOk ? 1 : 0) << " job=" << (jobOk ? 1 : 0)
	   << " task=" << (taskOk ? 1 : 0);
	LogDebug(cs.str());
}

static void InstallCacheHook(int* installed, int*)
{
	if (HookRowWanted(HOOK_DISPATCH_JOB))
	{
		const char* cacheHookResult = HookInstall(HOOK_DISPATCH_JOB, hook_dispatchJob,
				&game::g_hookOrig.orig_dispatchJob, installed, false);
		if (cacheHookResult == NULL)
		{
			std::ostringstream ds;
			ds << "dispatchJob hook installed"
			   << " orig=" << (void*)game::g_hookOrig.orig_dispatchJob
			   << " detour=" << (void*)hook_dispatchJob;
			LogMsg(ds.str());
		}
		else
		{
			LogError("FAILED to hook dispatchJob");
			navmesh::g_navmeshCfg.cachingEnabled = false;
		}

		// The populate hook is no longer installed here. It
		// installs lazily beside the edgeProcess clone-guard on the first
		// dispatch (nm_lazy_hooks.cpp), where it captures each job's input
		// triangle count for the zero-face rule.
	}
}

// Group cohesion: binary-patch scatter branch
static void CohesionPatchStep(int*, int*)
{
	if (movement::g_movementCfg.groupCohesionEnabled && game::g_hookOrig.orig_addOrderSelected)
	{
		if (!ApplyScatterPatch())
		{
			LogMsg("Scatter patch failed, cohesion disabled");
			movement::g_movementCfg.groupCohesionEnabled = false;
		}
	}
	else if (movement::g_movementCfg.groupCohesionEnabled)
	{
		LogMsg("addOrderSelected hook required for cohesion");
		movement::g_movementCfg.groupCohesionEnabled = false;
	}
}

static void ClearFormationStep(int*, int*)
{
	ClearFormationGroups();
}

// Pathfinding diagnostics (4 hooks: 3 bg thread + requestPath). findPathFull also installs on
// its own when only the hierarchical arm wants it: then csFindPath is not hooked, so the
// search's queue label reads -1 and the player budget boost never arms.
static void InstallPathfindHooks(int* installed, int*)
{
	const bool diag = HookRowWanted(HOOK_CS_FIND_PATH);
	const bool full = HookRowWanted(HOOK_FIND_PATH_FULL);
	int diagInstalled = 0;

	if (diag)
	{
		if (HookInstall(HOOK_CS_FIND_PATH, hook_csFindPath,
				&game::g_hookOrig.orig_csFindPath, installed, false) == NULL)
			diagInstalled++;
		else
			LogError("FAILED to hook ContentStream::findPath");

		if (HookInstall(HOOK_CS_CHECK_FACE_CONN, hook_csCheckFaceConn,
				&game::g_hookOrig.orig_csCheckFaceConn, installed, false) == NULL)
			diagInstalled++;
		else
			LogError("FAILED to hook checkFaceConnectivity");
	}

	if (full)
	{
		if (HookInstall(HOOK_FIND_PATH_FULL, hook_findPathFull,
				&game::g_hookOrig.orig_findPathFull, installed, false) == NULL)
			{ if (diag) diagInstalled++; }
		else
		{
			LogError("FAILED to hook findPathFull");
			// The heuristic guards, installed by an earlier step, stay in: each calls its site
			// unchanged while the instances are in place, and NPC searches reach the same sites.
			if (pathfind::g_pathfindCfg.playerHierarchicalMode != AHIER_OFF)
			{
				pathfind::g_pathfindCfg.playerHierarchicalMode = AHIER_OFF;
				LogError("FAILED to hook findPathFull; playerHierarchical is off for this session");
			}
		}
	}

	if (diag)
	{
		if (HookInstall(HOOK_REQUEST_PATH, hook_requestPath,
				&game::g_hookOrig.orig_requestPath, installed, false) == NULL)
			diagInstalled++;
		else
			LogError("FAILED to hook requestPath");

		if (diagInstalled < 4)
			pathfind::g_pathfindCfg.pathfindDiagEnabled = false;

		LogMsg("Pathfinding step 1: " +
		       std::string(diagInstalled == 4 ? "all 4 hooks installed" : "PARTIAL install"));
	}
	else if (full)
	{
		LogMsg(std::string("Pathfinding step 1: findPathFull alone for playerHierarchical=")
		       + AstarHierModeName(pathfind::g_pathfindCfg.playerHierarchicalMode));
	}
}

// Player searches reach the heuristic's three sites only through the hierarchical arm, so the
// arm runs only over a complete guard: a partial or refused install turns it off here, before
// findPathFull's want is read.
static void HierarchicalGuardStep(int*, int*)
{
	if (pathfind::g_pathfindCfg.playerHierarchicalMode == AHIER_OFF || GraphHeuristicGuardComplete())
		return;
	pathfind::g_pathfindCfg.playerHierarchicalMode = AHIER_OFF;
	LogError(std::string("Graph heuristic guard ") + GraphHeuristicGuardToken()
	         + "; playerHierarchical is off for this session");
}

// Priority boost (pathReqSubmit hook)
static void InstallSubmitHook(int* installed, int*)
{
	if (HookRowWanted(HOOK_PATH_REQ_SUBMIT))
	{
		if (HookInstall(HOOK_PATH_REQ_SUBMIT, hook_pathReqSubmit,
				&game::g_hookOrig.orig_pathReqSubmit, installed, false) == NULL)
		{
			LogMsg("Pathfinding step 2: submit hook installed");
		}
		else
			LogError("FAILED to hook PathRequestQueue::submit");
	}
}

// findPathFallback hook. Read-only: it counts its own site, carries
// the engine's own player/NPC verdict to the search that runs inside it.
// The verdict is the only authoritative requester label there is.
static void InstallFallbackHook(int* installed, int*)
{
	if (HookRowWanted(HOOK_CS_FIND_PATH_FALLBACK))
	{
		if (HookInstall(HOOK_CS_FIND_PATH_FALLBACK, hook_csFindPathFallback,
				&game::g_hookOrig.orig_csFindPathFallback, installed, false) == NULL)
		{
			LogMsg("Pathfinding step 4: fallback hook installed");
		}
		else
			LogError("FAILED to hook findPathFallback");
	}
}

// Path-result extraction guard and streaming-collection insert count. Each
// installs only when its own key is on, so the banner counts what was
// actually wanted.
// The insert hook has two consumers with separate keys, and either one on
// its own installs it: the lifecycle rows must survive a run made with the
// pathfinding diagnostics off, because comparing such a run against one
// with them on is what the rows are for.
static void InstallPathExtractHooks(int* installed, int*)
{
	if (HookRowWanted(HOOK_CONTENT_STREAM_CALLEE_0X8869))
	{
		if (HookInstall(HOOK_CONTENT_STREAM_CALLEE_0X8869, hook_contentStreamCallee0x8869,
				&game::g_hookOrig.orig_contentStreamCallee0x8869, installed, false) == NULL)
		{
			LogMsg("pathExtractGuard: extraction hook installed");
		}
		else
			LogError("FAILED to hook contentStreamCallee_0x8869");
	}

	if (HookRowWanted(HOOK_ADD_INSTANCE))
	{
		if (HookInstall(HOOK_ADD_INSTANCE, hook_addInstance,
				&game::g_hookOrig.orig_addInstance, installed, false) == NULL)
		{
			StitchSourceNoteAddObserver();
			LogMsg("sectionStamp: addInstance count hook installed");
		}
		else
			LogError("FAILED to hook hkaiStreamingCollection::addInstance");
	}
	else
		LogMsg("addInstance not hooked: sectionStamp and navMeshLife are both off, "
		       "so neither the insert count nor the lifecycle rows exist this session");
}

// Path-worker-pool pass-through instrumentation: four hooks, no
// behaviour change. contentStream,
// dequeueWork and enqueueThreadSafe install unconditionally here;
// gatesUpdateCodes is gated on its own INI key (gatePassDiag)
// since it exists purely to time gate-code passes. The want is read once,
// before either gate install, so a failed updateCodes still reports the
// findPath line.
static void InstallPathPoolHooks(int* installed, int*)
{
	bool gateHookWanted = HookRowWanted(HOOK_GATES_UPDATE_CODES);
	int poolInstalled = 0;
	int poolTotal = 3 + (gateHookWanted ? 2 : 0);   // + updateCodes and findPath

	if (HookInstall(HOOK_CONTENT_STREAM, hook_contentStream,
			&orig_contentStream, installed, true) == NULL)
		poolInstalled++;
	else
		LogError("FAILED to hook SectionManager::contentStream (PathPool)");

	if (HookInstall(HOOK_DEQUEUE_WORK, hook_dequeueWork,
			&orig_dequeueWork, installed, true) == NULL)
		poolInstalled++;
	else
		LogError("FAILED to hook SectionManager::dequeueWork_threadSafe (PathPool)");

	if (HookInstall(HOOK_ENQUEUE_THREAD_SAFE, hook_enqueueThreadSafe,
			&orig_enqueueThreadSafe, installed, true) == NULL)
		poolInstalled++;
	else
		LogError("FAILED to hook PathRequestQueue::enqueue_threadSafe (PathPool)");

	if (gateHookWanted)
	{
		if (HookInstall(HOOK_GATES_UPDATE_CODES, hook_gatesUpdateCodes,
				&orig_gatesUpdateCodes, installed, true) == NULL)
			poolInstalled++;
		else
		{
			LogError("FAILED to hook Gates__updateCodes (PathPool)");
			pathfind::g_pathfindCfg.gatePassDiagEnabled = false;
		}
		if (orig_gatesUpdateCodes
		    && HookInstall(HOOK_GATES_FIND_PATH, hook_gatesFindPath,
				&orig_gatesFindPath, installed, true) == NULL)
			poolInstalled++;
		else
			LogError("FAILED to hook Gates__findPath (GatePass)");
	}

	std::ostringstream ps;
	ps << "PathPool Step 1: " << poolInstalled << "/" << poolTotal
	   << " hooks installed npcWaitDiag=" << (pathfind::g_pathfindCfg.npcWaitDiagEnabled ? "on" : "off")
	   << " gatePassDiag=" << (pathfind::g_pathfindCfg.gatePassDiagEnabled ? "on" : "off");
	LogMsg(ps.str());
}


// =========================================================================
// InstallHooks
// =========================================================================

// Every startup install, and the steps between them that depend on one, in
// the order they run. The log's install lines follow this order.
static void (*const kInstallSteps[])(int*, int*) =
{
	InstallLoadingHooks,
	InstallDestroyListHook,
	InstallResetUnloadHook,

	// Present in every build: the fault it guards is measured, and with no
	// out-of-bounds record it calls the game's own walk unchanged.
	InstallUnstitchGuard,

	// Present in every build and whatever the guard's key says: a pass-through
	// on the stitch that records what it wrote, so the guard's drops can be
	// traced to their writer. stitchSourceLines sets verbosity only.
	InstallStitchSource,

	// Present in every build too, and for the same reason: the search fault it
	// prevents was recorded twice mid-session.
	InstallGraphVisitorGuard,

	// And its sibling: the node the guard above keeps in the search is
	// expanded through the cache slot beside the one it tests.
	InstallGraphExpandGuard,

	// A third copy of the same cache shape, on the cluster-graph search's own
	// node-position helper -- reached only when clusterGraphBypass actually
	// consults the graph, a call path neither guard above sits on.
	InstallGraphPositionGuard,

	// The hierarchical heuristic's three unchecked instance reads, only while wanted.
	InstallGraphHeuristicGuard,
	HierarchicalGuardStep,

	// The cross-tile cluster link cost, rewritten as each graph instance registers, only while wanted.
	InstallClusterCrossCost,

	// And again: three records at one instruction, each with a valid key and a
	// valid slot, faulting one dereference below them.
	InstallMeshFaceGuard,

	// The add-list self-duplicate: createInstance frees a NavInstance that is
	// already queued and queues the freed pointer, and the drain reads it.
	InstallCreateInstanceGuard,

	// The hull destroy queue: an object queued twice is deleted twice by the
	// physics thread, the second time through freed memory.
	InstallHullQueueGuard,

	// The click-hull same-target skip swaps a verified slot for its live setting in every build.
	InstallHullSameSkip,

	// The OgreMain scene switches install for their live settings in every build; no manifest row.
	InstallSceneForkSkip,
	InstallInstEmptySkip,

	// The D3D11 state-object switch: installed whatever its
	// live key says; no manifest row.
	InstallD3dStateSkip,

	// The drain end of the navmesh adjacency window, whatever the key says:
	// the key chooses deferral or counting, never whether the window is seen.
	InstallNavMeshAdjacency,

	// The retire half of the section lifecycle rows. The registration half
	// rides the streaming-collection insert hook below, which this key alone
	// is enough to install.
	InstallNavMeshLife,

	// The Ogre join spin installs for its live setting; its module site is outside this table.
	InstallOgreJoinSpin,

#ifdef KEO_DEBUG
	// Read-only diagnostic, off unless unstitchProbe is set; the entry itself
	// does not exist in a PROD build.
	InstallUnstitchProbe,
	// Read-only too, and on unless sectionKeyProbe is cleared: what it records
	// is only useful if it was already running when the fault arrived.
	InstallSectionKeyProbe,
	// Pass-through counter, no INI key: a concurrency reading is only worth
	// anything if it was running for the whole session, and the one control
	// that matters (preload on versus off) is another key entirely.
	InstallPhysXPoolProbe,
	// Pass-through counter of player haul sizes; DEV only, no INI key.
	keo_inventory::InstallJobCounters,
#endif

	InstallZonePauseGuard,
	// The far visibility-check stagger: installed whatever its
	// live key says.
	InstallOnScreenStagger,
	// The paused off-screen skip: installed whatever its live key
	// says.
	InstallPausedSkip,
	InstallZoneLifecycleHooks,
	InstallCorpsePin,
	keo_inventory::InstallBackpackFirst,
	fixes::InstallTownClaim,
	keo_inventory::InstallBackpackSidecar,
	keo_inventory::InstallBackpackWindow,
	InstallFormationPace,
	// The faction relations lookup is installed for its live setting in every build.
	InstallFactionRelations,
	fixes::InstallThrowout,
#if ZONEHAND_STEP >= 2
	InstallNestValidationGuard,
#endif
	// The ground-food score sees the worn backpack, while backpackFoodScore is on.
	keo_inventory::InstallBackpackFood,

	// A finished or repaired wall's navmesh splice, deferred until physics has moved its hulls; only while wanted.
	navmesh::InstallWallSplice,
	// A box across a cell border regenerates its cells in full; after the wall splice, whose install
	// compares the same entry's unpatched head. Only while wanted.
	navmesh::InstallSpliceSeam,
	// Machine operators fill main inventory and backpack before delivering; only while wanted.
	keo_inventory::InstallOperatorTrips,
	InstallOrderHook,
	OrderAbortStep,
	InstallIslandHooks,
	InstallCancelHooks,
	InstallCacheHook,
	// The rebuild-navmesh key's detour, while caching is wanted; a refusal leaves the key as the game has it.
	InstallNavMeshRebuildKey,
	CohesionPatchStep,
	ClearFormationStep,
	InstallPathfindHooks,
	InstallSubmitHook,
	InstallFallbackHook,
	InstallPathExtractHooks,
	InstallPathPoolHooks,

	// The route planner's two detours, only while plannerMode is set; a refusal clears it before the store is made.
	planner::InstallPlannerHooks,
	// The route planner's store and whole-map base, only while plannerMode is set. No hook.
	planner::PlannerBaseStartStep,
};

void InstallHooks(int* installed, int* total)
{
	*installed = 0;
	*total = 0;

	// The total is the wanted set as the config stands before any install, so
	// a failed install lowers *installed and never the total, even where the
	// failure clears a key a later want reads.
	const HookWantInputs wantedAtStart = HookWantInputsNow();
	for (int i = 0; i < HOOK_ROW_COUNT; ++i)
	{
		if (HookWantEval(s_wants[i], wantedAtStart))
			++*total;
	}

	for (size_t i = 0; i < sizeof(kInstallSteps) / sizeof(kInstallSteps[0]); ++i)
		kInstallSteps[i](installed, total);
}
