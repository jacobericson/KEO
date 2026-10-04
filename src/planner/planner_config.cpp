// planner_config.cpp - defaults and INI rows for the route planner module.
#include "planner/planner_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <string.h>

// The parsers' one write: an accepted value into its field of the live config.
static bool StoreField(int planner::PlannerConfig::* field, int v) { planner::g_plannerCfg.*field = v; return true; }

// plannerMode: off, observe or on; anything else is refused.
static bool ParsePlannerMode(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int mode;
	if (_stricmp(val.c_str(), "off") == 0)
		mode = planner::PLANNER_OFF;
	else if (_stricmp(val.c_str(), "observe") == 0)
		mode = planner::PLANNER_OBSERVE;
	else if (_stricmp(val.c_str(), "on") == 0)
		mode = planner::PLANNER_ON;
	else
		return false;
	return StoreField(&planner::PlannerConfig::mode, mode);
}

// plannerBaseBuild: true or false.
static bool ParsePlannerBaseBuild(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (!ParseBoolOr(val, NULL, &b, &third) || third)
		return false;
	return StoreField(&planner::PlannerConfig::baseBuild, b ? 1 : 0);
}

// plannerWaterCost: off, floor, dynamic or engine; anything else is refused.
static bool ParsePlannerWaterCost(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int mode;
	if (_stricmp(val.c_str(), "off") == 0)
		mode = planner::PWC_OFF;
	else if (_stricmp(val.c_str(), "floor") == 0)
		mode = planner::PWC_FLOOR;
	else if (_stricmp(val.c_str(), "dynamic") == 0)
		mode = planner::PWC_DYNAMIC;
	else if (_stricmp(val.c_str(), "engine") == 0)
		mode = planner::PWC_ENGINE;
	else
		return false;
	return StoreField(&planner::PlannerConfig::waterCost, mode);
}

// plannerWaterEngine: off or match; anything else is refused.
static bool ParsePlannerWaterEngine(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int mode;
	if (_stricmp(val.c_str(), "off") == 0)
		mode = planner::PWE_OFF;
	else if (_stricmp(val.c_str(), "match") == 0)
		mode = planner::PWE_MATCH;
	else
		return false;
	return StoreField(&planner::PlannerConfig::waterEngine, mode);
}

// plannerLegAim: true or false.
static bool ParsePlannerLegAim(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (!ParseBoolOr(val, NULL, &b, &third) || third)
		return false;
	return StoreField(&planner::PlannerConfig::legAim, b ? 1 : 0);
}

// The drop boxes' choices: the INI text each parser accepts, the value it stores.
static const ConfigChoice kPlannerModeChoices[] =
{
	{ "off", planner::PLANNER_OFF, "Off" }, { "observe", planner::PLANNER_OBSERVE, "Observe", true }, { "on", planner::PLANNER_ON, "On" }
};

static const ConfigChoice kPlannerWaterCostChoices[] =
{
	{ "off", planner::PWC_OFF, "Off" }, { "floor", planner::PWC_FLOOR, "Floor" },
	{ "dynamic", planner::PWC_DYNAMIC, "Dynamic" }, { "engine", planner::PWC_ENGINE, "Engine" }
};

static const ConfigChoice kPlannerWaterEngineChoices[] =
{
	{ "off", planner::PWE_OFF, "Off" }, { "match", planner::PWE_MATCH, "Match" }
};

// The int fields that hold a switch: 0 off, 1 on.
static const ConfigChoice kPlannerSwitchChoices[] =
{
	{ "false", 0, "Off" }, { "true", 1, "On" }
};

namespace planner {

const PlannerConfig kPlannerDefaults =
{
	PLANNER_ON, // mode
	2, // legSpan
	1, // baseBuild
	3, // aheadTiles
	10, // waitSeconds
	PWC_DYNAMIC, // waterCost
	PWE_MATCH, // waterEngine
	3, // acidCost
	1000, // preArrivalMs
	1, // legAim
	3, // mergeBias
	15, // mergeDetour
};

PlannerConfig g_plannerCfg = kPlannerDefaults;

const char* PlannerModeName(int mode)
{
	if (mode == PLANNER_OBSERVE) return "observe";
	if (mode == PLANNER_ON) return "on";
	return "off";
}

const char* PlanWaterModeName(int mode)
{
	if (mode == PWC_OFF) return "off";
	if (mode == PWC_DYNAMIC) return "dynamic";
	if (mode == PWC_ENGINE) return "engine";
	return "floor";
}

} // namespace planner

