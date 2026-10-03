// The settings page factory, built twice at /DZONEHAND_STEP=3: with
// /DKEO_DEBUG (the DEV table, every key) and without it (the PROD table).
// In either, devBuild false shows exactly PROD's rows.

#include "gui/settings_factory.h"
#include "gui/settings_rows.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "base/config_table.h"
#include "base/config_values.h"
#include "bench/bench_slots.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <float.h>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"

// The bench units the custom rows call need these from the runtime; the
// settings rows never reach them.
bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

static const char* const RENDER_TITLE = "Render and particles";
static const char* const MODULE_TITLES[] =
{
	"Zone loading", "Navmesh", "Pathfinding", "Movement and orders", "Crash guards and probes", "Settings panel"
};

#ifdef KEO_DEBUG
static const char* const SUITE_NAME = "settings_factory_units";
#else
static const char* const SUITE_NAME = "settings_factory_prod_units";
#endif
static const size_t CORE_ROWS_DEV = 72;
static const int DEV_ONLY_ROWS = 84;

// The PROD page's rows, by key: the settings an end user changes in game.
static const char* const kProdPage[] =
{
	"renderLevers", "particleStepCap", "foliagePageBudgetMs", "preload", "zoneLifeRetainRadius",
	"navmeshWorkerCount", "navmeshDiskCacheMaxMB", "groupCohesion", "k7PostDeathHold", NULL
};
static const int PROD_PAGE_ROWS = 9;

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
	return std::string(k.label) + (k.live ? "" : " (restart)");
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

// The rows between the header titled title and the next header.
static std::vector<const SettingsRow*> Section(const std::vector<SettingsRow>& rows, const char* title)
{
	std::vector<const SettingsRow*> out;
	size_t i = 0;
	while (i < rows.size() && !(rows[i].kind == SR_HEADER && rows[i].label == title))
		++i;
	for (++i; i < rows.size() && rows[i].kind != SR_HEADER; ++i)
		out.push_back(&rows[i]);
	return out;
}

static std::vector<const SettingsRow*> ModuleSections(const std::vector<SettingsRow>& rows)
{
	std::vector<const SettingsRow*> out;
	for (int m = 0; m < 6; ++m)
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

// ---- Sections --------------------------------------------------------------

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

static void CheckSections()
{
	SettingsStaging st;
	StageAll(&st);
	SettingsBench bench;
	bench.available = false;
	bench.reason = "test";

	// DEV: every module section, then the Benchmark section.
	std::vector<std::string> dev = Headers(Rows(&st, true, &bench));
	bool sections = dev.size() == 9 && dev[0] == "Kenshi Engine Optimizations" && dev[1] == RENDER_TITLE
	                && dev[8] == "Benchmark";
	for (int i = 0; sections && i < 6; ++i) sections = dev[i + 2] == MODULE_TITLES[i];
	Check(sections, "Sections: DEV");

	// PROD: only the sections that keep a row, and no Benchmark.
	std::vector<SettingsRow> rows = Rows(&st, false, &bench);
	std::vector<std::string> prod = Headers(rows);
	const char* const want[] = { "Kenshi Engine Optimizations", RENDER_TITLE, "Zone loading", "Navmesh", "Movement and orders" };
	sections = prod.size() == 5;
	for (int i = 0; sections && i < 5; ++i) sections = prod[i] == want[i];
	Check(sections, "Sections: PROD");
	Check(!rows.empty() && rows[0].kind == SR_HEADER && rows[0].label == "Kenshi Engine Optimizations",
	      "the page opens with the long-name header");

	// The render section is today's, label for label and kind for kind.
	std::vector<const SettingsRow*> render = Section(rows, RENDER_TITLE);
	std::vector<std::string> wantLabel;
	std::vector<SettingsRowKind> wantKind;
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (!k.label || k.kind == RK_TEXT || k.devOnly)
			continue;
		wantLabel.push_back(std::string(k.label) + (k.live ? "" : " (restart)"));
		wantKind.push_back(k.kind == RK_BOOL ? SR_CHECKBOX : SR_SLIDER);
	}
	bool same = render.size() == wantLabel.size();
	for (size_t i = 0; same && i < render.size(); ++i)
		same = render[i]->label == wantLabel[i] && render[i]->kind == wantKind[i];
	Check(same, "Sections");
	Check(render.size() == 3 && render[0]->label == "Render and particle levers (restart)"
	      && render[1]->label == "Cap particle steps at high game speed"
	      && render[2]->label == "Foliage build budget at speed (ms, 0 = off)", "Sections");
}

// ---- Every shown key once; labels; counts ---------------------------------

static void CheckShownKeys(bool devBuild)
{
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> rows = Rows(&st, devBuild, NULL);
	bool ok = true;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		std::vector<const SettingsRow*> sec = Section(rows, mod.title);
		size_t shown = 0;
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!Shown(k, devBuild))
				continue;
			++shown;
			if (CountLabel(sec, RowLabel(k)) != 1)
			{
				ok = false;
				printf("  not shown once: %s\n", k.name);
			}
		}
		if (sec.size() != shown)
			ok = false;
	}
	for (size_t i = 0; i < rows.size(); ++i)
		for (size_t j = i + 1; j < rows.size(); ++j)
			if (rows[i].label == rows[j].label)
				ok = false;
	Check(ok, "Every shown key once");
}

