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
// sliders, each group's startup-only rows after its live ones, a grouped row
// sitting under its checkbox outside that order; one footnote
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
			if (i < benchmark && r.enabledByRow < 0)
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
	Check(full.size() == 5 && full[4] == PROD_SECTIONS[4], "fallback: the full table leaves no other section");

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
	Check(unique && headers.size() == 6 && headers[5] == keo_gui::OTHER_SETTINGS_TITLE,
	      "fallback: one Other settings section after the player ones");
	const ConfigModule* mod = NULL;
	const ConfigKey* k = FindConfigKey(dropped, &mod);
	std::vector<const SettingsRow*> other = Section(rows, keo_gui::OTHER_SETTINGS_TITLE);
	Check(k && other.size() == 1 && other[0]->label == RowLabel(*k) && Section(rows, "Zone loading").size() == 6,
	      "fallback: the unplaced key alone under Other settings");
}

// The row index of the only row labelled with key's label, -1 when none or
// more than one carries it.
static int OnlyRowOf(const std::vector<SettingsRow>& rows, const char* key)
{
	const ConfigModule* mod = NULL;
	const ConfigKey* k = FindConfigKey(key, &mod);
	if (!k)
		return -1;
	int at = -1, n = 0;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].label == RowLabel(*k))
		{
			at = (int)i;
			++n;
		}
	}
	return n == 1 ? at : -1;
}

// The rows labelled with key's label, every one of them.
static std::vector<int> RowsOf(const std::vector<SettingsRow>& rows, const char* key)
{
	std::vector<int> out;
	const ConfigModule* mod = NULL;
	const ConfigKey* k = FindConfigKey(key, &mod);
	for (size_t i = 0; k && i < rows.size(); ++i)
	{
		if (rows[i].label == RowLabel(*k))
			out.push_back((int)i);
	}
	return out;
}

// The PROD tab's two groups: the throw-out slider under its checkbox and the
// operator tier under the backpack switch, each greyed by its head's row.
static void CheckGroups()
{
	using keo_gui::kSettingsPlaces;
	Check(keo_gui::SettingsGroupsValid(kSettingsPlaces), "groups: the table's groups are valid");

	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> prod = Rows(&st, false, NULL);
	static const char* const kMembers[2][2] =
	{
		{ "throwOutFix", "throwOutHoldMinutes" },
		{ "backpackFixes", "operatorHoldUntil" }
	};
	bool follows = true;
	for (int g = 0; g < 2; ++g)
	{
		int head = OnlyRowOf(prod, kMembers[g][0]);
		int member = OnlyRowOf(prod, kMembers[g][1]);
		follows = follows && head >= 0 && member == head + 1;
		std::vector<int> all = RowsOf(prod, kMembers[g][1]);
		for (size_t i = 0; i < all.size(); ++i)
			follows = follows && head >= 0 && prod[all[i]].enabledByRow == head;
	}
	Check(follows, "groups: each member follows its head");

	// Every greyed row names a checkbox row carrying its own group head's label.
	bool gated = true;
	int gatedRows = 0;
	for (size_t i = 0; i < prod.size(); ++i)
	{
		int h = prod[i].enabledByRow;
		if (h < 0)
			continue;
		++gatedRows;
		bool known = false;
		for (int g = 0; g < 2; ++g)
		{
			std::vector<int> heads = RowsOf(prod, kMembers[g][0]);
			std::vector<int> members = RowsOf(prod, kMembers[g][1]);
			for (size_t j = 0; j < members.size(); ++j)
			{
				if (members[j] != (int)i)
					continue;
				known = true;
				gated = gated && heads.size() == 1 && heads[0] == h;
			}
		}
		gated = gated && known && (size_t)h < prod.size() && prod[h].kind == SR_CHECKBOX;
	}
	Check(gated && gatedRows == 2, "groups: each member's enabledByRow is its head's row, a checkbox");

	// The table with the throw-out slider placed before its checkbox.
	std::vector<keo_gui::SettingsPlace> places;
	for (int i = 0; kSettingsPlaces[i].section; ++i)
		places.push_back(kSettingsPlaces[i]);
	keo_gui::SettingsPlace end = { NULL, NULL, NULL, NULL };
	places.push_back(end);
	for (size_t i = 0; i + 1 < places.size(); ++i)
	{
		if (places[i].key && strcmp(places[i].key, "throwOutFix") == 0)
		{
			keo_gui::SettingsPlace t = places[i];
			places[i] = places[i + 1];
			places[i + 1] = t;
			break;
		}
	}
	std::vector<SettingsRow> swapped;
	keo_gui::AddPlayerSections(&places[0], &st, &swapped);
	int fix = OnlyRowOf(swapped, "throwOutFix");
	int hold = OnlyRowOf(swapped, "throwOutHoldMinutes");
	int wall = OnlyRowOf(swapped, "wallSpliceFix");
	Check(!keo_gui::SettingsGroupsValid(&places[0]) && fix >= 0 && hold >= 0 && wall >= 0
	      && swapped[fix].enabledByRow == -1 && swapped[hold].enabledByRow == -1 && hold > wall,
	      "groups: a group whose first place is not a checkbox is refused");

	keo_gui::SettingsPlace split[] =
	{
		{ "Gameplay fixes",     "fixes", "throwOutFix",         "throwOut" },
		{ "Backpacks and jobs", "fixes", "throwOutHoldMinutes", "throwOut" },
		{ NULL, NULL, NULL, NULL }
	};
	Check(!keo_gui::SettingsGroupsValid(split), "groups: a group split across sections is refused");

	std::vector<SettingsRow> dev = Rows(&st, true, NULL);
	bool none = !dev.empty();
	for (size_t i = 0; i < dev.size(); ++i)
		none = none && dev[i].enabledByRow == -1;
	Check(none, "groups: the DEV page has no group");
}

// The three Gameplay fixes labels are the user's exact words, each on one PROD row.
static void CheckRuledLabels()
{
	static const char* const kLabels[3][2] =
	{
		{ "townClaimFix",  "Prevent NPC from claiming player buildings" },
		{ "throwOutFix",   "Fix guard throwout loop" },
		{ "wallSpliceFix", "Player buildings properly stitch into navmesh" }
	};
	SettingsStaging st;
	StageAll(&st);
	std::vector<SettingsRow> prod = Rows(&st, false, NULL);
	bool exact = true;
	for (int i = 0; i < 3; ++i)
	{
		const ConfigModule* mod = NULL;
		const ConfigKey* k = FindConfigKey(kLabels[i][0], &mod);
		exact = exact && k && k->label && strcmp(k->label, kLabels[i][1]) == 0
		        && OnlyRowOf(prod, kLabels[i][0]) >= 0;
	}
	Check(exact, "labels: the three Gameplay fixes labels are the user's exact words");
}

int main()
{
	CheckSectionOrder();
	CheckLayoutTable();
	CheckFallback();
	CheckGroups();
	CheckRuledLabels();
	return CheckExit(SUITE_NAME);
}
