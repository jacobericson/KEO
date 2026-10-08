// fixes_config.cpp - defaults and INI rows for the fixes module.
#include "fixes/fixes_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

static const ConfigChoice kOnScreenStaggerChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" } };

// onScreenStagger: off or on; anything else is refused.
static bool ParseOnScreenStagger(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kOnScreenStaggerChoices); ++i)
	{
		if (_stricmp(val.c_str(), kOnScreenStaggerChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_onScreenStagger = kOnScreenStaggerChoices[i].value;
			return true;
		}
	}
	return false;
}

static const ConfigChoice kPausedOffscreenSkipChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" } };

// pausedOffscreenSkip: off or on; anything else is refused.
static bool ParsePausedOffscreenSkip(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kPausedOffscreenSkipChoices); ++i)
	{
		if (_stricmp(val.c_str(), kPausedOffscreenSkipChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_pausedOffscreenSkip = kPausedOffscreenSkipChoices[i].value;
			return true;
		}
	}
	return false;
}

// graphHeuristicGuard: a boolean stored as 1 or 0; anything else is refused.
static bool ParseGraphHeuristicGuard(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b;
	if (!ParseBool(val, &b))
		return false;
	fixes::g_fixesCfg.graphHeuristicGuardOn = b ? 1 : 0;
	return true;
}

static const ConfigChoice kRelationsSelfFindChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" }, { "verify", 2, "Verify" } };

// relationsSelfFind: off, on or verify; anything else is refused.
static bool ParseRelationsSelfFind(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kRelationsSelfFindChoices); ++i)
	{
		if (_stricmp(val.c_str(), kRelationsSelfFindChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_relationsSelfFind = kRelationsSelfFindChoices[i].value;
			return true;
		}
	}
	return false;
}

// clusterCrossCost: a boolean stored as 1 or 0; anything else is refused.
static bool ParseClusterCrossCost(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b;
	if (!ParseBool(val, &b))
		return false;
	fixes::g_fixesCfg.clusterCrossCostOn = b ? 1 : 0;
	return true;
}

static const ConfigChoice kHullSameSkipChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" } };

// hullSameSkip: off or on; anything else is refused.
static bool ParseHullSameSkip(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kHullSameSkipChoices); ++i)
	{
		if (_stricmp(val.c_str(), kHullSameSkipChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_hullSameSkip = kHullSameSkipChoices[i].value;
			return true;
		}
	}
	return false;
}

static const ConfigChoice kSceneSwitchChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" } };

// sceneForkSkip: off or on; anything else is refused.
static bool ParseSceneForkSkip(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kSceneSwitchChoices); ++i)
	{
		if (_stricmp(val.c_str(), kSceneSwitchChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_sceneForkSkip = kSceneSwitchChoices[i].value;
			return true;
		}
	}
	return false;
}

// instEmptySkip: off or on; anything else is refused.
static bool ParseInstEmptySkip(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kSceneSwitchChoices); ++i)
	{
		if (_stricmp(val.c_str(), kSceneSwitchChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_instEmptySkip = kSceneSwitchChoices[i].value;
			return true;
		}
	}
	return false;
}

static const ConfigChoice kD3dStateChoices[] = { { "off", 0, "Off" }, { "on", 1, "On" } };

// d3dStateSkip: off or on; anything else is refused.
static bool ParseD3dStateSkip(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kD3dStateChoices); ++i)
	{
		if (_stricmp(val.c_str(), kD3dStateChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_d3dStateSkip = kD3dStateChoices[i].value;
			return true;
		}
	}
	return false;
}

namespace fixes {

const FixesConfig kFixesDefaults =
{
	true, // pathExtractGuardEnabled
	true, // sectionStampEnabled
	true, // navMeshUpdateGuardEnabled
#ifdef KEO_DEBUG
	true, // destroyListDiagEnabled
#else
	false, // destroyListDiagEnabled
#endif
	true, // destroyListDeferEnabled
	true, // unstitchGuardEnabled
	32, // cfg_stitchSourceLines
	true, // graphVisitorGuardEnabled
	true, // graphExpandGuardEnabled
	true, // graphPositionGuardEnabled
	true, // meshFaceGuardEnabled
	true, // createInstanceGuardEnabled
	true, // hullDoublePushGuardEnabled
	1, // cfg_hullSameSkip
	1, // cfg_sceneForkSkip
	1, // cfg_instEmptySkip
	1, // cfg_d3dStateSkip
	true, // stitchByteGuardEnabled
	true, // navMeshLifeEnabled
	false, // unstitchProbeEnabled
	true, // sectionKeyProbeEnabled
	true, // physPurecallRecordEnabled
	true, // physQueryGuardEnabled
	true, // corpsePinEnabled
	1, // cfg_relationsSelfFind
	true, // nestValidationGuardEnabled
	1, // cfg_onScreenStagger
	1, // cfg_pausedOffscreenSkip
	0, // graphHeuristicGuardOn
	0, // clusterCrossCostOn
	20, // cfg_ogreJoinSpinUs
	true, // townClaimFixEnabled
	true, // throwOutFixEnabled
	60, // throwOutHoldMinutes
};

FixesConfig g_fixesCfg = kFixesDefaults;
} // namespace fixes

