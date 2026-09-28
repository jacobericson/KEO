// movement_config.cpp - defaults and INI rows for the movement module.
#include "movement/movement_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

static bool ParseIslandEdgeRing(const std::string& val, ConfigLogFn log)
{
	bool b;
	bool isObserve = _stricmp(val.c_str(), "observe") == 0;
	bool isOff = _stricmp(val.c_str(), "off") == 0;
	bool isOn = _stricmp(val.c_str(), "on") == 0;
	if (isObserve || isOff || isOn) {
		b = isOn;
		log("Config: islandEdgeRing=" + val + " is a retired value; read as " + (b ? "true" : "false"));
		movement::g_movementCfg.islandEdgeRingEnabled = b;
		return true;
	}
	if (ParseBool(val, &b)) { movement::g_movementCfg.islandEdgeRingEnabled = b; return true; }
	return false;
}

static bool ParseK7PostDeathHold(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (ParseBoolOr(val, "observe", &b, &third)) {
		movement::g_movementCfg.cfg_k7PostDeathHold = third ? K7_HOLD_OBSERVE : (b ? K7_HOLD_ON : K7_HOLD_OFF);
		return true;
	}
	return false;
}

static const ConfigChoice kIslandEdgeRingChoices[] =
{
	{ "false", 0, "false" }, { "true", 1, "true" }
};

static const ConfigChoice kK7HoldChoices[] =
{
	{ "false", K7_HOLD_OFF, "off" }, { "observe", K7_HOLD_OBSERVE, "observe" }, { "true", K7_HOLD_ON, "on" }
};

namespace movement {

const MovementConfig kMovementDefaults =
{
	true, // groupCohesionEnabled
	true, // islandFixEnabled
	2, // cfg_islandFarSpan
	false, // islandEdgeRingEnabled
	true, // playerCharRegistryEnabled
	true, // islandDeletedReissueEnabled
	K7_HOLD_ON, // cfg_k7PostDeathHold
	true, // k7DestReadyGateEnabled
	true, // k7ArrivalTriggerEnabled
};

MovementConfig g_movementCfg = kMovementDefaults;
} // namespace movement

namespace movement_config_detail {
union MovementConfigPodCheck { movement::MovementConfig s; };
} // namespace movement_config_detail
using namespace movement_config_detail;

namespace movement {

const ConfigKey g_movementConfigKeys[] =
{
	CFG_OBOOL("groupCohesion", MovementConfig, groupCohesionEnabled,         NDOC, SHOW,
	  "Squad cohesion",
	  "Keeps a squad given one move order walking together instead of scattering."),
	CFG_OBOOL("islandFix", MovementConfig, islandFixEnabled,             NDOC, SHOW,
	  "Island routing overlay",
	  "Lets the mod's island overlay answer the engine's island checks."),
	CFG_OINT("islandFarSpan", MovementConfig, cfg_islandFarSpan, 0.0f, 8.0f, INT_MIN, DOC, SHOW,
	  "Far order island span in cells",
	  "Treats two cells this many or more apart as different islands, so a far move order walks leg by"
	  " leg instead of one path that can end short. 0 keeps the game's answer."),
	CFG_OCUSTOM_CHOICES("islandEdgeRing", MovementConfig, islandEdgeRingEnabled, ParseIslandEdgeRing, DOC,
	  "Edge route leg ring",
	  "Narrows the island list the engine's edge-route leg finder sees to the cells around the"
	  " character, so a leg is at most about two cells. false only counts what it would remove.", kIslandEdgeRingChoices),
	CFG_OBOOL("playerCharRegistry", MovementConfig, playerCharRegistryEnabled,    DOC, SHOW,
	  "Watch every player character",
	  "Watches every player-faction character, not only those with a move order, so the zone each"
	  " stands in keeps its place in the navmesh queue and in what loads first."),
	CFG_OBOOL("islandDeletedReissue", MovementConfig, islandDeletedReissueEnabled,  NDOC, SHOW,
	  "Re-issue deleted move orders",
	  "Re-issues a player move order the engine deleted, never one the player cancelled."),
	CFG_OCUSTOM_CHOICES("k7PostDeathHold", MovementConfig, cfg_k7PostDeathHold, ParseK7PostDeathHold, DOC,
	  "Hold move orders through combat",
	  "Holds a player move order that ended as combat took over, and re-issues it once the fight ends,"
	  " up to 60 seconds. observe only logs what would be held.", kK7HoldChoices),
	CFG_OBOOL("k7DestReadyGate", MovementConfig, k7DestReadyGateEnabled,       DOC, SHOW,
	  "Wait for the destination's navmesh",
	  "Before re-issuing a deleted order, waits up to 15 seconds for the destination cell's navmesh to"
	  " be in the world."),
	CFG_OBOOL("k7ArrivalTrigger", MovementConfig, k7ArrivalTriggerEnabled,      DOC, SHOW,
	  "Re-issue on navmesh arrival",
	  "When an order stops far short while its destination's navmesh is missing, re-issues it as soon"
	  " as that navmesh arrives. Off only logs when it would have sent."),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace movement
