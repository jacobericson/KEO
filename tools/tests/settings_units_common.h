#ifndef KEO_TOOLS_TESTS_SETTINGS_UNITS_COMMON_H
#define KEO_TOOLS_TESTS_SETTINGS_UNITS_COMMON_H

// The settings-page suites' shared expectations and helpers
// (settings_factory_units, settings_layout_units and their PROD builds):
// the module titles, the PROD page by key and section, the suite's own
// shown-key predicates, and row lookups. Include after check.h.

#include "gui/settings_factory.h"
#include "gui/settings_rows.h"
#include "base/config_table.h"
#include "base/config_values.h"
#include "bench/bench_slots.h"
#include <cstring>
#include <string>
#include <vector>

static const char* const RENDER_TITLE = "Render and particles";
static const char* const MODULE_TITLES[] =
{
	"Zone loading", "Navmesh", "Pathfinding", "Movement and orders", "Crash guards and probes",
	"Inventory and jobs", "Route planner", "Settings panel"
};

// The PROD page's rows in display order, by key and section: the settings an
// end user changes in game; in a section, checkboxes, drop boxes, then
// sliders, each live before startup-only, but a grouped row right after its
// group's checkbox.
struct ProdRow
{
	const char* key;
	const char* section;
};
static const ProdRow kProdPage[] =
{
	{ "preload", "Zone loading" }, { "navmeshWorkerCount", "Zone loading" },
	{ "zoneLifeRetainRadius", "Zone loading" }, { "zoneLifeSquadRadius", "Zone loading" },
	{ "zoneRetentionMaxHeld", "Zone loading" }, { "plannerAheadTiles", "Zone loading" },
	{ "navmeshDiskCacheMaxMB", "Zone loading" },
	{ "particleStepCap", "Performance" }, { "renderLevers", "Performance" }, { "foliagePageBudgetMs", "Performance" },
	{ "groupCohesion", "Squad movement" }, { "formationGatherPace", "Squad movement" },
	{ "k7PostDeathHold", "Squad movement" }, { "plannerMode", "Squad movement" },
	{ "townClaimFix", "Gameplay fixes" }, { "throwOutFix", "Gameplay fixes" },
	{ "throwOutHoldMinutes", "Gameplay fixes" }, { "wallSpliceFix", "Gameplay fixes" },
	{ "backpackFixes", "Backpacks and jobs" }, { "operatorHoldUntil", "Backpacks and jobs" },
	{ NULL, NULL }
};
static const int PROD_PAGE_ROWS = 20;
static const char* const PROD_SECTIONS[] = { "Zone loading", "Performance", "Squad movement", "Gameplay fixes",
	"Backpacks and jobs" };

static void CheckNamed(bool ok, const std::string& what)
{
	Check(ok, what.c_str());
}

static void DiscardLog(const std::string&) {}

// ---- The suite's own predicates, independent of the factory's ------------

static bool HasWidget(const ConfigKey& k)
{
	switch (k.kind)
	{
	case CK_BOOL:
	case CK_FLOAT:
	case CK_DOUBLE: return true;
	case CK_INT:    return k.choices != NULL || k.lo <= k.hi;
	case CK_CUSTOM: return k.choices != NULL;
	default:        return false;
	}
}

static bool ShouldHaveLabel(const ConfigKey& k)
{
	return !k.retired && k.kind != CK_TEXT && HasWidget(k);
}

static bool Shown(const ConfigKey& k, bool devBuild)
{
	return k.label && ShouldHaveLabel(k) && (devBuild || (!k.devOnly && !k.debugOnlyReader));
}

static std::string RowLabel(const ConfigKey& k)
{
	return std::string(k.label ? k.label : "") + (k.live ? "" : " *");
}

static bool EndsWith(const std::string& s, const char* tail)
{
	size_t n = strlen(tail);
	return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

static int ModuleIndex(const char* name)
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		if (strcmp(kConfigModules[m].name, name) == 0)
			return m;
	}
	return -1;
}

static int ModuleFor(const char* key)
{
	const ConfigModule* mod = NULL;
	FindConfigKey(key, &mod);
	return mod ? (int)(mod - kConfigModules) : -1;
}

static int KeyIndex(const ConfigModule& m, const char* name)
{
	for (int i = 0; m.keys[i].name; ++i)
	{
		if (!m.keys[i].retired && strcmp(m.keys[i].name, name) == 0)
			return i;
	}
	return -1;
}

static int RowCount(const ConfigKey* keys)
{
	int n = 0;
	while (keys[n].name)
		++n;
	return n;
}

// The rows between the header titled title and the next header or note.
static std::vector<const SettingsRow*> Section(const std::vector<SettingsRow>& rows, const char* title)
{
	std::vector<const SettingsRow*> out;
	size_t i = 0;
	while (i < rows.size() && !(rows[i].kind == SR_HEADER && rows[i].label == title))
		++i;
	for (++i; i < rows.size() && rows[i].kind != SR_HEADER && rows[i].kind != SR_NOTE; ++i)
		out.push_back(&rows[i]);
	return out;
}

static const char* ProdSection(const char* key)
{
	for (int i = 0; kProdPage[i].key; ++i)
	{
		if (strcmp(kProdPage[i].key, key) == 0)
			return kProdPage[i].section;
	}
	return "";
}

// The heading a shown key's row sits under: its module's title in DEV, its
// player section in PROD.
static const char* SectionOf(const ConfigModule& mod, const ConfigKey& k, bool devBuild)
{
	return devBuild ? mod.title : ProdSection(k.name);
}

static std::vector<const SettingsRow*> ModuleSections(const std::vector<SettingsRow>& rows)
{
	std::vector<const SettingsRow*> out;
	for (int m = 0; m < 8; ++m)
	{
		std::vector<const SettingsRow*> sec = Section(rows, MODULE_TITLES[m]);
		out.insert(out.end(), sec.begin(), sec.end());
	}
	return out;
}

static const SettingsRow* FindLabel(const std::vector<SettingsRow>& rows, const std::string& label)
{
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].label == label)
			return &rows[i];
	}
	return NULL;
}

static int CountLabel(const std::vector<const SettingsRow*>& rows, const std::string& label)
{
	int n = 0;
	for (size_t i = 0; i < rows.size(); ++i)
		n += rows[i]->label == label ? 1 : 0;
	return n;
}

static void StageAll(SettingsStaging* s)
{
	memset(s, 0, sizeof(*s));
	for (int m = 0; m < kConfigModuleCount; ++m)
		StageModule(kConfigModules[m], &s->module[m]);
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		s->benchSpeed[i] = 1;
}

static std::vector<SettingsRow> Rows(SettingsStaging* s, bool devBuild, const SettingsBench* bench)
{
	std::vector<SettingsRow> rows;
	BuildSettingsRows(s, devBuild, bench, &rows);
	return rows;
}

static bool OneEntry(const std::vector<IniEntry>& e, int count, const char* key, const char* value, IniValueKind kind,
                     bool append)
{
	return count == 1 && e.size() == 1 && e[0].key == key && e[0].value == value && e[0].kind == kind
	    && e[0].append == append;
}

static std::vector<std::string> Headers(const std::vector<SettingsRow>& rows)
{
	std::vector<std::string> headers;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].kind == SR_HEADER)
			headers.push_back(rows[i].label);
	}
	return headers;
}

#endif