static void CheckLabels()
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (ShouldHaveLabel(k))
				CheckNamed(k.label && k.label[0] && k.tooltip && k.tooltip[0], std::string("label ") + k.name);
		}
	}
}

static void CheckRowCounts()
{
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> dev = Rows(&st, true, NULL), prod = Rows(&st, false, NULL);
	Check(ModuleSections(dev).size() == CORE_ROWS_DEV, "core rows dev");
	Check(ModuleSections(prod).size() == 6, "core rows prod");
	Check(Section(dev, RENDER_TITLE).size() == 21, "render rows dev");
	Check(Section(prod, RENDER_TITLE).size() == 3, "render rows prod");
}

// ---- Restart and devOnly ---------------------------------------------------

static void CheckRestart()
{
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> rows = Rows(&st, true, NULL);
	std::vector<const SettingsRow*> core = ModuleSections(rows);
	bool ok = !core.empty();
	for (size_t i = 0; i < core.size(); ++i)
		ok = ok && EndsWith(core[i]->label, " (restart)");
	int live = 0;
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (!k.live || !k.label || k.kind == RK_TEXT)
			continue;
		const SettingsRow* r = FindLabel(rows, k.label);
		ok = ok && r && !EndsWith(r->label, " (restart)");
		++live;
	}
	Check(ok && live > 0, "Restart");
}

static bool OnProdPage(const char* name)
{
	for (int i = 0; kProdPage[i]; ++i)
	{
		if (strcmp(kProdPage[i], name) == 0)
			return true;
	}
	return false;
}

static void CheckDevOnly()
{
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> dev = Rows(&st, true, NULL), prod = Rows(&st, false, NULL);
	bool ok = true;
	int devRows = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!k.devOnly || !k.label)
				continue;
			if (FindLabel(prod, RowLabel(k)))
				ok = false;
			if (ShouldHaveLabel(k))
			{
				ok = ok && FindLabel(dev, RowLabel(k)) != NULL;
				++devRows;
			}
		}
	}
	Check(ok && devRows == DEV_ONLY_ROWS, "devOnly");

	// Every row with a widget is a developer row unless it is on the PROD page.
	int n = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!ShouldHaveLabel(k))
				continue;
			bool page = OnProdPage(k.name);
			CheckNamed(k.devOnly != page && Shown(k, false) == page, std::string("PROD page ") + k.name);
			n += page ? 1 : 0;
		}
	}
	Check(n == PROD_PAGE_ROWS, "PROD page: every listed key has a row");
	for (size_t i = 0; i < prod.size(); ++i)
		n -= prod[i].kind == SR_HEADER ? 0 : 1;
	Check(n == 0, "PROD page: the listed keys are its only rows");
}

// ---- Numeric rows ----------------------------------------------------------

static void CheckNumericRows()
{
	int c = ModuleFor("islandFarSpan");
	const ConfigModule& core = kConfigModules[c];
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<SettingsRow> dev = Rows(&st, true, NULL), prod = Rows(&st, false, NULL);

	int far = KeyIndex(core, "islandFarSpan");
	const SettingsRow* r = far >= 0 && core.keys[far].label ? FindLabel(dev, RowLabel(core.keys[far])) : NULL;
	Check(r && r->kind == SR_SLIDER && r->lo == 0.0f && r->hi == 8.0f && r->stepExp == 0
	      && r->floatPtr == &st.module[c].slots[far].f, "islandFarSpan: an integer slider, 0 to 8 in whole steps");
	if (r)
	{
		std::vector<IniEntry> e;
		*r->floatPtr = 5.0f;
		int n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
		Check(OneEntry(e, n, "islandFarSpan", "5", INI_INT, true), "islandFarSpan: 5.0 writes islandFarSpan=5");
		e.clear();
		*r->floatPtr = 4.6f;
		n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
		Check(OneEntry(e, n, "islandFarSpan", "5", INI_INT, true), "islandFarSpan: 4.6 writes islandFarSpan=5");
		e.clear();
		*r->floatPtr = 2.4f;
		n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
		Check(n == 0 && e.empty(), "islandFarSpan: a value that rounds to the saved one writes nothing");
	}

	int f = ModuleFor("stitchSourceLines");
	const ConfigModule& fixes = kConfigModules[f];
	int ssl = KeyIndex(fixes, "stitchSourceLines");
	bool none = ssl >= 0;
	for (size_t i = 0; none && i < dev.size(); ++i)
		none = dev[i].intPtr != &st.module[f].slots[ssl].i && dev[i].floatPtr != &st.module[f].slots[ssl].f
		    && (!fixes.keys[ssl].label || dev[i].label.compare(0, strlen(fixes.keys[ssl].label), fixes.keys[ssl].label) != 0);
	for (size_t i = 0; none && i < prod.size(); ++i)
		none = !fixes.keys[ssl].label || prod[i].label.compare(0, strlen(fixes.keys[ssl].label), fixes.keys[ssl].label) != 0;
	Check(none, "stitchSourceLines has no row in either build");

	// Every slider's default and maximum lie on its grid, from sliderLo.
	for (int mm = 1; mm < 7; ++mm)
	for (int i = 0; kConfigModules[mm].keys[i].name; ++i)
	{
		const ConfigModule& mod = kConfigModules[mm];
		const ConfigKey& k = mod.keys[i];
		bool slider = k.kind == CK_FLOAT || k.kind == CK_DOUBLE || (k.kind == CK_INT && !k.choices && k.lo <= k.hi);
		if (k.retired || !slider)
			continue;
		double def = atof(ConfigFormatValue(mod, k, mod.defaults).c_str());
		double scale = (double)(1 << k.stepExp);
		double defSteps = (def - k.sliderLo) * scale, hiSteps = ((double)k.hi - k.sliderLo) * scale;
		CheckNamed(k.stepExp >= 0 && k.stepExp <= 8 && (k.kind != CK_INT || k.stepExp == 0) && k.sliderLo <= def
		           && def <= k.hi && defSteps == floor(defSteps) && hiSteps == floor(hiSteps),
		           std::string("slider grid ") + k.name);
	}

	// A slider never starts below the value the loader clamps to, except a
	// clampPositiveOnly row's 0, which the loader keeps.
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			bool slider = k.kind == CK_FLOAT || k.kind == CK_DOUBLE || (k.kind == CK_INT && !k.choices);
			if (k.retired || !slider || k.lo > k.hi)
				continue;
			CheckNamed(k.sliderLo >= k.lo || (k.clampPositiveOnly && k.sliderLo == 0.0f),
			           std::string("slider start within the clamp ") + k.name);
		}
	}
}

