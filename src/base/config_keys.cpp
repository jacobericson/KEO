// config_keys.cpp - benchmark and retired rows, and the constant module list.
#include "base/config_table.h"
#include "base/config_values.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "bench/bench_lever_ab.h"
#include "bench/bench_sweep.h"
#include <limits.h>
#include <sstream>
#include <string.h>

static bool ParseBenchLevers(const std::string& val, ConfigLogFn log)
{
	std::vector<std::string> unknownLevers;
	bool leversFallback = false;
	ParseBenchLeversKey("bench.levers", val, &unknownLevers, &leversFallback);
	for (size_t i = 0; i < unknownLevers.size(); ++i)
		log("Bench: unknown lever '" + unknownLevers[i] + "' in bench.levers, ignored");
	if (leversFallback)
		log("Bench: bench.levers named no known lever, using every lever");
	return true;
}

static bool ParseBenchSweep(const std::string& val, ConfigLogFn log)
{
	std::vector<std::string> badLegs, extraLegs;
	bool sweepDefault = false;
	ParseBenchSweepKey("bench.sweep", val, &badLegs, &extraLegs, &sweepDefault);
	for (size_t i = 0; i < badLegs.size(); ++i)
		log("Bench: bench.sweep entry '" + badLegs[i] + "' ignored (a slot:1 or slot:20 leg is expected)");
	for (size_t i = 0; i < extraLegs.size(); ++i)
	{
		std::ostringstream ss;
		ss << "Bench: bench.sweep entry '" << extraLegs[i] << "' ignored (past the " << BENCH_SWEEP_MAX_LEGS
		   << "-leg limit)";
		log(ss.str());
	}
	if (sweepDefault)
		log("Bench: bench.sweep named no valid leg, using the default " + BenchSweepListText());
	return true;
}

// ---- The core table ------------------------------------------------------
//
// Every macro sets every column but debugOnlyReader, which reads false;
// minInt's "none" is INT_MIN, not zero. A row with lo > hi has no clamp. A
// row's two default texts are its values as the INI writes them, DEV first.

#define CFG_ROW(n, kind, size, lo, hi, tgt, minI, posOnly, doc, dev, prod, fn, label, tip, diag, sLo, sExp, ch, chN) \
	{ n, kind, 0, size, lo, hi, false, label, tip, diag, sLo, sExp, \
	  tgt, minI, posOnly, doc, false, dev, prod, fn, ch, chN }
// A custom row without choices stays INI-only.
#define CFG_CUSTOM(n, tgt, width, fn, doc, dev, prod) \
	CFG_ROW(n, CK_CUSTOM, width, 1.0f, 0.0f, tgt, INT_MIN, false, doc, dev, prod, fn, NULL, NULL, false, 0.0f, 0, NULL, 0)
#define CFG_RETIRED(n) \
	{ n, CK_CUSTOM, 0, 0, 1.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, \
	  NULL, INT_MIN, false, false, true, NULL, NULL, NULL, NULL, 0 }

#define DOC  true

