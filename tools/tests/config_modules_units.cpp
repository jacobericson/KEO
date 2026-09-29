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
	{ "zoneLifeIdleSeconds", "zone" },
};
static const Expected kExpected[] =
{
	{ "render", "Render and particles", sizeof(RenderConfig), &kRenderDefaults },
	{ "zone", "Zone loading", sizeof(zone::ZoneConfig), &zone::kZoneDefaults },
	{ "navmesh", "Navmesh", sizeof(navmesh::NavMeshConfig), &navmesh::kNavMeshDefaults },
	{ "pathfind", "Pathfinding", sizeof(pathfind::PathfindConfig), &pathfind::kPathfindDefaults },
	{ "movement", "Movement and orders", sizeof(movement::MovementConfig), &movement::kMovementDefaults },
	{ "fixes", "Crash guards and probes", sizeof(fixes::FixesConfig), &fixes::kFixesDefaults },
	{ "gui", "Settings panel", sizeof(zoneopt_gui::GuiConfig), &zoneopt_gui::kGuiDefaults },
	{ "core", "Benchmark and retired keys", 0, NULL }
};

static void Fail(const char* key, const std::string& reason)
{
	++CheckFailureCounter();
	printf("config_modules_units: %s: %s\n", key, reason.c_str());
}

int main()
{
	Check(kConfigModuleCount == 8 && kConfigModuleCount <= CONFIG_MODULE_MAX, "eight modules within stage capacity");
	std::set<std::string> names;
	int moduleKeys = 0, activeCore = 0, retiredCore = 0, debug = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		if (m >= 8) { Fail(mod.name, "unexpected module"); continue; }
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
				if (strcmp(k.name, "unstitchProbe") && strcmp(k.name, "sectionKeyProbe")) Fail(k.name, "unexpected debugOnlyReader");
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
			if (k.live && strcmp(mod.name, "render")) Fail(k.name, "live row outside the render module");
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
	Check(moduleKeys == 77 && activeCore == 2 && retiredCore == 22 && debug == 2, "module and core row counts");
	for (size_t i = 0; i < sizeof(kOwners) / sizeof(kOwners[0]); ++i)
	{
		const ConfigModule* mod = NULL;
		const ConfigKey* k = FindConfigKey(kOwners[i].key, &mod);
		if (!k) Fail(kOwners[i].key, std::string("expected in ") + kOwners[i].module + ", missing");
		else if (strcmp(mod->name, kOwners[i].module)) Fail(kOwners[i].key, std::string("expected in ") + kOwners[i].module + ", found in " + mod->name);
	}
	return CheckExit("config_modules_units");
}