// ---- One staged copy per module --------------------------------------------

static bool s_testA = true;
static bool s_testB = false;

static const ConfigKey kTestKeys[] =
{
	{ "testA", CK_BOOL, 0, 0, 1.0f, 0.0f, false, "Test switch A", "The first test switch.", false, 0.0f, 0,
	  &s_testA, 0, false, false, false, "true", "true", NULL, NULL, 0 },
	{ "testB", CK_BOOL, 0, 0, 1.0f, 0.0f, false, "Test switch B", "The second test switch.", false, 0.0f, 0,
	  &s_testB, 0, false, false, false, "false", "false", NULL, NULL, 0 },
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0,
	  NULL, 0, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

static void CheckStagePerModule()
{
	int r = ModuleIndex("render"), c = ModuleFor("deferral");
	Check(r >= 0 && c >= 0 && r != c, "stage per module: the render and core modules");
	if (r < 0 || c < 0)
		return;

	// Through the page: a core row's change stays in the core module's copy.
	{
		SettingsStaging saved;
		StageAll(&saved);
		SettingsStaging staged = saved;
		std::vector<SettingsRow> rows = Rows(&staged, true, NULL);
		int d = KeyIndex(kConfigModules[c], "deferral");
		const SettingsRow* row = d >= 0 && kConfigModules[c].keys[d].label
			? FindLabel(rows, RowLabel(kConfigModules[c].keys[d])) : NULL;
		Check(row && row->kind == SR_CHECKBOX && row->boolPtr, "stage per module: the deferral row");
		if (row)
			*row->boolPtr = !*row->boolPtr;
		std::vector<IniEntry> e;
		Check(memcmp(&staged.module[r], &saved.module[r], sizeof(ConfigModuleStage)) == 0
		      && ModuleStageEntries(kConfigModules[r], staged.module[r], saved.module[r], &e) == 0
		      && ModuleStageEntries(kConfigModules[c], staged.module[c], saved.module[c], &e) == 1,
		      "stage per module");
	}

	// A third module stages, shows and saves through the same functions.
	ConfigModule mods[3] = { kConfigModules[0], kConfigModules[1], { "test", "Test module", kTestKeys, NULL, NULL, 0 } };
	static ConfigModuleStage saved[3], staged[3];
	memset(saved, 0, sizeof(saved));
	for (int m = 0; m < 3; ++m)
		StageModule(mods[m], &saved[m]);
	memcpy(staged, saved, sizeof(saved));
	Check(saved[2].slots[0].b && !saved[2].slots[1].b, "stage per module: the test globals staged");
	std::vector<SettingsRow> rows;
	for (int m = 0; m < 3; ++m)
		AddModuleRows(mods[m], &staged[m], true, &rows);
	const SettingsRow* a = FindLabel(rows, "Test switch A (restart)");
	Check(a && a->kind == SR_CHECKBOX && a->boolPtr == &staged[2].slots[0].b, "stage per module: the test row");
	if (a)
		*a->boolPtr = false;
	std::vector<IniEntry> e;
	int n = ModuleStageEntries(mods[2], staged[2], saved[2], &e);
	Check(OneEntry(e, n, "testA", "false", INI_BOOL, true), "stage per module");
	e.clear();
	Check(memcmp(&staged[0], &saved[0], sizeof(ConfigModuleStage)) == 0
	      && memcmp(&staged[1], &saved[1], sizeof(ConfigModuleStage)) == 0
	      && ModuleStageEntries(mods[0], staged[0], saved[0], &e) == 0
	      && ModuleStageEntries(mods[1], staged[1], saved[1], &e) == 0, "stage per module");
	Check(s_testA && !s_testB, "stage per module: the globals are never written");
}

static void CheckStageBounds()
{
	bool ok = kConfigModuleCount <= CONFIG_MODULE_MAX;
	SettingsStaging st;
	StageAll(&st);
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		ok = ok && RowCount(mod.keys) <= CONFIG_STAGE_MAX;
		// StageModule skips a state larger than its copy: every module's fits,
		// and its state is staged whole.
		CheckNamed(mod.stateSize <= (size_t)CONFIG_STATE_MAX, std::string("stage bounds: state size ") + mod.name);
		CheckNamed(!mod.state || memcmp(st.module[m].state, mod.state, mod.stateSize) == 0,
		           std::string("stage bounds: state staged ") + mod.name);
		// An offset row's widget binds the field itself, so its kind must be one
		// whose field the widget's pointer type reads.
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (k.target || !ShouldHaveLabel(k))
				continue;
			std::vector<SettingsRow> rows = Rows(&st, true, NULL);
			CheckNamed(CountLabel(Section(rows, mod.title), RowLabel(k)) == 1,
			           std::string("every labelled offset row has one widget ") + k.name);
		}
	}
	Check(ok, "stage bounds");

	Check(RenderModuleIndex() == 0 && ModuleIndex("render") == 0
	      && (void*)&StagedRender(&st) == (void*)st.module[0].state, "the render module at index 0");
}

