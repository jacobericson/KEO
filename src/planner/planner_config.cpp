// planner_config.cpp - defaults and INI rows for the route planner module.
#include "planner/planner_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <stdlib.h>
#include <string.h>

// The parsers' one write: an accepted value into its field of the live config.
static bool StoreField(int planner::PlannerConfig::* field, int v) { planner::g_plannerCfg.*field = v; return true; }

// A decimal integer in [lo, hi] with nothing after it; anything else is refused.
static bool ParseRanged(const std::string& val, int lo, int hi, int* out)
{
	const char* s = val.c_str();
	char* end = NULL;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || v < lo || v > hi)
		return false;
	*out = (int)v;
	return true;
}

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

// plannerLegSpan: 1..8.
static bool ParsePlannerLegSpan(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 1, 8, &v))
		return false;
	return StoreField(&planner::PlannerConfig::legSpan, v);
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

// plannerAheadTiles: 0..8.
static bool ParsePlannerAheadTiles(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 0, 8, &v))
		return false;
	return StoreField(&planner::PlannerConfig::aheadTiles, v);
}

// plannerWaitSeconds: 1..60.
static bool ParsePlannerWaitSeconds(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 1, 60, &v))
		return false;
	return StoreField(&planner::PlannerConfig::waitSeconds, v);
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

// plannerAcidCost: 1..10; a value out of range is refused and the default stays.
static bool ParsePlannerAcidCost(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 1, 10, &v))
		return false;
	return StoreField(&planner::PlannerConfig::acidCost, v);
}

// plannerAdvanceSection: true or false.
static bool ParsePlannerAdvanceSection(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (!ParseBoolOr(val, NULL, &b, &third) || third)
		return false;
	return StoreField(&planner::PlannerConfig::advanceSection, b ? 1 : 0);
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

// plannerMergeBias: 1..10; a value out of range is refused and the default stays.
static bool ParsePlannerMergeBias(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 1, 10, &v))
		return false;
	return StoreField(&planner::PlannerConfig::mergeBias, v);
}

// plannerMergeDetour: 0..100 per cent; a value out of range is refused and the default stays.
static bool ParsePlannerMergeDetour(const std::string& val, ConfigLogFn log)
{
	(void)log;
	int v;
	if (!ParseRanged(val, 0, 100, &v))
		return false;
	return StoreField(&planner::PlannerConfig::mergeDetour, v);
}

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
	1, // advanceSection
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
	CFG_OCUSTOM("plannerMode", PlannerConfig, mode, ParsePlannerMode, NDOC),
	CFG_OCUSTOM("plannerLegSpan", PlannerConfig, legSpan, ParsePlannerLegSpan, NDOC),
	CFG_OCUSTOM("plannerBaseBuild", PlannerConfig, baseBuild, ParsePlannerBaseBuild, NDOC),
	CFG_OCUSTOM("plannerAheadTiles", PlannerConfig, aheadTiles, ParsePlannerAheadTiles, NDOC),
	CFG_OCUSTOM("plannerWaitSeconds", PlannerConfig, waitSeconds, ParsePlannerWaitSeconds, NDOC),
	CFG_OCUSTOM("plannerWaterCost", PlannerConfig, waterCost, ParsePlannerWaterCost, NDOC),
	CFG_OCUSTOM("plannerWaterEngine", PlannerConfig, waterEngine, ParsePlannerWaterEngine, NDOC),
	CFG_OCUSTOM("plannerAcidCost", PlannerConfig, acidCost, ParsePlannerAcidCost, NDOC),
	CFG_OCUSTOM("plannerAdvanceSection", PlannerConfig, advanceSection, ParsePlannerAdvanceSection, NDOC),
	CFG_OCUSTOM("plannerLegAim", PlannerConfig, legAim, ParsePlannerLegAim, NDOC),
	CFG_OCUSTOM("plannerMergeBias", PlannerConfig, mergeBias, ParsePlannerMergeBias, NDOC),
	CFG_OCUSTOM("plannerMergeDetour", PlannerConfig, mergeDetour, ParsePlannerMergeDetour, NDOC),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace planner
