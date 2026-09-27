// banner.cpp - Startup banner and KenshiLib binding line.
// Main thread; the banner follows InstallHooks and precedes the three guard
// installs, or records a build-gate refusal. Logging uses the core log lock.

#include "plugin/plugin_entry_internal.h"
#include "base/config.h"
#include "movement/island_edge_ring.h"
#include "render/render_config.h"
#include "gui/settings_panel.h"
#include "bench/bench_runner.h"

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
	    << ", deferral=" << (deferralEnabled ? "ON" : "OFF")
	    << ", priorityBoost=" << (priorityBoostEnabled ? "ON" : "OFF")
	    << ", preload=" << (preloadEnabled ? "ON" : "OFF")
	    << ", movementAware=" << (movementAwareEnabled ? "ON" : "OFF")
	    << ", caching=" << (cachingEnabled ? "ON" : "OFF")
	    << ", cohesion=" << (groupCohesionEnabled ? "ON" : "OFF")
	    << ", pathDiag=" << (pathfindDiagEnabled ? "ON" : "OFF")
	    << ", pathStep=" << 4
	    << ", routeTier=" << (routeTierEnabled ? "ON" : "OFF")
	    << ", clusterGraphBypass=" << (clusterGraphBypassMode == CGB_BYPASS ? "ON"
	        : (clusterGraphBypassMode == CGB_MEASURE ? "MEASURE"
	        : (clusterGraphBypassMode == CGB_PLAYER ? "PLAYER" : "OFF")))
	    << ", unstitchGuard=" << (unstitchGuardEnabled ? "ON" : "OFF")
	    << ", stitchSourceLines=" << cfg_stitchSourceLines
	    << ", graphVisitorGuard=" << (graphVisitorGuardEnabled ? "ON" : "OFF")
	    << ", graphExpandGuard=" << (graphExpandGuardEnabled ? "ON" : "OFF")
	    << ", graphPositionGuard=" << (graphPositionGuardEnabled ? "ON" : "OFF")
	    << ", createInstanceGuard=" << (createInstanceGuardEnabled ? "ON" : "OFF")
	    << ", hullDoublePushGuard=" << (hullDoublePushGuardEnabled ? "ON" : "OFF")
	    << ", stitchByteGuard=" << (stitchByteGuardEnabled ? "ON" : "OFF")
#ifdef ZONEOPT_DEBUG
	    << ", unstitchProbe=" << (unstitchProbeEnabled ? "ON" : "OFF")
#endif
	    << ", islandFix=" << (islandFixEnabled ? "ON" : "OFF")
	    << ", islandFarSpan=" << cfg_islandFarSpan
	    << ", islandEdgeRing=" << IslandEdgeRingModeStr()
	    // The *Step= tokens are fixed values, kept for the line's format.
	    << ", islandStep=" << 4
	    << ", preloadStep=" << 1
	    << ", pathpoolStep=" << 1
	    << ", zonelifeStep=" << 3
	    << ", nmfixStep=" << 9
	    // Fresh navmesh work buffers get the game's region pruning and
	    // extra-vertex settings; off = Havok's defaults.
	    << ", nmPrune=" << 2 << "/"
	    << (navmeshVanillaPruningEnabled ? "on" : "off")
	    // Neighbour-seed instrumentation and the shipped-tile stand-in seeds;
	    // off = the INI key is false.
	    << ", nmNbrSeed=" << 2 << "/"
	    << (navmeshNeighbourSeedsEnabled ? "on" : "off")
	    // Adjacent navmesh jobs are deferred unless the key is false (count only).
	    << ", nmAdj=" << 2 << "/"
	    << (navmeshAdjExclusionEnabled ? "enforce" : "count");
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