// ---- The worker row, a change, a custom drop box ---------------------------

static void CheckWorkerRow()
{
	int c = ModuleFor("navmeshWorkerCount");
	const ConfigModule& core = kConfigModules[c];
	int w = KeyIndex(core, "navmeshWorkerCount");
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<SettingsRow> rows = Rows(&st, false, NULL);
	const SettingsRow* r = FindLabel(rows, "Navmesh worker threads (restart)");
	bool ok = r && w >= 0 && r->kind == SR_DROPBOX && r->intPtr == &((navmesh::NavMeshConfig*)st.module[c].state)->cfg_navmeshWorkerCount && r->choices.size() == 7
	       && r->choices[0].first == "Auto" && r->choices[0].second == 0;
	for (int n = 1; ok && n <= 6; ++n)
		ok = r->choices[n].second == n && r->choices[n].first == std::string(1, (char)('0' + n));
	Check(ok, "worker row");
	Check(r && r->tooltip == "Threads that generate and load navmesh tiles in the background. Auto uses half the logical CPUs.",
	      "worker row");
	if (!ok)
		return;
	std::vector<IniEntry> e;
	*r->intPtr = 3;
	int n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
	Check(OneEntry(e, n, "navmeshWorkerCount", "3", INI_INT, true), "worker row");
	e.clear();
	((navmesh::NavMeshConfig*)saved.module[c].state)->cfg_navmeshWorkerCount = 3;
	*r->intPtr = 0;
	n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
	Check(OneEntry(e, n, "navmeshWorkerCount", "0", INI_INT, false), "worker row");
}

static void CheckOneKey()
{
	int c = ModuleFor("deferral");
	const ConfigModule& core = kConfigModules[c];
	int d = KeyIndex(core, "deferral");
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<IniEntry> e;
	SettingsDiff diff = DiffSettings(st, StagedRender(&st), saved);
	int n = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
		n += ModuleStageEntries(kConfigModules[m], st.module[m], saved.module[m], &e);
	Check(n == 0 && e.empty() && diff.applied == 0 && diff.saved == 0, "one key: no change gives no entry");

	std::vector<SettingsRow> rows = Rows(&st, true, NULL);
	const SettingsRow* r = d >= 0 && core.keys[d].label ? FindLabel(rows, RowLabel(core.keys[d])) : NULL;
	Check(r && r->boolPtr && *r->boolPtr, "one key: deferral saved true");
	if (!r)
		return;
	*r->boolPtr = false;
	n = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
		n += ModuleStageEntries(kConfigModules[m], st.module[m], saved.module[m], &e);
	diff = DiffSettings(st, StagedRender(&st), saved);
	Check(OneEntry(e, n, "deferral", "false", INI_BOOL, true) && diff.saved == 1 && diff.applied == 0,
	      "one key: deferral=false");
}

