// zone_config.cpp - defaults and INI rows for the zone module.
#include "zone/zone_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

static bool ParseZoneGeometryMode(const std::string& val, ConfigLogFn log)
{
	ZoneGeometryMode m;
	if (ZoneGeometryModeFromName(val.c_str(), &m))
		zone::g_zoneCfg.zoneGeometryMode = m;
	else
		log("Config: zoneGeometryMode '" + val + "' refused; only contentOnly is available");
	return true;
}

namespace zone {

const ZoneConfig kZoneDefaults =
{
	true, // deferralEnabled
	true, // preloadEnabled
	true, // movementAwareEnabled
	true, // saveLoadUnloadEnabled
	true, // escapePauseGuardEnabled
	true, // townGuardEnabled
	true, // zoneRetentionEnabled
	false, // islandReadinessRuleEnabled
	true, // readinessOverridesEnabled
	true, // zoneLifeUnloadEnabled
#ifdef KEO_DEBUG
	true, // zoneCycleStatsEnabled
#else
	false, // zoneCycleStatsEnabled
#endif
	true, // zoneWedgeGuardEnabled
	ZONE_GEOMETRY_CONTENT_ONLY, // zoneGeometryMode
	true, // cfg_camFocusEnabled
	0.0f, // cfg_preloadKeepAliveSeconds
	10.0, // cfg_camLogInterval
	0.0f, // cfg_camFocusMaxDist
	3.0f, // cfg_camFocusHardMult
	250.0f, // cfg_camFocusHysteresis
	1, // cfg_zoneLifeRetainRadius
	1, // cfg_zoneLifeSquadRadius
	45, // cfg_zoneRetentionMaxHeld
	30.0, // cfg_zoneLifeIdleSeconds
};

ZoneConfig g_zoneCfg = kZoneDefaults;
} // namespace zone

namespace zone_config_detail {
union ZoneConfigPodCheck { zone::ZoneConfig s; };
} // namespace zone_config_detail
using namespace zone_config_detail;