namespace planner_config_detail {
union PlannerConfigPodCheck { planner::PlannerConfig s; };
} // namespace planner_config_detail
using namespace planner_config_detail;

namespace planner {

const ConfigKey g_plannerConfigKeys[] =
{
	CFG_OCUSTOM_CHOICES("plannerMode", PlannerConfig, mode, ParsePlannerMode, NDOC, SHOW,
	  "Route planner",
	  "Plans a long squad move over the whole map and walks it leg by leg, so the squad does not stop"
	  " short where the game cannot see a path. Observe (DEV) only logs the plans.", kPlannerModeChoices),
	// Below 1 is refused; above 8 loads as 8.
	CFG_OINT("plannerLegSpan", PlannerConfig, legSpan, 1.0f, 8.0f, 1, NDOC, DEVROW,
	  "Planner leg span (cells)",
	  "The most cells one planned leg crosses, and the span below which an order walks unplanned. Default 2."),
	CFG_OCUSTOM_CHOICES("plannerBaseBuild", PlannerConfig, baseBuild, ParsePlannerBaseBuild, NDOC, DEVROW,
	  "Build the whole-map route graph",
	  "Builds the planner's graph of the whole map from the navmesh tiles at startup. Off plans over"
	  " the loaded cells only. Default On.", kPlannerSwitchChoices),
	// Below 0 is refused; above 8 loads as 8.
	CFG_OINT("plannerAheadTiles", PlannerConfig, aheadTiles, 0.0f, 8.0f, 0, NDOC, DEVROW,
	  "Route tiles to preload",
	  "Tiles along a planned route queued for preloading ahead of the squad. 0 queues none; default 3."),
	// Below 1 is refused; above 60 loads as 60.
	CFG_OINT("plannerWaitSeconds", PlannerConfig, waitSeconds, 1.0f, 60.0f, 1, NDOC, DEVROW,
	  "Wait for a route section (s)",
	  "Seconds a squad waits at a leg's end for the next navmesh section before the planner re-plans."
	  " Default 10."),
	CFG_OCUSTOM_CHOICES("plannerWaterCost", PlannerConfig, waterCost, ParsePlannerWaterCost, NDOC, DEVROW,
	  "Planner water cost",
	  "How the planner prices water: Dynamic by each character's swim speed, Floor never below the"
	  " game's price, Engine the game's price alone, Off as land. Default Dynamic.", kPlannerWaterCostChoices),
	CFG_OCUSTOM_CHOICES("plannerWaterEngine", PlannerConfig, waterEngine, ParsePlannerWaterEngine, NDOC, DEVROW,
	  "Water cost in path requests",
	  "Match writes the planner's water price into each player path request, so the game's own leg"
	  " searches price water as the plan did. Default Match.", kPlannerWaterEngineChoices),
	// Below 1 is refused; above 10 loads as 10.
	CFG_OINT("plannerAcidCost", PlannerConfig, acidCost, 1.0f, 10.0f, 1, NDOC, DEVROW,
	  "Acid water cost (x)",
	  "How many times the water price acidic water costs a character who is not immune. 1 prices it as"
	  " any water; default 3."),
	// Below 0 is refused; above 3000 loads as 3000.
	CFG_OINT("plannerPreArrivalMs", PlannerConfig, preArrivalMs, 0.0f, 3000.0f, 0, NDOC, DEVROW,
	  "Request the next leg early (ms)",
	  "The path latency, in milliseconds, the next leg's request is sent ahead of a squad reaching its"
	  " leg's end. 0 is off; default 1000."),
	CFG_OCUSTOM_CHOICES("plannerLegAim", PlannerConfig, legAim, ParsePlannerLegAim, NDOC, DEVROW,
	  "Aim legs at the next leg",
	  "Aims each leg's border crossing along the line to the leg after it rather than at the crossing's"
	  " nearest point. Default On.", kPlannerSwitchChoices),
	// Below 1 is refused; above 10 loads as 10.
	CFG_OINT("plannerMergeBias", PlannerConfig, mergeBias, 1.0f, 10.0f, 1, NDOC, DEVROW,
	  "Run-together route bias",
	  "A run-together member's step onto the leading member's route costs 1/n of its length, so the"
	  " members join one route. 1 is no bias; default 3."),
	// Below 0 is refused; above 100 loads as 100.
	CFG_OINT("plannerMergeDetour", PlannerConfig, mergeDetour, 0.0f, 100.0f, 0, NDOC, DEVROW,
	  "Run-together detour cap (%)",
	  "The longest detour, in per cent of its own route, a member takes to join the leading route."
	  " Default 15."),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace planner