static void CheckCustomDropBox()
{
	int c = ModuleFor("k7PostDeathHold");
	const ConfigModule& core = kConfigModules[c];
	int k7 = KeyIndex(core, "k7PostDeathHold");
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<SettingsRow> rows = Rows(&st, false, NULL);
	const SettingsRow* r = k7 >= 0 && core.keys[k7].label ? FindLabel(rows, RowLabel(core.keys[k7])) : NULL;
	bool ok = r && r->kind == SR_DROPBOX && r->intPtr == &st.module[c].slots[k7].i && r->choices.size() == 3
	       && r->choices[0].first == "off" && r->choices[0].second == K7_HOLD_OFF
	       && r->choices[1].first == "observe" && r->choices[1].second == K7_HOLD_OBSERVE
	       && r->choices[2].first == "on" && r->choices[2].second == K7_HOLD_ON;
	Check(ok, "custom drop box: k7PostDeathHold shows off, observe and on");
	if (!ok)
		return;
	const char* want[3] = { "false", "observe", "true" };
	int held = movement::g_movementCfg.cfg_k7PostDeathHold;
	for (int i = 0; i < 3; ++i)
	{
		saved.module[c].slots[k7].i = -1;
		*r->intPtr = r->choices[i].second;
		std::vector<IniEntry> e;
		int n = ModuleStageEntries(core, st.module[c], saved.module[c], &e);
		Check(n == 1 && e.size() == 1 && e[0].key == "k7PostDeathHold" && e[0].value == want[i],
		      "custom drop box: writes false, observe or true");
		movement::g_movementCfg.cfg_k7PostDeathHold = -1;
		Check(n == 1 && ConfigApplyValue(core, core.keys[k7], e[0].value, &DiscardLog)
		      && movement::g_movementCfg.cfg_k7PostDeathHold == r->choices[i].second, "custom drop box: the written value parses back");
	}
	movement::g_movementCfg.cfg_k7PostDeathHold = held;
}

// Every row the page shows saves: a change through its bound pointer gives
// exactly one entry, its own key's.
static void CheckEveryRowSaves()
{
	SettingsStaging saved;
	StageAll(&saved);
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!Shown(k, true))
				continue;
			SettingsStaging st = saved;
			std::vector<SettingsRow> rows = Rows(&st, true, NULL);
			const SettingsRow* r = FindLabel(rows, RowLabel(k));
			bool changed = false;
			if (r && r->kind == SR_CHECKBOX && r->boolPtr)
			{
				*r->boolPtr = !*r->boolPtr;
				changed = true;
			}
			else if (r && r->kind == SR_SLIDER && r->floatPtr)
			{
				*r->floatPtr = *r->floatPtr == r->hi ? r->lo : r->hi;
				changed = true;
			}
			else if (r && r->kind == SR_DROPBOX && r->intPtr)
			{
				for (size_t ch = 0; !changed && ch < r->choices.size(); ++ch)
				{
					if (r->choices[ch].second != *r->intPtr)
					{
						*r->intPtr = r->choices[ch].second;
						changed = true;
					}
				}
			}
			std::vector<IniEntry> e;
			int n = 0;
			for (int mm = 0; mm < kConfigModuleCount; ++mm)
				n += ModuleStageEntries(kConfigModules[mm], st.module[mm], saved.module[mm], &e);
			CheckNamed(changed && n == 1 && e.size() == 1 && e[0].key == k.name && !e[0].value.empty(),
			           std::string("row saves ") + k.name);
		}
	}
}

// The staged copy holds what the globals and the render state hold.
static void CheckStageReads()
{
	SettingsStaging st;
	StageAll(&st);
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		CheckNamed(!mod.state || memcmp(st.module[m].state, mod.state, mod.stateSize) == 0,
		           std::string("StageModule copies state ") + mod.name);
	}
}

// ---- The commit clamp and the round trip -----------------------------------

static std::string Text(double v)
{
	std::ostringstream ss;
	ss << v;
	return ss.str();
}

static size_t TargetWidth(const ConfigKey& k)
{
	switch (k.kind)
	{
	case CK_BOOL:   return sizeof(bool);
	case CK_INT:    return sizeof(int);
	case CK_FLOAT:  return sizeof(float);
	case CK_DOUBLE: return sizeof(double);
	case CK_CUSTOM: return k.size;
	default:        return 0;
	}
}

// Every target global of a module, so a round trip can load into them and
// put them back.
struct TargetSnapshot
{
	char state[CONFIG_STATE_MAX];
	char bytes[CONFIG_STAGE_MAX][16];
};

static void SaveTargets(const ConfigModule& m, TargetSnapshot* s)
{
	if (m.state) memcpy(s->state, m.state, m.stateSize);
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		size_t w = TargetWidth(k);
		if (k.target && !k.retired && w <= sizeof(s->bytes[i]))
			memcpy(s->bytes[i], k.target, w);
	}
}

static void RestoreTargets(const ConfigModule& m, const TargetSnapshot& s)
{
	if (m.state) memcpy(m.state, s.state, m.stateSize);
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		size_t w = TargetWidth(k);
		if (k.target && !k.retired && w <= sizeof(s.bytes[i]))
			memcpy(k.target, s.bytes[i], w);
	}
}

static const SettingsRow* RowFor(const std::vector<SettingsRow>& rows, const ConfigKey& k)
{
	return k.label ? FindLabel(rows, RowLabel(k)) : NULL;
}

