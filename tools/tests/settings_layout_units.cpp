// The settings page's layout, built twice at /DZONEHAND_STEP=3: with
// /DKEO_DEBUG (the DEV table) and without it (the PROD table). Row order in
// each section and the footnote in both views, the PROD layout table, and the
// fallback section for a shown key the table does not place.

#include "gui/settings_layout.h"
#include "render/render_config.h"
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "settings_units_common.h"

// The bench units the custom rows call need these from the runtime; the
// settings rows never reach them.
bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

#ifdef KEO_DEBUG
static const char* const SUITE_NAME = "settings_layout_units";
#else
static const char* const SUITE_NAME = "settings_layout_prod_units";
#endif

// A row's widget group in a section's order: checkboxes, drop boxes, sliders.
static int Group(const SettingsRow& r)
{
	return r.kind == SR_CHECKBOX ? 0 : r.kind == SR_DROPBOX ? 1 : 2;
}

// In every section the checkboxes come first, then the drop boxes, then the
// sliders, each group's startup-only rows after its live ones; one footnote
// follows the sections exactly when a startup-only row shows, before the
// Benchmark section.
static void CheckSectionOrder()
{
	SettingsStaging st;
	StageAll(&st);
	SettingsBench bench;
	bench.available = false;
	bench.reason = "test";
	for (int view = 0; view < 2; ++view)
	{
		std::vector<SettingsRow> rows = Rows(&st, view != 0, &bench);
		bool order = true, anyRestart = false, tooltips = true;
		int last = -1;
		int notes = 0;
		size_t note = rows.size(), benchmark = rows.size();
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const SettingsRow& r = rows[i];
			if (r.kind == SR_HEADER)
			{
				last = -1;
				if (r.label == "Benchmark")
					benchmark = i;
				continue;
			}
			if (r.kind == SR_NOTE)
			{
				++notes;
				note = i;
				continue;
			}
			if (r.restart)
			{
				anyRestart = true;
				tooltips = tooltips && EndsWith(r.tooltip, " Takes effect after restarting the game.");
			}
			if (i < benchmark)
			{
				int place = 2 * Group(r) + (r.restart ? 1 : 0);
				order = order && place >= last;
				last = place;
			}
		}
		std::string name = view ? "DEV" : "PROD";
		CheckNamed(order, "checkboxes, drop boxes, sliders, each live then restart: " + name);
		CheckNamed(tooltips, "restart rows say so in their tooltips: " + name);
		CheckNamed(anyRestart && notes == 1 && rows[note].label == "* Takes effect after restarting the game.",
		           "one footnote: " + name);
		bool placed = note < rows.size() && (view ? benchmark == note + 1 : note + 1 == rows.size());
		CheckNamed(placed, "the footnote follows the sections: " + name);
	}
}

// The tab's own table places every PROD row once, in a section matching
// this suite's, each section's places together.
static void CheckLayoutTable()
{
	using keo_gui::kSettingsPlaces;
	bool ok = true;
	int n = 0;
	for (; kSettingsPlaces[n].section; ++n)
	{
		const keo_gui::SettingsPlace& p = kSettingsPlaces[n];
		const ConfigModule* mod = NULL;
		const ConfigKey* k = FindConfigKey(p.key, &mod);
		bool here = k && mod && strcmp(mod->name, p.module) == 0 && Shown(*k, false)
		         && strcmp(ProdSection(p.key), p.section) == 0;
		for (int j = 0; here && j < n; ++j)
			here = strcmp(kSettingsPlaces[j].key, p.key) != 0;
		// A section seen before must be the one just before this place.
		for (int j = 0; here && j + 1 < n; ++j)
			here = strcmp(kSettingsPlaces[j].section, p.section) != 0 || strcmp(kSettingsPlaces[n - 1].section, p.section) == 0;
		CheckNamed(here, std::string("layout place ") + p.key);
		ok = ok && here;
	}
	Check(ok && n == PROD_PAGE_ROWS, "layout table: every PROD row placed once");
}


// A shown key the table misses still reaches the tab, once, under its own
// heading after the player sections, and no heading repeats.
static void CheckFallback()
{
	using keo_gui::kSettingsPlaces;
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> rows;
	keo_gui::AddPlayerSections(kSettingsPlaces, &st, &rows);
	std::vector<std::string> full = Headers(rows);
	Check(full.size() == 3 && full[2] == PROD_SECTIONS[2], "fallback: the full table leaves no other section");

	// The table without one Zone loading key, its module's title being a
	// player section's.
	const char* const dropped = "navmeshDiskCacheMaxMB";
	std::vector<keo_gui::SettingsPlace> places;
	for (int i = 0; kSettingsPlaces[i].section; ++i)
	{
		if (strcmp(kSettingsPlaces[i].key, dropped) != 0)
			places.push_back(kSettingsPlaces[i]);
	}
	keo_gui::SettingsPlace end = { NULL, NULL, NULL };
	places.push_back(end);
	rows.clear();
	keo_gui::AddPlayerSections(&places[0], &st, &rows);

	std::vector<std::string> headers = Headers(rows);
	bool unique = true;
	for (size_t i = 0; i < headers.size(); ++i)
		for (size_t j = i + 1; j < headers.size(); ++j)
			unique = unique && headers[i] != headers[j];
	Check(unique && headers.size() == 4 && headers[3] == keo_gui::OTHER_SETTINGS_TITLE,
	      "fallback: one Other settings section after the player ones");
	const ConfigModule* mod = NULL;
	const ConfigKey* k = FindConfigKey(dropped, &mod);
	std::vector<const SettingsRow*> other = Section(rows, keo_gui::OTHER_SETTINGS_TITLE);
	Check(k && other.size() == 1 && other[0]->label == RowLabel(*k) && Section(rows, "Zone loading").size() == 6,
	      "fallback: the unplaced key alone under Other settings");
}

int main()
{
	CheckSectionOrder();
	CheckLayoutTable();
	CheckFallback();
	return CheckExit(SUITE_NAME);
}
