// pathfind_config.cpp - defaults and INI rows for the pathfind module.
#include "pathfind/pathfind_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

static bool ParseClusterGraphBypass(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (ParseBoolOr(val, "player", &b, &third) && third) {
		pathfind::g_pathfindCfg.clusterGraphBypassMode = CGB_PLAYER;
		return true;
	}
	if (ParseBoolOr(val, "measure", &b, &third)) {
		pathfind::g_pathfindCfg.clusterGraphBypassMode = third ? CGB_MEASURE : (b ? CGB_BYPASS : CGB_ORIGINAL);
		return true;
	}
	return false;
}

// playerHierarchical: off, observe or on; anything else is refused.
static bool ParsePlayerHierarchical(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int mode;
	if (_stricmp(val.c_str(), "off") == 0)
		mode = AHIER_OFF;
	else if (_stricmp(val.c_str(), "observe") == 0)
		mode = AHIER_OBSERVE;
	else if (_stricmp(val.c_str(), "on") == 0)
		mode = AHIER_ON;
	else
		return false;
	pathfind::g_pathfindCfg.playerHierarchicalMode = mode;
	return true;
}

// playerHierOnCap: rerun or keep; anything else is refused.
static bool ParsePlayerHierOnCap(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int onCap;
	if (_stricmp(val.c_str(), "rerun") == 0)
		onCap = AHIER_CAP_RERUN;
	else if (_stricmp(val.c_str(), "keep") == 0)
		onCap = AHIER_CAP_KEEP;
	else
		return false;
	pathfind::g_pathfindCfg.playerHierOnCapMode = onCap;
	return true;
}

static const ConfigChoice kClusterGraphChoices[] =
{
	{ "false", CGB_ORIGINAL, "false" }, { "true", CGB_BYPASS, "true" }, { "player", CGB_PLAYER, "player" },
	{ "measure", CGB_MEASURE, "measure" }
};

namespace pathfind {

const PathfindConfig kPathfindDefaults =
{
	true, // pathfindDiagEnabled
#ifdef ZONEOPT_DEBUG
	true, // npcWaitDiagEnabled
#else
	false, // npcWaitDiagEnabled
#endif
#ifdef ZONEOPT_DEBUG
	true, // gatePassDiagEnabled
#else
	false, // gatePassDiagEnabled
#endif
	true, // pathCostLinesEnabled
	CGB_BYPASS, // clusterGraphBypassMode
	true, // playerRepathTierEnabled
	AHIER_OFF, // playerHierarchicalMode
	AHIER_CAP_RERUN, // playerHierOnCapMode
};

PathfindConfig g_pathfindCfg = kPathfindDefaults;
} // namespace pathfind

namespace pathfind_config_detail {
union PathfindConfigPodCheck { pathfind::PathfindConfig s; };
} // namespace pathfind_config_detail
using namespace pathfind_config_detail;

namespace pathfind {

const ConfigKey g_pathfindConfigKeys[] =
{
	CFG_OBOOL("pathfindDiag", PathfindConfig, pathfindDiagEnabled,          NDOC, SHOW,
	  "Pathfinding hooks",
	  "Installs the mod's pathfinding hooks. The cluster graph bypass, the path request priority tiers,"
	  " the longer A* search for player orders, the path extraction guard and the path log lines all run"
	  " through them; off turns every one of them off."),
	CFG_OBOOL("npcWaitDiag", PathfindConfig, npcWaitDiagEnabled,           NDOC, DIAG,
	  "NPC path wait diagnostic",
	  "Measures how long NPC path requests wait, for the log."),
	CFG_OBOOL("gatePassDiag", PathfindConfig, gatePassDiagEnabled,          NDOC, DIAG,
	  "Gate code pass timing",
	  "Times each gate-code pass for the GateRate: log line."),
	CFG_OBOOL("pathCostLines", PathfindConfig, pathCostLinesEnabled,         DOC, DIAG,
	  "Path search class lines",
	  "Prints the heartbeat's per-class AstarClass: detail lines; the compact AstarCap: and PathBusy:"
	  " lines print either way."),
	CFG_OCUSTOM_CHOICES("clusterGraphBypass", PathfindConfig, clusterGraphBypassMode, ParseClusterGraphBypass, DOC,
	  "Cluster graph pre-check",
	  "true answers the engine's connectivity pre-check as connected without reading the stale cluster"
	  " graph; false hands the check back to the game. measure and player ask the graph, then wave every"
	  " pair, or a player's pairs, through.", kClusterGraphChoices),
	CFG_OBOOL("playerRepathTier", PathfindConfig, playerRepathTierEnabled,      DOC, SHOW,
	  "Player path re-request priority",
	  "A player character's mid-walk path re-request keeps the priority its move order got instead of"
	  " queueing behind every NPC. Off only counts."),
	CFG_OCUSTOM("playerHierarchical", PathfindConfig, playerHierarchicalMode, ParsePlayerHierarchical, NDOC),
	CFG_OCUSTOM("playerHierOnCap", PathfindConfig, playerHierOnCapMode, ParsePlayerHierOnCap, NDOC),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace pathfind