// A close, then the next start: the commit's clamp, the entries the commit
// writes for module m, each applied the way the loader applies an INI line,
// the loader's clamp, and the module staged again from what was loaded. True
// when the row's field reads the same in the tab (after the clamp) and after
// the load. *written is the row's entry value, "" when it wrote none.
static bool RoundTrip(SettingsStaging* st, const SettingsStaging& saved, int m, const ConfigKey& k,
                      const SettingsRow& row, std::string* written)
{
	std::vector<std::string> notes;
	ClampSettings(st, saved, g_renderCfg, &DiscardLog, &notes);

	const ConfigModule& mod = kConfigModules[m];
	static ConfigModuleStage reload;
	memset(&reload, 0, sizeof(reload));
	std::vector<IniEntry> e;
	written->clear();
	if (m == RenderModuleIndex())
	{
		// The render keys are written whole by the render writer, loaded into
		// the render state and clamped as the render loader does.
		RenderIniEntries(StagedRender(st), RenderConfigDefaults(), &e);
		SettingsStaging from = saved;
		RenderConfig loaded = StagedRender(&from);
		ConfigModule local = mod;
		local.state = &loaded;
		for (size_t i = 0; i < e.size(); ++i)
		{
			int ki = KeyIndex(mod, e[i].key.c_str());
			if (ki >= 0)
				ConfigApplyValue(local, mod.keys[ki], e[i].value, &DiscardLog);
			if (e[i].key == k.name)
				*written = e[i].value;
		}
		ClampRenderValues(&loaded, RenderConfigDefaults(), &notes);
		memcpy(reload.state, &loaded, sizeof(loaded));
	}
	else
	{
		TargetSnapshot snap;
		SaveTargets(mod, &snap);
		ModuleStageEntries(mod, st->module[m], saved.module[m], &e);
		for (size_t i = 0; i < e.size(); ++i)
		{
			int ki = KeyIndex(mod, e[i].key.c_str());
			if (ki >= 0)
				ConfigApplyValue(mod, mod.keys[ki], e[i].value, &DiscardLog);
			if (e[i].key == k.name)
				*written = e[i].value;
		}
		ConfigClampLoaded(&DiscardLog);
		StageModule(mod, &reload);
		RestoreTargets(mod, snap);
	}

	const char* field = row.boolPtr ? (const char*)row.boolPtr
	                  : row.floatPtr ? (const char*)row.floatPtr : (const char*)row.intPtr;
	size_t width = row.boolPtr ? sizeof(bool) : row.floatPtr ? sizeof(float) : sizeof(int);
	if (!field)
		return false;
	size_t at = (size_t)(field - (const char*)&st->module[m]);
	return at + width <= sizeof(ConfigModuleStage) && memcmp(field, (const char*)&reload + at, width) == 0;
}

// Stages value into k's row (a slider's value, a drop box's choice, or a
// flipped checkbox), then closes and loads; true when the tab and the load
// agree.
static bool RoundTripValue(int m, const ConfigKey& k, double value, std::string* written)
{
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<SettingsRow> rows = Rows(&st, true, NULL);
	const SettingsRow* r = RowFor(rows, k);
	written->clear();
	if (!r)
		return false;
	if (r->kind == SR_CHECKBOX)
		*r->boolPtr = !*r->boolPtr;
	else if (r->kind == SR_SLIDER)
		*r->floatPtr = (float)value;
	else if (r->kind == SR_DROPBOX)
		*r->intPtr = (int)value;
	return RoundTrip(&st, saved, m, k, *r, written);
}

// What a close writes for the core module after value is staged into key's
// slider.
static std::vector<IniEntry> CommitEntries(const char* key, float value)
{
	int c = ModuleFor(key);
	const ConfigModule& core = kConfigModules[c];
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<SettingsRow> rows = Rows(&st, true, NULL);
	std::vector<IniEntry> e;
	int ki = KeyIndex(core, key);
	const SettingsRow* r = ki >= 0 ? RowFor(rows, core.keys[ki]) : NULL;
	if (!r || !r->floatPtr)
		return e;
	*r->floatPtr = value;
	std::vector<std::string> notes;
	ClampSettings(&st, saved, g_renderCfg, &DiscardLog, &notes);
	ModuleStageEntries(core, st.module[c], saved.module[c], &e);
	return e;
}

static bool OneValue(const std::vector<IniEntry>& e, const char* key, const char* value)
{
	return e.size() == 1 && e[0].key == key && e[0].value == value;
}

