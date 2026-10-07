// The config module contract: ownership, defaults, bounds and load clamps.
#include "base/config_table.h"
#include "base/config_values.h"
#include "gui/settings_factory.h"
#include "render/render_config.h"
#include <cstring>
#include <set>
#include <sstream>
#include "check.h"

bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

namespace config_modules_units_detail {
struct Owner { const char* key; const char* module; };
struct Expected { const char* name; const char* title; size_t size; const void* defaults; };
struct PlannerField { const char* key; int planner::PlannerConfig::* field; int value; };
struct PlannerLine { const char* key; const char* text; int planner::PlannerConfig::* field; int value; };
}
using namespace config_modules_units_detail;

static const Owner kOwners[] =
{
	{ "deferral", "zone" },
	{ "priorityBoost", "navmesh" },
	{ "preload", "zone" },
	{ "movementAware", "zone" },
	{ "caching", "navmesh" },
	{ "groupCohesion", "movement" },
	{ "pathfindDiag", "pathfind" },
	{ "islandFix", "movement" },
	{ "islandFarSpan", "movement" },
	{ "islandEdgeRing", "movement" },
	{ "playerCharRegistry", "movement" },
	{ "reprioFast", "navmesh" },
	{ "routeTier", "navmesh" },
	{ "pathExtractGuard", "fixes" },
	{ "sectionStamp", "fixes" },
	{ "navMeshUpdateGuard", "fixes" },
	{ "destroyListDiag", "fixes" },
	{ "destroyListDefer", "fixes" },
	{ "saveLoadUnload", "zone" },
	{ "escapePauseGuard", "zone" },
	{ "townGuard", "zone" },
	{ "zoneRetention", "zone" },
	{ "islandReadinessRule", "zone" },
	{ "readinessOverrides", "zone" },
	{ "npcWaitDiag", "pathfind" },
	{ "gatePassDiag", "pathfind" },
	{ "pathCostLines", "pathfind" },
	{ "zoneLifeUnload", "zone" },
	{ "islandDeletedReissue", "movement" },
	{ "k7PostDeathHold", "movement" },
	{ "k7DestReadyGate", "movement" },
	{ "k7ArrivalTrigger", "movement" },
	{ "formationFollow", "movement" },
	{ "formationGatherPace", "movement" },
	{ "movementTrace", "movement" },
	{ "navmeshVanillaPruning", "navmesh" },
	{ "navmeshNeighbourSeeds", "navmesh" },
	{ "navmeshBuildLockNarrow", "navmesh" },
	{ "navmeshStallThrottle", "navmesh" },
	{ "clusterGraphBypass", "pathfind" },
	{ "unstitchGuard", "fixes" },
	{ "stitchSourceLines", "fixes" },
	{ "graphVisitorGuard", "fixes" },
	{ "graphExpandGuard", "fixes" },
	{ "graphPositionGuard", "fixes" },
	{ "meshFaceGuard", "fixes" },
	{ "createInstanceGuard", "fixes" },
	{ "hullDoublePushGuard", "fixes" },
	{ "stitchByteGuard", "fixes" },
	{ "navmeshAdjExclusion", "navmesh" },
	{ "playerRepathTier", "pathfind" },
	{ "navMeshLife", "fixes" },
	{ "unstitchProbe", "fixes" },
	{ "sectionKeyProbe", "fixes" },
	{ "settingsPanel", "gui" },
	{ "navmeshMissHash", "navmesh" },
	{ "navmeshMissSplit", "navmesh" },
	{ "navmeshMissSplitBg", "navmesh" },
	{ "zoneCycleStats", "zone" },
	{ "zoneWedgeGuard", "zone" },
	{ "physPurecallRecord", "fixes" },
	{ "physQueryGuard", "fixes" },
	{ "corpsePin", "fixes" },
	{ "nestValidationGuard", "fixes" },
	{ "graphHeuristicGuard", "fixes" },
	{ "playerHierarchical", "pathfind" },
	{ "playerHierOnCap", "pathfind" },
	{ "clusterCrossCost", "fixes" },
	{ "zoneGeometryMode", "zone" },
	{ "camFocus", "zone" },
	{ "preloadKeepAliveSeconds", "zone" },
	{ "navmeshWorkerCount", "navmesh" },
	{ "navmeshGenConcurrency", "navmesh" },
	{ "navmeshDiskCacheMaxMB", "navmesh" },
	{ "camLogInterval", "zone" },
	{ "camFocusMaxDist", "zone" },
	{ "camFocusHardMult", "zone" },
	{ "camFocusHysteresis", "zone" },
	{ "reprioritizeInterval", "navmesh" },
	{ "zoneLifeRetainRadius", "zone" },
	{ "zoneLifeSquadRadius", "zone" },
	{ "zoneRetentionMaxHeld", "zone" },
	{ "zoneLifeIdleSeconds", "zone" },
	{ "plannerMode", "planner" },
	{ "plannerLegSpan", "planner" },
	{ "plannerBaseBuild", "planner" },
	{ "plannerAheadTiles", "planner" },
	{ "plannerWaitSeconds", "planner" },
	{ "plannerWaterCost", "planner" },
	{ "plannerWaterEngine", "planner" },
	{ "plannerAcidCost", "planner" },
	{ "plannerPreArrivalMs", "planner" },
	{ "plannerLegAim", "planner" },
	{ "plannerMergeBias", "planner" },
	{ "plannerMergeDetour", "planner" },
	{ "townClaimFix", "fixes" },
	{ "throwOutFix", "fixes" },
	{ "throwOutHoldMinutes", "fixes" },
	{ "wallSpliceFix", "navmesh" },
	{ "backpackFirstDefault", "inventory" },
	{ "backpackFoodScore", "inventory" },
	{ "backpackDialogueFunction", "inventory" },
	{ "operatorFillBeforeDeliver", "inventory" },
};
static const Expected kExpected[] =
{
	{ "render", "Render and particles", sizeof(RenderConfig), &kRenderDefaults },
	{ "zone", "Zone loading", sizeof(zone::ZoneConfig), &zone::kZoneDefaults },
	{ "navmesh", "Navmesh", sizeof(navmesh::NavMeshConfig), &navmesh::kNavMeshDefaults },
	{ "pathfind", "Pathfinding", sizeof(pathfind::PathfindConfig), &pathfind::kPathfindDefaults },
	{ "movement", "Movement and orders", sizeof(movement::MovementConfig), &movement::kMovementDefaults },
	{ "fixes", "Crash guards and probes", sizeof(fixes::FixesConfig), &fixes::kFixesDefaults },
	{ "inventory", "Inventory and jobs", sizeof(keo_inventory::InventoryConfig), &keo_inventory::kInventoryDefaults },
	{ "planner", "Route planner", sizeof(planner::PlannerConfig), &planner::kPlannerDefaults },
	{ "gui", "Settings panel", sizeof(keo_gui::GuiConfig), &keo_gui::kGuiDefaults },
	{ "core", "Benchmark and retired keys", 0, NULL }
};

