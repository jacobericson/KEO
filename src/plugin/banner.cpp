// banner.cpp - Startup banner and KenshiLib binding line.
// Main thread; the banner follows InstallHooks and precedes the three guard
// installs, or records a build-gate refusal. Logging uses the core log lock.

#include "plugin/plugin_entry_internal.h"
#include "base/config.h"
#include "movement/island_edge_ring.h"
#include "render/render_config.h"
#include "gui/settings_panel.h"
#include "bench/bench_runner.h"
#include "fixes/search/graph_heuristic_guard.h"
#include "fixes/search/cluster_cross_cost.h"
#include "pathfind/astar_hier_policy.h"
#include "planner/planner_hooks.h"

namespace plugin_entry_detail
{
// Emitted on both paths: after a normal install, and after a refused one, so a
// player's log always says which it was. gateTok is "ok(<n> sites, <k> shared)"
// or "FAILED"; <k> counts sites another plugin had already hooked.
// renderInstalled/renderWanted count the render levers; the token reads
// "render=off" when the master switch is off. gui= is the settings tab,
// benchTok the benchmark ("ok" or "off(<reason>)").

void LogInitBanner(int installed, int totalHooks, const std::string& gateTok,
                          int renderInstalled, int renderWanted, const std::string& benchTok)
{
	std::ostringstream renderTok;
	if (g_renderCfg.renderLevers)
		renderTok << renderInstalled << "/" << renderWanted;
	else
		renderTok << "off";

	std::ostringstream msg;
	msg << "Initialized - " << installed << "/" << totalHooks << " hooks installed"
	    << ", deferral=" << (zone::g_zoneCfg.deferralEnabled ? "ON" : "OFF")
	    << ", priorityBoost=" << (navmesh::g_navmeshCfg.priorityBoostEnabled ? "ON" : "OFF")
	    << ", preload=" << (zone::g_zoneCfg.preloadEnabled ? "ON" : "OFF")
	    << ", movementAware=" << (zone::g_zoneCfg.movementAwareEnabled ? "ON" : "OFF")
	    << ", caching=" << (navmesh::g_navmeshCfg.cachingEnabled ? "ON" : "OFF")
	    << ", cohesion=" << (movement::g_movementCfg.groupCohesionEnabled ? "ON" : "OFF")
	    << ", pathDiag=" << (pathfind::g_pathfindCfg.pathfindDiagEnabled ? "ON" : "OFF")
	    << ", pathStep=" << 4
	    << ", routeTier=" << (navmesh::g_navmeshCfg.routeTierEnabled ? "ON" : "OFF")
	    << ", clusterGraphBypass=" << (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_BYPASS ? "ON"
	        : (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_MEASURE ? "MEASURE"
	        : (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_PLAYER ? "PLAYER" : "OFF")))
	    << ", playerHier=" << AstarHierModeName(pathfind::g_pathfindCfg.playerHierarchicalMode)
	    << "/" << AstarHierOnCapName(pathfind::g_pathfindCfg.playerHierOnCapMode)
	    << ", unstitchGuard=" << (fixes::g_fixesCfg.unstitchGuardEnabled ? "ON" : "OFF")
	    << ", stitchSourceLines=" << fixes::g_fixesCfg.cfg_stitchSourceLines
	    << ", graphVisitorGuard=" << (fixes::g_fixesCfg.graphVisitorGuardEnabled ? "ON" : "OFF")
	    << ", graphExpandGuard=" << (fixes::g_fixesCfg.graphExpandGuardEnabled ? "ON" : "OFF")
	    << ", graphPositionGuard=" << (fixes::g_fixesCfg.graphPositionGuardEnabled ? "ON" : "OFF")
	    << ", graphHeuristicGuard=" << GraphHeuristicGuardToken()
	    << ", clusterCrossCost=" << ClusterCrossCostToken()
	    << ", createInstanceGuard=" << (fixes::g_fixesCfg.createInstanceGuardEnabled ? "ON" : "OFF")
	    << ", hullDoublePushGuard=" << (fixes::g_fixesCfg.hullDoublePushGuardEnabled ? "ON" : "OFF")
	    << ", stitchByteGuard=" << (fixes::g_fixesCfg.stitchByteGuardEnabled ? "ON" : "OFF")
#ifdef ZONEOPT_DEBUG
	    << ", unstitchProbe=" << (fixes::g_fixesCfg.unstitchProbeEnabled ? "ON" : "OFF")
#endif
	    << ", islandFix=" << (movement::g_movementCfg.islandFixEnabled ? "ON" : "OFF")
	    << ", islandFarSpan=" << movement::g_movementCfg.cfg_islandFarSpan
	    << ", islandEdgeRing=" << IslandEdgeRingModeStr()
	    << ", planner=" << planner::PlannerBannerToken()
	    // The *Step= tokens are fixed values, kept for the line's format.
	    << ", islandStep=" << 4
	    << ", preloadStep=" << 1
	    << ", pathpoolStep=" << 1
	    << ", zonelifeStep=" << 3
	    << ", nmfixStep=" << 9
	    // Fresh navmesh work buffers get the game's region pruning and
	    // extra-vertex settings; off = Havok's defaults.
	    << ", nmPrune=" << 2 << "/"
	    << (navmesh::g_navmeshCfg.navmeshVanillaPruningEnabled ? "on" : "off")
	    // Neighbour-seed instrumentation and the shipped-tile stand-in seeds;
	    // off = the INI key is false.
	    << ", nmNbrSeed=" << 2 << "/"
	    << (navmesh::g_navmeshCfg.navmeshNeighbourSeedsEnabled ? "on" : "off")
	    // Adjacent navmesh jobs are deferred unless the key is false (count only).
	    << ", nmAdj=" << 2 << "/"
	    << (navmesh::g_navmeshCfg.navmeshAdjExclusionEnabled ? "enforce" : "count");
	msg << ", render=" << renderTok.str();
	msg << ", gui=" << SettingsPanelToken();
	msg << ", bench=" << benchTok;
	msg << ", klibStep=" << 5
	    << ", klibMembers=" << 5
	;
	msg << ", gate=" << gateTok;
	// The only knob a folded build still varies. Appended last so grepping
	// for an earlier token's fixed prefix is unaffected.
	msg << ", zonehand=" << ZONEHAND_STEP;
	BenchRunnerSetBanner("render=" + renderTok.str() + " gate=" + gateTok + " bench=" + benchTok);
	DebugLog(msg.str());
	LogMsg(msg.str());
}


// =========================================================================
// Plugin entry point
// =========================================================================

void LogKlibBinding(const char* message)
{
	LogMsg(message);
}

}
using namespace plugin_entry_detail;