static void CheckCommitClamp()
{
	Check(OneValue(CommitEntries("camFocusMaxDist", 250.0f), "camFocusMaxDist", "500"),
	      "commit clamp: camFocusMaxDist 250 writes 500");
	Check(OneValue(CommitEntries("camFocusMaxDist", 60000.0f), "camFocusMaxDist", "50000"),
	      "commit clamp: camFocusMaxDist 60000 writes 50000");
	Check(OneValue(CommitEntries("islandFarSpan", 12.0f), "islandFarSpan", "8"),
	      "commit clamp: islandFarSpan 12 writes 8");
	Check(OneValue(CommitEntries("navmeshDiskCacheMaxMB", 10.0f), "navmeshDiskCacheMaxMB", "32"),
	      "commit clamp: navmeshDiskCacheMaxMB 10 writes 32");
	Check(OneValue(CommitEntries("zoneLifeIdleSeconds", 1.5f), "zoneLifeIdleSeconds", "5"),
	      "commit clamp: zoneLifeIdleSeconds 1.5 writes 5");
	Check(OneValue(CommitEntries("reprioritizeInterval", 40.0f), "reprioritizeInterval", "30"),
	      "commit clamp: reprioritizeInterval 40 writes 30");
	Check(CommitEntries("reprioritizeInterval", 0.2f).empty(),
	      "commit clamp: reprioritizeInterval 0.2 clamps to its saved 1 and writes nothing");
	Check(CommitEntries("camFocusHardMult", std::numeric_limits<float>::quiet_NaN()).empty(),
	      "commit clamp: a value that is not a number keeps the saved one");

	// A typed value beyond the int range saturates as the loader's strtol does,
	// so it clamps to the bound the next start loads.
	Check(OneValue(CommitEntries("navmeshDiskCacheMaxMB", 1e11f), "navmeshDiskCacheMaxMB", "8192"),
	      "commit clamp: navmeshDiskCacheMaxMB 1e11 writes 8192");
	Check(OneValue(CommitEntries("islandFarSpan", 1e11f), "islandFarSpan", "8"),
	      "commit clamp: islandFarSpan 1e11 writes 8");
	Check(OneValue(CommitEntries("islandFarSpan", -1e11f), "islandFarSpan", "0"),
	      "commit clamp: islandFarSpan -1e11 writes 0");
	{
		int ci = ModuleFor("navmeshDiskCacheMaxMB");
		const ConfigModule& cm = kConfigModules[ci];
		const ConfigKey& dk = cm.keys[KeyIndex(cm, "navmeshDiskCacheMaxMB")];
		int held = navmesh::g_navmeshCfg.cfg_navmeshDiskCacheMaxMB;
		bool applied = ConfigApplyValue(cm, dk, "100000000000", &DiscardLog);
		ConfigClampValue(dk, &navmesh::g_navmeshCfg.cfg_navmeshDiskCacheMaxMB, NULL);
		bool loads = applied && navmesh::g_navmeshCfg.cfg_navmeshDiskCacheMaxMB == 8192;
		navmesh::g_navmeshCfg.cfg_navmeshDiskCacheMaxMB = held;
		Check(loads, "commit clamp: the loader reads navmeshDiskCacheMaxMB=100000000000 as 8192");
	}

	// An integer slider's fraction does not survive the close.
	int c = ModuleFor("islandFarSpan");
	const ConfigModule& core = kConfigModules[c];
	int far = KeyIndex(core, "islandFarSpan");
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging st = saved;
	std::vector<std::string> notes;
	st.module[c].slots[far].f = (float)movement::g_movementCfg.cfg_islandFarSpan + 0.4f;
	ClampSettings(&st, saved, g_renderCfg, &DiscardLog, &notes);
	Check(st.module[c].slots[far].f == (float)movement::g_movementCfg.cfg_islandFarSpan, "commit clamp: islandFarSpan keeps no fraction");
	st.module[c].slots[far].f = 4.6f;
	ClampSettings(&st, saved, g_renderCfg, &DiscardLog, &notes);
	Check(st.module[c].slots[far].f == 5.0f, "commit clamp: islandFarSpan 4.6 stays 5");
}

// Every shown row of every module through a close and the next start: a
// checkbox flipped, each choice of a drop box, and a slider at its ends, past
// them, inside, at a fraction and at a value that is not a number.
static void CheckRoundTrips()
{
	int kinds[6] = { 0, 0, 0, 0, 0, 0 };   // target bool, int, float, double, custom; offset
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (!Shown(k, true))
				continue;
			std::vector<double> values;
			if (k.kind == CK_BOOL)
				values.push_back(0.0);
			else if (k.choices)
			{
				for (int ch = 0; ch < k.choiceCount; ++ch)
					values.push_back((double)k.choices[ch].value);
			}
			else
			{
				double lo = k.lo, hi = k.hi;
				values.push_back(k.sliderLo);
				values.push_back(lo);
				values.push_back(hi);
				values.push_back(lo - 1.0);
				values.push_back(hi + 1.0);
				values.push_back((lo + hi) / 2.0);
				values.push_back(lo + 0.4);
				values.push_back(lo + 0.6);
				values.push_back(std::numeric_limits<double>::quiet_NaN());
				if (k.kind == CK_INT)
				{
					values.push_back(1e11);
					values.push_back(-1e11);
				}
			}
			bool ok = true;
			for (size_t v = 0; v < values.size(); ++v)
			{
				std::string written;
				if (!RoundTripValue(m, k, values[v], &written))
				{
					ok = false;
					printf("  round trip %s: staged %s, wrote '%s'\n", k.name, Text(values[v]).c_str(), written.c_str());
				}
			}
			CheckNamed(ok, std::string("round trip ") + k.name);
			if (!k.target) ++kinds[5];
			if (k.kind == CK_BOOL)
				++kinds[0];
			else if (k.kind == CK_INT)
				++kinds[1];
			else if (k.kind == CK_FLOAT)
				++kinds[2];
			else if (k.kind == CK_DOUBLE)
				++kinds[3];
			else if (k.kind == CK_CUSTOM)
				++kinds[4];
		}
	}
	Check(kinds[0] && kinds[1] && kinds[2] && kinds[3] && kinds[4] && kinds[5],
	      "round trip: target bool, int, float, double and custom rows, and offset rows");

	// camFocusMaxDist: each whole value the slider reaches below the clamp is
	// written and loaded as the clamp, and 0 stays 0.
	int c = ModuleFor("camFocusMaxDist");
	const ConfigModule& core = kConfigModules[c];
	const ConfigKey& cf = core.keys[KeyIndex(core, "camFocusMaxDist")];
	bool ok = zone::g_zoneCfg.cfg_camFocusMaxDist == 0.0f;
	for (int v = 1; ok && v < 500; ++v)
	{
		std::string written;
		ok = RoundTripValue(c, cf, (double)v, &written) && written == "500";
		if (!ok)
			printf("  camFocusMaxDist %d wrote '%s'\n", v, written.c_str());
	}
	Check(ok, "round trip camFocusMaxDist 1..499 loads as the tab shows");
	float held = zone::g_zoneCfg.cfg_camFocusMaxDist;
	zone::g_zoneCfg.cfg_camFocusMaxDist = 1000.0f;
	std::string written;
	bool zero = RoundTripValue(c, cf, 0.0, &written) && written == "0";
	zone::g_zoneCfg.cfg_camFocusMaxDist = held;
	Check(zero, "round trip camFocusMaxDist 0 over a saved 1000 stays 0");
}