namespace zone {
static_assert(__alignof(ZoneConfig) >= 8, "ZoneConfig must be 8-byte aligned");

const ConfigKey g_zoneConfigKeys[] =
{
	CFG_OBOOL("deferral", ZoneConfig, deferralEnabled,              NDOC, DEVROW,
	  "Navmesh readiness deferral",
	  "Lets a zone count as ready while its navmesh work is still pending, once its content sections"
	  " have drained."),
	CFG_OBOOL("preload", ZoneConfig, preloadEnabled,               NDOC, SHOW,
	  "Preload nearby zones",
	  "Loads the zones around the camera and the player's characters before they are needed."),
	CFG_OBOOL("movementAware", ZoneConfig, movementAwareEnabled,         NDOC, DEVROW,
	  "Movement-aware preloading",
	  "Watches player move orders and preloads the zones toward each destination."),
	CFG_OBOOL("saveLoadUnload", ZoneConfig, saveLoadUnloadEnabled,        NDOC, DEVROW,
	  "Unload mod zones at save load",
	  "At a save load's reset, unloads every zone the mod still holds and clears its state. Off only"
	  " counts them."),
	CFG_OBOOL("escapePauseGuard", ZoneConfig, escapePauseGuardEnabled,      DOC, DEVROW,
	  "Keep the escape menu's pause",
	  "Keeps the escape menu's pause when a zone load finishes behind it. Off also turns off the zone"
	  " cycle measurement and the wedge report."),
	CFG_OBOOL("townGuard", ZoneConfig, townGuardEnabled,             DOC, DEVROW,
	  "Town coverage guard",
	  "Refuses a town's coverage refresh that carries a nonpositive timer, the engine's own signal that"
	  " nothing in that coverage is leased."),
	CFG_OBOOL("zoneRetention", ZoneConfig, zoneRetentionEnabled,         DOC, DEVROW,
	  "Zone retention",
	  "Holds a cell the game has taken over from the mod past its countdowns, for as long as the"
	  " retention policy says."),
	CFG_OBOOL("islandReadinessRule", ZoneConfig, islandReadinessRuleEnabled,   NDOC, DEVROW,
	  "Per-caller readiness rule",
	  "Answers the content readiness check by the caller's class instead of one answer for every"
	  " caller."),
	CFG_OBOOL("readinessOverrides", ZoneConfig, readinessOverridesEnabled,    NDOC, DEVROW,
	  "Readiness deferral override",
	  "Lets the readiness deferral override the game's answer. Off leaves the game's own answer."),
	CFG_OBOOL("zoneLifeUnload", ZoneConfig, zoneLifeUnloadEnabled,        NDOC, DEVROW,
	  "Unload idle mod zones",
	  "Unloads the zones the mod loaded once they are idle and outside the camera's and the squads'"
	  " radii."),
	CFG_OBOOL("zoneCycleStats", ZoneConfig, zoneCycleStatsEnabled,        DOC, DEVROW,
	  "Zone cycle measurement",
	  "Measures each zone manager loading cycle, the player characters in cells the mod holds and Set"
	  " B's size, for the log. Reading only."),
	CFG_OBOOL("zoneWedgeGuard", ZoneConfig, zoneWedgeGuardEnabled,        DOC, DEVROW,
	  "Zone cycle wedge report",
	  "Reports once if a loading cycle stays in one phase for more than 10 seconds. Reading only; never"
	  " forces the phase forward."),
	CFG_OCUSTOM("zoneGeometryMode", ZoneConfig, zoneGeometryMode, ParseZoneGeometryMode, DOC),
	CFG_OBOOL("camFocus", ZoneConfig, cfg_camFocusEnabled,          DOC, DEVROW,
	  "Camera focus prediction",
	  "Predicts the camera's zone from where it points instead of where it sits, so a zoomed-out camera"
	  " still preloads the zone the squad walks into."),
	CFG_OFLOAT("preloadKeepAliveSeconds", ZoneConfig, cfg_preloadKeepAliveSeconds, 0.0f, 86400.0f, false, NDOC, DEVROW,
	  "Preloaded zone keep-alive seconds",
	  "How long a zone the mod preloads stays loaded before it may unload. 0 takes the game's own"
	  " default.", 0.0f, 0),
	CFG_ODOUBLE("camLogInterval", ZoneConfig, cfg_camLogInterval, 1.0f, 300.0f, NDOC, DEVROW,
	  "Camera log interval seconds",
	  "Seconds between debug camera log lines.", 1.0f, 0),
	CFG_OFLOAT("camFocusMaxDist", ZoneConfig, cfg_camFocusMaxDist, 500.0f, 50000.0f, true, DOC, DEVROW,
	  "Camera focus distance cap",
	  "World units from the nearest squad member past which the focus point is pulled back toward them."
	  " 0 is one zone width.", 0.0f, 0),
	CFG_OFLOAT("camFocusHardMult", ZoneConfig, cfg_camFocusHardMult, 1.0f, 10.0f, false, DOC, DEVROW,
	  "Camera focus hard cutoff multiple",
	  "Multiple of the distance cap past which the focus point is ignored outright.", 1.0f, 1),
	CFG_OFLOAT("camFocusHysteresis", ZoneConfig, cfg_camFocusHysteresis, 0.0f, 2000.0f, false, DOC, DEVROW,
	  "Camera focus hysteresis",
	  "World units past the preload threshold a neighbouring zone's prediction must move to take over"
	  " or be released, so a point near a border does not thrash.", 0.0f, 0),
	CFG_OINT_LIVE("zoneLifeRetainRadius", ZoneConfig, cfg_zoneLifeRetainRadius, 1.0f, 4.0f, INT_MIN, DOC, SHOW,
	  "Zones kept around the camera (vanilla 1)",
	  "Radius, in zones around the camera, inside which the zones the mod loaded stay loaded: 1 keeps a"
	  " 3x3, 2 a 5x5. The game itself keeps a 3x3 around the camera."),
	CFG_OINT_LIVE("zoneLifeSquadRadius", ZoneConfig, cfg_zoneLifeSquadRadius, 0.0f, 4.0f, 0, DOC, SHOW,
	  "Zones kept around other squads (vanilla 0)",
	  "Radius, in zones around each of your characters away from the camera. 0 keeps only the zone each"
	  " one stands in, as the game does by itself (a 3x3 with its Fast zone hopping option): the mod then"
	  " loads and keeps nothing extra around them, which saves CPU with many squads spread over the map,"
	  " and switching the camera to one of them loads its surroundings at that moment. 1 keeps and"
	  " preloads a 3x3 around each."),
	CFG_OINT_LIVE("zoneRetentionMaxHeld", ZoneConfig, cfg_zoneRetentionMaxHeld, 12.0f, 45.0f, INT_MIN, DOC, SHOW,
	  "Max extra zones kept loaded",
	  "How many zones the mod may keep loaded beyond the ones the game keeps by itself (the game keeps"
	  " none extra). Past it the mod keeps only the zones right around the camera and your characters,"
	  " and stops preloading around squads away from the camera, until the count falls back."),
	CFG_ODOUBLE("zoneLifeIdleSeconds", ZoneConfig, cfg_zoneLifeIdleSeconds, 5.0f, 600.0f, NDOC, DEVROW,
	  "Idle zone unload delay seconds",
	  "Seconds a zone the mod loaded must sit outside the camera's and the squads' radii before it is"
	  " unloaded.", 5.0f, 0),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace zone
