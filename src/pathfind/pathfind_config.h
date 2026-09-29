// pathfind_config.h - the pathfind module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"

// clusterGraphBypass: what checkFaceConnectivity
// answers. Four positions, mutually exclusive, so there is no combination to
// get wrong:
//
//   CGB_BYPASS (true, the default) -- answer 1 without consulting the cluster
//     graph. The graph is stale and rejects reachable pairs, and walking it
//     reads a navmesh section slot that can be replaced underneath it.
//   CGB_ORIGINAL (false) -- call the original and obey it, so a pair the graph
//     rejects never reaches the A* search.
//   CGB_MEASURE ("measure") -- call the original, count its answer, and wave
//     the pair through anyway. Routing behaves as under CGB_BYPASS while each
//     rejected pair's search is labelled and its outcome recorded, so what the
//     bypass lets through is read inside one run instead of from a pair of
//     runs. It pays CGB_ORIGINAL's read cost: diagnostic, not a default.
//   CGB_PLAYER ("player") -- call the original, wave a rejected pair through
//     only when the request being served is player-owned, and obey the
//     rejection otherwise. The requester is read from the request the
//     direct-path check stashed for this thread a few instructions earlier;
//     a check with no request behind it -- the AI task system's reachability
//     query, which never runs that check -- counts as unattributed and is
//     never waved, so the unknown case keeps vanilla behaviour.
//
// CGB_ORIGINAL, CGB_MEASURE and CGB_PLAYER all consult the cluster graph, and
// so all three walk the section slot that CGB_BYPASS exists to avoid reading.
// graphPositionGuard covers the absent-instance fault on that walk; the two
// A* absent-instance guards sit on different sites and do not.
//
// Read on every connectivity check, from the contentStream and AI back threads.
enum ClusterGraphMode
{
	CGB_BYPASS = 0,
	CGB_ORIGINAL,
	CGB_MEASURE,
	CGB_PLAYER
};

namespace pathfind {

// Starts as a copy of kPathfindDefaults, then written by LoadConfig on the
// main thread before any hook installs. Two main-thread writers later only
// clear flags: CheckBuildGate (plugin_entry.cpp), when the build gate fails
// and no hook installs, clears pathfindDiagEnabled; InstallHooks
// (hook_manifest.cpp), with earlier hooks already live, clears
// pathfindDiagEnabled when any of its four pathfinding hooks fails and
// gatePassDiagEnabled when the Gates__updateCodes hook fails. Read on any
// thread; a hook running during that clear reads the old or the new value
// of one bool.
struct PathfindConfig
{
	bool pathfindDiagEnabled;

	// npcWaitDiag: NPC path-wait diagnostic. DEV default on, PROD default off;
	// read by path_pool.cpp.
	bool npcWaitDiagEnabled;

	// gatePassDiag: per-pass gate-code timing diagnostic. DEV default on, PROD
	// default off; decides the Gates__updateCodes install (hook_manifest.cpp) and the
	// GateRate: line (path_pool_report.cpp).
	bool gatePassDiagEnabled;

	// pathCostLines: the hook_findPathFull record and its histograms/cap
	// counters accumulate whenever findPathFull is hooked
	// (pathfindDiagEnabled); this key controls only how much of that gets
	// printed on the heartbeat -- the compact AstarCap:/PathBusy: lines always
	// print, this key adds the per-class AstarClass: detail lines. On by
	// default (astar_cost.cpp).
	bool pathCostLinesEnabled;

	int clusterGraphBypassMode;

	// playerRepathTier: the game's own mid-walk re-request for a player character
	// calls requestPath at priority 0, the same as an NPC's, so it queues at the
	// NPC tier (10) instead of the mod's player tier (45) that its original order
	// got. The publish-and-match in player_repath_tier.h runs and counts either
	// way; this key chooses only whether a match's tier is actually written
	// (true, the default) or just counted (false, observe; playerRepath= in the
	// Phase12 heartbeat carries seen/would/tiered/set every time). Read on every
	// match.
	bool playerRepathTierEnabled;
};

extern PathfindConfig g_pathfindCfg;
extern const PathfindConfig kPathfindDefaults;
extern const ConfigKey g_pathfindConfigKeys[];

} // namespace pathfind