static void CheckEveryLabelledRowShows()
{
	for (int view = 0; view < 2; ++view)
	{
		SettingsStaging st;
		StageAll(&st);
		std::vector<SettingsRow> rows = Rows(&st, view != 0, NULL);
		for (int m = 0; m < kConfigModuleCount; ++m)
		for (int i = 0; kConfigModules[m].keys[i].name; ++i)
		{
			const ConfigKey& k = kConfigModules[m].keys[i];
			if (!Shown(k, view != 0)) continue;
			CheckNamed(CountLabel(Section(rows, kConfigModules[m].title), RowLabel(k)) == 1,
			           std::string("not shown once: ") + k.name);
		}
	}
}

static void CheckDebugOnlyReaderHidden()
{
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> prod = Rows(&st, false, NULL), dev = Rows(&st, true, NULL);
	for (int m = 0; m < kConfigModuleCount; ++m)
	for (int i = 0; kConfigModules[m].keys[i].name; ++i)
	{
		const ConfigKey& k = kConfigModules[m].keys[i];
		if (!k.debugOnlyReader || !k.label) continue;
		CheckNamed(!FindLabel(prod, RowLabel(k)), std::string("debugOnlyReader row shown in PROD: ") + k.name);
		CheckNamed(CountLabel(Section(dev, kConfigModules[m].title), RowLabel(k)) == 1,
		           std::string("debugOnlyReader row missing in DEV: ") + k.name);
	}
}

static void CheckIniOnlyOffsetText()
{
	SettingsStaging saved;
	StageAll(&saved);
	SettingsStaging staged = saved;
	strcpy_s(StagedRender(&staged).particleLoopingNames, sizeof(StagedRender(&staged).particleLoopingNames),
	         "factory_text_probe");
	std::vector<IniEntry> entries;
	int render = ModuleIndex("render");
	int n = ModuleStageEntries(kConfigModules[render], staged.module[render], saved.module[render], &entries);
	Check(OneEntry(entries, n, "particleLoopingNames", "factory_text_probe", INI_TEXT, true),
	      "INI-only offset text saves through its state field");
}

static void CheckUnlabelledOffsetDouble()
{
	double state = 1.25;
	const double defaults = 1.25;
	const ConfigKey keys[] =
	{
		{ "iniOnlyDouble", CK_DOUBLE, 0, sizeof(double), 1.0f, 10.0f, false, NULL, NULL, false, 1.0f, 0,
		  NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0, false },
		{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0,
		  NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0, false }
	};
	ConfigModule module = { "iniOnly", "INI-only", keys, &state, &defaults, sizeof(state) };
	ConfigModuleStage saved;
	memset(&saved, 0, sizeof(saved));
	StageModule(module, &saved);
	ConfigModuleStage staged = saved;
	*(double*)staged.state = 2.75;
	ClampModuleStage(module, &staged, saved, NULL);
	std::vector<IniEntry> entries;
	int n = ModuleStageEntries(module, staged, saved, &entries);
	Check(OneEntry(entries, n, "iniOnlyDouble", "2.75", INI_FLOAT, true),
	      "unlabelled offset double keeps and saves its state field");
}

int main()
{
	CheckSections();
	CheckShownKeys(false);
	CheckShownKeys(true);
	CheckEveryLabelledRowShows();
	CheckDebugOnlyReaderHidden();
	CheckLabels();
	CheckRowCounts();
	CheckRestart();
	CheckDevOnly();
	CheckNumericRows();
	CheckStagePerModule();
	CheckStageBounds();
	CheckWorkerRow();
	CheckOneKey();
	CheckCustomDropBox();
	CheckEveryRowSaves();
	CheckStageReads();
	CheckCommitClamp();
	CheckRoundTrips();
	CheckIniOnlyOffsetText();
	CheckUnlabelledOffsetDouble();
	return CheckExit(SUITE_NAME);
}