static void Fail(const char* key, const std::string& reason)
{
	++CheckFailureCounter();
	printf("config_modules_units: %s: %s\n", key, reason.c_str());
}

static void DiscardLog(const std::string&) {}

// The planner's fields as the process starts and an empty INI loads them: no line applied, then
// the load's clamp.
static const PlannerField kPlannerEmptyIni[] =
{
	{ "plannerMode", &planner::PlannerConfig::mode, 2 },
	{ "plannerLegSpan", &planner::PlannerConfig::legSpan, 2 },
	{ "plannerBaseBuild", &planner::PlannerConfig::baseBuild, 1 },
	{ "plannerAheadTiles", &planner::PlannerConfig::aheadTiles, 3 },
	{ "plannerWaitSeconds", &planner::PlannerConfig::waitSeconds, 10 },
	{ "plannerWaterCost", &planner::PlannerConfig::waterCost, 2 },
	{ "plannerWaterEngine", &planner::PlannerConfig::waterEngine, 1 },
	{ "plannerAcidCost", &planner::PlannerConfig::acidCost, 3 },
	{ "plannerPreArrivalMs", &planner::PlannerConfig::preArrivalMs, 1000 },
	{ "plannerLegAim", &planner::PlannerConfig::legAim, 1 },
	{ "plannerMergeBias", &planner::PlannerConfig::mergeBias, 3 },
	{ "plannerMergeDetour", &planner::PlannerConfig::mergeDetour, 15 },
};