namespace fixes_config_detail {
union FixesConfigPodCheck { fixes::FixesConfig s; };
} // namespace fixes_config_detail
using namespace fixes_config_detail;

static const ConfigChoice kOgreJoinSpinUsChoices[] =
{
	{ "0", 0, "Off" }, { "10", 10, "10 us" }, { "20", 20, "20 us" }, { "50", 50, "50 us" }
};

// ogreJoinSpinUs: 0, 10, 20 or 50; anything else is refused.
static bool ParseOgreJoinSpinUs(const std::string& val, ConfigLogFn log)
{
	(void)log;
	for (int i = 0; i < CFG_COUNT(kOgreJoinSpinUsChoices); ++i)
	{
		if (_stricmp(val.c_str(), kOgreJoinSpinUsChoices[i].ini) == 0)
		{
			fixes::g_fixesCfg.cfg_ogreJoinSpinUs = kOgreJoinSpinUsChoices[i].value;
			return true;
		}
	}
	return false;
}

namespace fixes {

const ConfigKey g_fixesConfigKeys[] =
{
	CFG_OBOOL("pathExtractGuard", FixesConfig, pathExtractGuardEnabled,      DOC, DEVROW,
	  "Path extraction guard",
	  "A fault while reading a path result rolls it back to no new edges instead of crashing; the"
	  " character re-paths on the next tick."),
	CFG_OBOOL("sectionStamp", FixesConfig, sectionStampEnabled,          DOC, DEVROW,
	  "Navmesh section insert counter",
	  "Counts every instance the navmesh streaming collection takes, on the PathGuard: log line."),
	CFG_OBOOL("navMeshUpdateGuard", FixesConfig, navMeshUpdateGuardEnabled,    DOC, DEVROW,
	  "Navmesh update fault recorder",
	  "Records a fault in the navmesh thread's update pass to navmesh_guard.txt and leaves the navmesh"
	  " lock held, so the damaged data is never read."),
	CFG_OBOOL("destroyListDiag", FixesConfig, destroyListDiagEnabled,       NDOC, DEVROW,
	  "Destroy list thread recorder",
	  "Records the calling thread of each insert into the game's destroy list. Changes nothing the game"
	  " does."),
	CFG_OBOOL("destroyListDefer", FixesConfig, destroyListDeferEnabled,      NDOC, DEVROW,
	  "Destroy list deferral",
	  "Queues destroy list inserts made off the main thread and replays them on the main thread, so the"
	  " list has a single writer."),
	CFG_OBOOL("unstitchGuard", FixesConfig, unstitchGuardEnabled,         DOC, DEVROW,
	  "Un-stitch bounds guard",
	  "Drops a stale navmesh connection record that names a node its neighbour no longer has, instead"
	  " of reading past the node map and crashing."),
	CFG_OINT("stitchSourceLines", FixesConfig, cfg_stitchSourceLines, 1.0f, 0.0f, 0, DOC, DEVROW,
	  "Stitch source log sets",
	  "How many sets that lost records to the un-stitch guard are described in the log, two lines each."
	  " 0 prints none."),
	CFG_OBOOL("graphVisitorGuard", FixesConfig, graphVisitorGuardEnabled,     DOC, DEVROW,
	  "Search estimate instance guard",
	  "Keeps a path search node whose section has no graph instance in the search without a distance"
	  " estimate, instead of crashing."),
	CFG_OBOOL("graphExpandGuard", FixesConfig, graphExpandGuardEnabled,      DOC, DEVROW,
	  "Search expansion instance guard",
	  "Drops a path search node whose section has no graph instance, instead of crashing, and carries"
	  " on with the rest."),
	CFG_OBOOL("graphPositionGuard", FixesConfig, graphPositionGuardEnabled,    NDOC, DEVROW,
	  "Cluster graph position guard",
	  "When the cluster graph is consulted, substitutes a node whose section has no graph instance"
	  " instead of crashing. Off only classifies and counts it."),
	CFG_OBOOL("meshFaceGuard", FixesConfig, meshFaceGuardEnabled,         DOC, DEVROW,
	  "Navmesh face bounds guard",
	  "Leaves a navmesh face whose edge record is no longer valid out of its section's bounding box,"
	  " instead of walking outside the section and crashing."),
	CFG_OBOOL("createInstanceGuard", FixesConfig, createInstanceGuardEnabled,   DOC, DEVROW,
	  "Queued navmesh piece guard",
	  "Stops a late stitch from re-creating a navmesh piece that is already queued or already added. Off only counts it."),
	CFG_OBOOL("hullDoublePushGuard", FixesConfig, hullDoublePushGuardEnabled,   DOC, DEVROW,
	  "Physics double delete guard",
	  "Drops a second destroy entry for one physics object before the physics thread deletes it twice."
	  " Off only counts duplicates."),
	CFG_OROW_L("hullSameSkip", CK_CUSTOM, FixesConfig, cfg_hullSameSkip, 1.0f, 0.0f, INT_MIN, false, DOC,
	           ParseHullSameSkip, "Skip unchanged click hulls",
	           "The physics thread stops re-sending a character's click hull to PhysX when it has not moved"
	           " since the last send. Teleports and new hulls always pass.", SHOW, 0.0f, 0,
	           kHullSameSkipChoices, CFG_COUNT(kHullSameSkipChoices), false, true),
	CFG_OROW_L("sceneForkSkip", CK_CUSTOM, FixesConfig, cfg_sceneForkSkip, 1.0f, 0.0f, INT_MIN, false, DOC,
	  ParseSceneForkSkip, "Skip idle scene worker forks",
	  "Skips the renderer's instance-batch cull, skeleton animation and instance bounds worker forks"
	  " whenever their lists give the workers nothing to do.",
	  SHOW, 0.0f, 0, kSceneSwitchChoices, CFG_COUNT(kSceneSwitchChoices), false, true),
	CFG_OROW_L("instEmptySkip", CK_CUSTOM, FixesConfig, cfg_instEmptySkip, 1.0f, 0.0f, INT_MIN, false, DOC,
	  ParseInstEmptySkip, "Skip empty instance batch uploads",
	  "An instance batch with nothing visible in a pass skips its vertex buffer lock and upload.",
	  SHOW, 0.0f, 0, kSceneSwitchChoices, CFG_COUNT(kSceneSwitchChoices), false, true),
	CFG_OROW_L("d3dStateSkip", CK_CUSTOM, FixesConfig, cfg_d3dStateSkip, 1.0f, 0.0f, INT_MIN, false, DOC,
	  ParseD3dStateSkip, "Keep unchanged D3D11 state objects",
	  "After a material change, keeps the bound blend, rasterizer and depth-stencil states whose settings"
	  " did not change, instead of making and binding them again.",
	  SHOW, 0.0f, 0, kD3dStateChoices, CFG_COUNT(kD3dStateChoices), false, true),
	CFG_OBOOL("stitchByteGuard", FixesConfig, stitchByteGuardEnabled,       DOC, DEVROW,
	  "Interior stitch byte guard",
	  "Skips the flag byte the game writes past an interior's navmesh when its stitch finishes. Off"
	  " only records what the write hit."),
	CFG_OBOOL("navMeshLife", FixesConfig, navMeshLifeEnabled,           DOC, DEVROW,
	  "Navmesh section lifecycle lines",
	  "Logs one line each time a navmesh section joins or leaves the streaming collection. Read-only."),
	CFG_OBOOL_DBG("unstitchProbe", FixesConfig, unstitchProbeEnabled,         DOC, DEVROW,
	  "Un-stitch probe",
	  "Before each navmesh instance teardown, logs any connection record whose node index would land"
	  " outside its neighbour's node map. Changes nothing."),
	CFG_OBOOL_DBG("sectionKeyProbe", FixesConfig, sectionKeyProbeEnabled,       DOC, DEVROW,
	  "Section key probe",
	  "Records the last few section-table lookups the world step makes from a packed key, so a crash"
	  " record says whether the index was in range. Changes nothing."),
	CFG_OBOOL("physPurecallRecord", FixesConfig, physPurecallRecordEnabled,    DOC, DEVROW,
	  "PhysX pure call recorder",
	  "Captures the faulting thread and call site into purecall_dump.txt when PhysX aborts on a pure"
	  " virtual call. Forensic only."),
	CFG_OBOOL("physQueryGuard", FixesConfig, physQueryGuardEnabled,        DOC, DEVROW,
	  "PhysX box query guard",
	  "Checks each result of the game's box query before it is used, and drops a collision shape that"
	  " is already destroyed."),
	CFG_OBOOL("corpsePin", FixesConfig, corpsePinEnabled,             DOC, DEVROW,
	  "Carried corpse follows its carrier",
	  "Moves the squad of a carried NPC corpse to its carrier instead of leaving it pinned at the"
	  " pickup spot."),
	CFG_OROW_L("relationsSelfFind", CK_CUSTOM, FixesConfig, cfg_relationsSelfFind, 1.0f, 0.0f, INT_MIN, false, DOC,
	           ParseRelationsSelfFind, "Faction self-relation lookup",
	           "Finds each faction's own relation entry with one lookup instead of walking every entry on every"
	           " AI pass. Verify runs the game's walk too and compares; any difference returns to the walk for"
	           " the session.", SHOW, 0.0f, 0, kRelationsSelfFindChoices, CFG_COUNT(kRelationsSelfFindChoices),
	           false, true),
	CFG_OBOOL("nestValidationGuard", FixesConfig, nestValidationGuardEnabled,   DOC, DEVROW,
	  "Nest validation guard",
	  "Skips the game's nest validation for a cell whose navmesh is not in yet, so no nest is destroyed"
	  " against a missing mesh; the cell is checked again next loading cycle."),
	CFG_OROW_L("onScreenStagger", CK_CUSTOM, FixesConfig, cfg_onScreenStagger, 1.0f, 0.0f, INT_MIN, false, DOC,
	           ParseOnScreenStagger, "Stagger far visibility checks",
	           "A character far beyond the NPC range and off screen gets the game's full visibility check one"
	           " AI pass in four; the other passes keep it off screen exactly as the check would. A camera"
	           " jump checks everyone at once.", SHOW, 0.0f, 0, kOnScreenStaggerChoices,
	           CFG_COUNT(kOnScreenStaggerChoices), false, true),
	CFG_OROW_L("pausedOffscreenSkip", CK_CUSTOM, FixesConfig, cfg_pausedOffscreenSkip, 1.0f, 0.0f, INT_MIN, false,
	           DOC, ParsePausedOffscreenSkip, "Skip off-screen paused updates",
	           "While the game is paused, a character that is off screen, not carried and not in the player's"
	           " squad runs only its zone check; it gets the full update one paused frame in sixteen and as"
	           " soon as it is on screen.", SHOW, 0.0f, 0, kPausedOffscreenSkipChoices,
	           CFG_COUNT(kPausedOffscreenSkipChoices), false, true),
	CFG_OBOOL("townClaimFix", FixesConfig, townClaimFixEnabled,           DOC, SHOW,
	  "Prevent NPC from claiming player buildings",
	  "A building placed near an NPC town joins the player's own town or none, never the NPC town."
	  " Inside an NPC town's radius, where one of your towns also covers the spot, a building that"
	  " does not found a town and arrives with no town, or with a town too small to hold the spot,"
	  " keeps the game's choice: in an unsaved zone that NPC town, whose power it then draws."),
	CFG_OBOOL("throwOutFix", FixesConfig, throwOutFixEnabled,             DOC, SHOW,
	  "Fix guard throwout loop",
	  "A knocked-out intruder a town throws out is not carried back and forth until it wakes."),
	CFG_OINT("throwOutHoldMinutes", FixesConfig, throwOutHoldMinutes, 1.0f, 1440.0f, INT_MIN, DOC, SHOW,
	  "Thrown-out hold in game minutes",
	  "How long a thrown-out body stays off the town's throw-out search if it does not wake first."),
	CFG_OCUSTOM("graphHeuristicGuard", FixesConfig, graphHeuristicGuardOn, ParseGraphHeuristicGuard, NDOC),
	CFG_OCUSTOM("clusterCrossCost", FixesConfig, clusterCrossCostOn, ParseClusterCrossCost, NDOC),
	CFG_OROW_L("ogreJoinSpinUs", CK_CUSTOM, FixesConfig, cfg_ogreJoinSpinUs, 1.0f, 0.0f, INT_MIN, false, DOC,
	           ParseOgreJoinSpinUs, "Ogre barrier join spin",
	           "Before the main thread blocks waiting for Ogre's worker threads, it checks for this long whether"
	           " they have all arrived, and goes on without a wait when they have.", SHOW, 0.0f, 0,
	           kOgreJoinSpinUsChoices, CFG_COUNT(kOgreJoinSpinUsChoices), false, true),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace fixes