static const ConfigKey kCoreKeys[] =
{
	CFG_CUSTOM("bench.levers",        NULL, 0, ParseBenchLevers, DOC, "", ""),
	CFG_CUSTOM("bench.sweep",         NULL, 0, ParseBenchSweep, DOC,
	           "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20", "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20"),

	// Keys a feature used to read. An old INI that still sets one gets a single
	// "Config: retired key '<key>' ignored" line instead of the generic
	// unknown-key line, so the log says the key is gone on purpose.
	//   squadPathCache: the squad path cache and its injection are deleted. The
	//                   injection reported a path whose edges were not a connected
	//                   chain and crashed the post-processor.
	//   stuckRetry:     the PLAYER STUCK retry (replacement move orders toward
	//                   the zone exit) was deleted; it was off by default.
	//                   PLAYER STUCK itself stays a diagnostic.
	//   instanceCullSingleThread: the single-thread instance culling lever read
	//                   render-queue slot 0, which Kenshi never uses, so every
	//                   InstanceBatchHW stopped drawing. Removed.
	//   camFocusDwellSec: the camera-focus zone-cell dwell was replaced by real
	//                   hysteresis on the prediction axes (camFocusHysteresis);
	//                   the dwell state was never read by anything behavioural.
	//   exitFaceProbe and the 17 tuning/capacity keys below: each parsed,
	//                   clamped and (cameraReserved/formationTimeout) cross-clamped
	//                   against another dead key, but nothing outside the parser
	//                   read the global it wrote. Retiring them stops them being
	//                   counted as applied overrides.
	//   plannerAdvanceSection: the route planner's arrival on entering a portal's far section. The
	//                   engine consults the planner only once a character is parked at the end of its
	//                   path, which is the planner's own portal point, so the rule could never act.
	CFG_RETIRED("squadPathCache"),
	CFG_RETIRED("stuckRetry"),
	CFG_RETIRED("instanceCullSingleThread"),
	CFG_RETIRED("camFocusDwellSec"),
	CFG_RETIRED("exitFaceProbe"),
	CFG_RETIRED("activePollInterval"),
	CFG_RETIRED("baselineScanInterval"),
	CFG_RETIRED("cameraReserved"),
	CFG_RETIRED("charScanInterval"),
	CFG_RETIRED("edgeThreshold"),
	CFG_RETIRED("evictInterval"),
	CFG_RETIRED("formationTimeout"),
	CFG_RETIRED("gatherRadiusSq"),
	CFG_RETIRED("gatherTimeout"),
	CFG_RETIRED("maxCharZones"),
	CFG_RETIRED("maxFormationGroups"),
	CFG_RETIRED("maxFormationMembers"),
	CFG_RETIRED("maxPendingOrder"),
	CFG_RETIRED("maxPreloaded"),
	CFG_RETIRED("maxWatched"),
	CFG_RETIRED("preloadThreshold"),
	CFG_RETIRED("scatterApproachDistSq"),
	CFG_RETIRED("plannerAdvanceSection"),

	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0,
	  NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

// The settings page's order; every initializer is an address constant.
extern const ConfigModule kConfigModules[] =
{
	{ "render", "Render and particles", g_renderKeys, &g_renderCfg, &kRenderDefaults, sizeof(RenderConfig) },
	{ "zone", "Zone loading", zone::g_zoneConfigKeys, &zone::g_zoneCfg, &zone::kZoneDefaults, sizeof(zone::ZoneConfig) },
	{ "navmesh", "Navmesh", navmesh::g_navmeshConfigKeys, &navmesh::g_navmeshCfg, &navmesh::kNavMeshDefaults, sizeof(navmesh::NavMeshConfig) },
	{ "pathfind", "Pathfinding", pathfind::g_pathfindConfigKeys, &pathfind::g_pathfindCfg, &pathfind::kPathfindDefaults, sizeof(pathfind::PathfindConfig) },
	{ "movement", "Movement and orders", movement::g_movementConfigKeys, &movement::g_movementCfg, &movement::kMovementDefaults, sizeof(movement::MovementConfig) },
	{ "fixes", "Crash guards and probes", fixes::g_fixesConfigKeys, &fixes::g_fixesCfg, &fixes::kFixesDefaults, sizeof(fixes::FixesConfig) },
	{ "planner", "Route planner", planner::g_plannerConfigKeys, &planner::g_plannerCfg, &planner::kPlannerDefaults, sizeof(planner::PlannerConfig) },
	{ "gui", "Settings panel", keo_gui::g_guiConfigKeys, &keo_gui::g_guiCfg, &keo_gui::kGuiDefaults, sizeof(keo_gui::GuiConfig) },
	{ "core", "Benchmark and retired keys", kCoreKeys, NULL, NULL, 0 },
};
extern const int kConfigModuleCount = (int)(sizeof(kConfigModules) / sizeof(kConfigModules[0]));