// Each planner key's default text and values inside its range, as an INI line loads them.
static const PlannerLine kPlannerLines[] =
{
	{ "plannerMode", "on", &planner::PlannerConfig::mode, 2 },
	{ "plannerMode", "off", &planner::PlannerConfig::mode, 0 },
	{ "plannerMode", "observe", &planner::PlannerConfig::mode, 1 },
	{ "plannerLegSpan", "2", &planner::PlannerConfig::legSpan, 2 },
	{ "plannerLegSpan", "1", &planner::PlannerConfig::legSpan, 1 },
	{ "plannerLegSpan", "8", &planner::PlannerConfig::legSpan, 8 },
	{ "plannerBaseBuild", "true", &planner::PlannerConfig::baseBuild, 1 },
	{ "plannerBaseBuild", "false", &planner::PlannerConfig::baseBuild, 0 },
	{ "plannerAheadTiles", "3", &planner::PlannerConfig::aheadTiles, 3 },
	{ "plannerAheadTiles", "0", &planner::PlannerConfig::aheadTiles, 0 },
	{ "plannerAheadTiles", "8", &planner::PlannerConfig::aheadTiles, 8 },
	{ "plannerWaitSeconds", "10", &planner::PlannerConfig::waitSeconds, 10 },
	{ "plannerWaitSeconds", "1", &planner::PlannerConfig::waitSeconds, 1 },
	{ "plannerWaitSeconds", "60", &planner::PlannerConfig::waitSeconds, 60 },
	{ "plannerWaterCost", "dynamic", &planner::PlannerConfig::waterCost, 2 },
	{ "plannerWaterCost", "off", &planner::PlannerConfig::waterCost, 0 },
	{ "plannerWaterCost", "floor", &planner::PlannerConfig::waterCost, 1 },
	{ "plannerWaterCost", "engine", &planner::PlannerConfig::waterCost, 3 },
	{ "plannerWaterEngine", "match", &planner::PlannerConfig::waterEngine, 1 },
	{ "plannerWaterEngine", "off", &planner::PlannerConfig::waterEngine, 0 },
	{ "plannerAcidCost", "3", &planner::PlannerConfig::acidCost, 3 },
	{ "plannerAcidCost", "1", &planner::PlannerConfig::acidCost, 1 },
	{ "plannerAcidCost", "10", &planner::PlannerConfig::acidCost, 10 },
	{ "plannerPreArrivalMs", "1000", &planner::PlannerConfig::preArrivalMs, 1000 },
	{ "plannerPreArrivalMs", "0", &planner::PlannerConfig::preArrivalMs, 0 },
	{ "plannerPreArrivalMs", "3000", &planner::PlannerConfig::preArrivalMs, 3000 },
	{ "plannerLegAim", "true", &planner::PlannerConfig::legAim, 1 },
	{ "plannerLegAim", "false", &planner::PlannerConfig::legAim, 0 },
	{ "plannerMergeBias", "3", &planner::PlannerConfig::mergeBias, 3 },
	{ "plannerMergeBias", "1", &planner::PlannerConfig::mergeBias, 1 },
	{ "plannerMergeBias", "10", &planner::PlannerConfig::mergeBias, 10 },
	{ "plannerMergeDetour", "15", &planner::PlannerConfig::mergeDetour, 15 },
	{ "plannerMergeDetour", "0", &planner::PlannerConfig::mergeDetour, 0 },
	{ "plannerMergeDetour", "100", &planner::PlannerConfig::mergeDetour, 100 },
};

// One INI line through the loader, then the load's clamp, from the compiled defaults with the
// line's field set apart, so a refused line leaves the marker; the field as loaded.
static int LoadPlannerLine(const char* key, const char* text, int planner::PlannerConfig::* field, ConfigLoadState* st)
{
	planner::g_plannerCfg = planner::kPlannerDefaults;
	planner::g_plannerCfg.*field = -7;
	ConfigApplyLine(key, text, 1, st, &DiscardLog);
	ConfigClampLoaded(&DiscardLog);
	return planner::g_plannerCfg.*field;
}

static void CheckPlannerKeys()
{
	const planner::PlannerConfig held = planner::g_plannerCfg;
	ConfigClampLoaded(&DiscardLog);
	for (size_t i = 0; i < sizeof(kPlannerEmptyIni) / sizeof(kPlannerEmptyIni[0]); ++i)
	{
		const PlannerField& f = kPlannerEmptyIni[i];
		if (planner::g_plannerCfg.*f.field != f.value) Fail(f.key, "an empty INI does not load the default");
	}
	for (size_t i = 0; i < sizeof(kPlannerLines) / sizeof(kPlannerLines[0]); ++i)
	{
		const PlannerLine& l = kPlannerLines[i];
		ConfigLoadState st;
		int v = LoadPlannerLine(l.key, l.text, l.field, &st);
		if (v != l.value || st.overrides != 1 || st.unrecognised != 0)
		{
			std::ostringstream ss;
			ss << l.key << "=" << l.text << " loads " << v << ", want " << l.value;
			Fail(l.key, ss.str());
		}
	}

	// A ranged key refuses a value below its range and clamps one above it to the top.
	ConfigLoadState below, above;
	Check(LoadPlannerLine("plannerLegSpan", "0", &planner::PlannerConfig::legSpan, &below) == -7
	      && below.overrides == 0 && below.unrecognised == 1, "planner: plannerLegSpan=0 is refused");
	Check(LoadPlannerLine("plannerLegSpan", "9", &planner::PlannerConfig::legSpan, &above) == 8
	      && above.overrides == 1, "planner: plannerLegSpan=9 loads as 8");
	planner::g_plannerCfg = held;
}

