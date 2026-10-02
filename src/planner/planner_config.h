// planner_config.h - The route planner module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"
#include "planner/plan_policy.h"   // PlannerMode

namespace planner {

// Starts as a copy of kPlannerDefaults, then written by LoadConfig on the main thread before any
// hook installs. Later writers, all on the main thread during InstallHooks, only clear mode to
// PLANNER_OFF: the hooks' install step when it refuses to arm or a detour fails to install, the
// store's start step when the store cannot be allocated, and the arm when the search scratch cannot
// be. Read on any thread; every field is read at startup or through a snapshot taken at arm.
struct PlannerConfig
{
	int mode;          // plannerMode: PlannerMode; off by default
	int legSpan;       // plannerLegSpan: 1..8, default 2; the leg bound and the direct threshold
	int baseBuild;     // plannerBaseBuild: 1 builds the whole-map base at startup; default 1
	int aheadTiles;    // plannerAheadTiles: 0..8, default 3; route tiles fed to the preload queue
	int waitSeconds;   // plannerWaitSeconds: 1..60, default 10; the awaited-section wait
	int waterCost;     // plannerWaterCost: PlanWaterMode; floor by default
};

extern PlannerConfig g_plannerCfg;
extern const PlannerConfig kPlannerDefaults;
extern const ConfigKey g_plannerConfigKeys[];

const char* PlannerModeName(int mode);   // "off", "observe" or "on"
const char* PlanWaterModeName(int mode); // "off", "floor", "dynamic" or "engine"

} // namespace planner