// The retain radius as an empty INI loads it, and as an explicit line inside its range loads it.
static void CheckZoneRetainRadius()
{
	const zone::ZoneConfig held = zone::g_zoneCfg;
	zone::g_zoneCfg = zone::kZoneDefaults;
	ConfigClampLoaded(&DiscardLog);
	Check(zone::g_zoneCfg.cfg_zoneLifeRetainRadius == 1, "zone: an empty INI loads zoneLifeRetainRadius 1");
	const char* const texts[] = { "2", "1", "4" };
	const int values[] = { 2, 1, 4 };
	for (int i = 0; i < 3; ++i)
	{
		zone::g_zoneCfg = zone::kZoneDefaults;
		zone::g_zoneCfg.cfg_zoneLifeRetainRadius = -7;
		ConfigLoadState st;
		ConfigApplyLine("zoneLifeRetainRadius", texts[i], 1, &st, &DiscardLog);
		ConfigClampLoaded(&DiscardLog);
		std::ostringstream ss;
		ss << "zone: zoneLifeRetainRadius=" << texts[i] << " loads " << values[i];
		Check(zone::g_zoneCfg.cfg_zoneLifeRetainRadius == values[i] && st.overrides == 1 && st.unrecognised == 0,
		      ss.str().c_str());
	}
	zone::g_zoneCfg = held;
}

// The squad radius and the held-zone cap: their defaults, the squad radius's
// 0 (vanilla) accepted and a negative value refused, the cap clamped to its
// range.
static void CheckZoneSquadRadiusAndCap()
{
	const zone::ZoneConfig held = zone::g_zoneCfg;
	zone::g_zoneCfg = zone::kZoneDefaults;
	ConfigClampLoaded(&DiscardLog);
	Check(zone::g_zoneCfg.cfg_zoneLifeSquadRadius == 1, "zone: an empty INI loads zoneLifeSquadRadius 1");
	Check(zone::g_zoneCfg.cfg_zoneRetentionMaxHeld == 45, "zone: an empty INI loads zoneRetentionMaxHeld 45");

	ConfigLoadState st;
	ConfigApplyLine("zoneLifeSquadRadius", "0", 1, &st, &DiscardLog);
	ConfigClampLoaded(&DiscardLog);
	Check(zone::g_zoneCfg.cfg_zoneLifeSquadRadius == 0 && st.overrides == 1, "zone: zoneLifeSquadRadius=0 loads 0");

	ConfigLoadState st2;
	ConfigApplyLine("zoneLifeSquadRadius", "-1", 1, &st2, &DiscardLog);
	Check(zone::g_zoneCfg.cfg_zoneLifeSquadRadius == 0 && st2.unrecognised == 1,
	      "zone: zoneLifeSquadRadius=-1 is refused and leaves the value");

	ConfigLoadState st3;
	ConfigApplyLine("zoneRetentionMaxHeld", "5", 1, &st3, &DiscardLog);
	ConfigClampLoaded(&DiscardLog);
	Check(zone::g_zoneCfg.cfg_zoneRetentionMaxHeld == 12, "zone: zoneRetentionMaxHeld=5 clamps to 12");
	zone::g_zoneCfg = held;
}

// The live offset rows outside the render module: each one's readers all run
// on the main thread, where the settings tab writes it.
static bool LiveOffsetRowAllowed(const char* module, const char* key)
{
	if (strcmp(module, "zone"))
		return false;
	return !strcmp(key, "zoneLifeRetainRadius") || !strcmp(key, "zoneLifeSquadRadius")
	    || !strcmp(key, "zoneRetentionMaxHeld");
}

int main()
{
	Check(kConfigModuleCount == 10 && kConfigModuleCount <= CONFIG_MODULE_MAX, "ten modules within stage capacity");
	std::set<std::string> names;
	int moduleKeys = 0, activeCore = 0, retiredCore = 0, debug = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		if (m >= 10) { Fail(mod.name, "unexpected module"); continue; }
		const Expected& e = kExpected[m];
		if (strcmp(mod.name, e.name) || strcmp(mod.title, e.title)) Fail(mod.name, "module order or title");
		if (mod.state)
		{
			if (!mod.defaults) Fail(mod.name, "offset rows with no defaults");
			if (mod.stateSize != e.size || mod.stateSize > CONFIG_STATE_MAX) Fail(mod.name, "state size");
			if (mod.defaults != e.defaults) Fail(mod.name, "defaults object address");
			if (mod.defaults && memcmp(mod.state, mod.defaults, mod.stateSize)) Fail(mod.name, "instance differs from defaults");
		}
		else if (strcmp(mod.name, "core") || mod.defaults || mod.stateSize) Fail(mod.name, "core state must be null");
		std::set<size_t> offsets;
		int i = 0;
		for (; i < CONFIG_STAGE_MAX && mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!names.insert(k.name).second) Fail(k.name, "duplicate key");
			if (strcmp(mod.name, "core") == 0) { if (k.retired) ++retiredCore; else ++activeCore; }
			else if (strcmp(mod.name, "render")) ++moduleKeys;
			if (k.debugOnlyReader)
			{
				++debug;
				if (strcmp(k.name, "unstitchProbe") && strcmp(k.name, "sectionKeyProbe") && strcmp(k.name, "formationFollow") && strcmp(k.name, "movementTrace")) Fail(k.name, "unexpected debugOnlyReader");
			}
			if (mod.state)
			{
				if (k.target) Fail(k.name, "target row in offset module");
				if (k.offset + k.size > mod.stateSize) Fail(k.name, "offset outside state");
				if (!offsets.insert(k.offset).second) Fail(k.name, "duplicate offset");
				size_t width = k.kind == CK_BOOL ? 1 : k.kind == CK_INT || k.kind == CK_FLOAT ? 4 : k.kind == CK_DOUBLE ? 8 : k.size;
				if (k.kind == CK_CUSTOM) width = strcmp(k.name, "islandEdgeRing") == 0 ? sizeof(bool) : sizeof(int);
				// Render scalar rows preserve the legacy zero size; their kind supplies the width.
				if (k.size != width && (strcmp(mod.name, "render") || k.size != 0)) Fail(k.name, "kind width");
				if (k.offset + width > mod.stateSize) Fail(k.name, "kind width outside state");
			}
			if (k.live && strcmp(mod.name, "render") && !LiveOffsetRowAllowed(mod.name, k.name)) Fail(k.name, "live row outside the render module and the allowed zone rows");
			if (!mod.state || !strcmp(mod.name, "render") || k.retired || k.lo > k.hi
			    || (k.kind != CK_INT && k.kind != CK_FLOAT && k.kind != CK_DOUBLE)) continue;
			std::vector<unsigned char> saved((unsigned char*)mod.state, (unsigned char*)mod.state + mod.stateSize);
			char* p = (char*)mod.state + k.offset;
			bool ok;
			if (k.kind == CK_INT) { *(int*)p = (int)k.hi + 1; ConfigClampLoaded(NULL); ok = *(int*)p == (int)k.hi; }
			else if (k.kind == CK_FLOAT) { *(float*)p = k.hi + 1.0f; ConfigClampLoaded(NULL); ok = *(float*)p == k.hi; }
			else { *(double*)p = (double)k.hi + 1.0; ConfigClampLoaded(NULL); ok = *(double*)p == (double)k.hi; }
			memcpy(mod.state, &saved[0], mod.stateSize);
			if (!ok) Fail(k.name, "not clamped at load");
		}
		if (i == CONFIG_STAGE_MAX) Fail(mod.name, "no table end within stage capacity");
	}
	Check(moduleKeys == 103 && activeCore == 2 && retiredCore == 23 && debug == 4, "module and core row counts");
	for (size_t i = 0; i < sizeof(kOwners) / sizeof(kOwners[0]); ++i)
	{
		const ConfigModule* mod = NULL;
		const ConfigKey* k = FindConfigKey(kOwners[i].key, &mod);
		if (!k) Fail(kOwners[i].key, std::string("expected in ") + kOwners[i].module + ", missing");
		else if (strcmp(mod->name, kOwners[i].module)) Fail(kOwners[i].key, std::string("expected in ") + kOwners[i].module + ", found in " + mod->name);
	}
	CheckPlannerKeys();
	CheckZoneRetainRadius();
	CheckZoneSquadRadiusAndCap();
	return CheckExit("config_modules_units");
}
