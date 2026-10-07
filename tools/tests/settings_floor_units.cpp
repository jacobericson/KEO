// The squad radius row's floor while the game's Fast zone hopping option is
// on: the zone module's rule, the floor on the tab's row on both pages, a
// close with the floor ahead of the clamp, and the floors that change
// nothing. Built with /DKEO_DEBUG; the PROD page comes from devBuild false.

#include "gui/settings_rows.h"
#include "zone/zone_config.h"
#include "render/render_config.h"
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "settings_units_common.h"

// The bench units the custom rows call need these from the runtime; the
// settings rows never reach them.
bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

static const char* const SUITE_NAME = "settings_floor_units";
static const char* const SQUAD = "zoneLifeSquadRadius";
static const char* const NOTE =
	"Fast zone hopping is on (Options, General): the game itself keeps a 3x3 around each of your characters,"
	" so this can't go below 1. Turn Fast zone hopping off to set 0.";

static float& SquadSlot(SettingsStaging* s)
{
	int z = ModuleFor(SQUAD);
	return s->module[z].slots[KeyIndex(kConfigModules[z], SQUAD)].f;
}

static int& SquadField(SettingsStaging* s)
{
	return ((zone::ZoneConfig*)s->module[ModuleFor(SQUAD)].state)->cfg_zoneLifeSquadRadius;
}

// Every module staged from the running config, the squad radius stored as n.
static void Store(SettingsStaging* s, int n)
{
	StageAll(s);
	SquadSlot(s) = (float)n;
	SquadField(s) = n;
}

static void CheckRule()
{
	ConfigFloor on = zone::ZoneSquadRadiusFloor(true);
	ConfigFloor off = zone::ZoneSquadRadiusFloor(false);
	Check(on.key && strcmp(on.key, SQUAD) == 0 && on.floor == 1, "rule: Fast zone hopping on puts the squad radius floor at 1");
	Check(off.key && strcmp(off.key, SQUAD) == 0 && off.floor == 0, "rule: Fast zone hopping off leaves the squad radius floor at 0");
	Check(on.note && std::string(on.note) == NOTE, "rule: the note names the option, its place and how to set 0");
}

static void CheckRow(bool devBuild, bool fzh, int stored)
{
	SettingsStaging st;
	Store(&st, stored);
	std::vector<SettingsRow> plain = Rows(&st, devBuild, NULL);
	std::vector<SettingsRow> rows = Rows(&st, devBuild, NULL);
	bool raised = ApplySettingsFloor(&st, zone::ZoneSquadRadiusFloor(fzh), &rows);
	const ConfigModule& zm = kConfigModules[ModuleFor(SQUAD)];
	const ConfigKey& k = zm.keys[KeyIndex(zm, SQUAD)];
	const SettingsRow* r = FindLabel(rows, RowLabel(k));
	int want = fzh && stored < 1 ? 1 : stored;
	std::ostringstream tag;
	tag << "row (" << (devBuild ? "DEV" : "PROD") << " page, Fast zone hopping " << (fzh ? "on" : "off")
	    << ", stored " << stored << "): ";
	Check(r && r->floatPtr == &SquadSlot(&st), (tag.str() + "the squad slider binds its staged slot").c_str());
	if (!r)
		return;
	Check(r->lo == (fzh ? 1.0f : 0.0f), (tag.str() + "the slider starts at the floor").c_str());
	Check(*r->floatPtr == (float)want && SquadField(&st) == want,
	      (tag.str() + "the staged value is the stored one, raised to the floor").c_str());
	Check(raised == (fzh && stored < 1), (tag.str() + "the floor reports a raise only when it raised").c_str());
	std::string base = k.tooltip;
	Check(r->tooltip == (fzh ? base + " " + NOTE : base),
	      (tag.str() + "the tooltip gains the note only while the option is on").c_str());
	int differ = 0;
	for (size_t i = 0; i < rows.size() && i < plain.size(); ++i)
		differ += rows[i].lo != plain[i].lo || rows[i].tooltip != plain[i].tooltip ? 1 : 0;
	Check(rows.size() == plain.size() && differ == (fzh ? 1 : 0), (tag.str() + "no other row changes").c_str());
}

// A close: the saved copy holds stored, the tab's slot holds typed; the
// floor, then the clamp. Returns the staged field; e gets the INI entries.
static int Close(bool fzh, int stored, float typed, std::vector<IniEntry>* e)
{
	SettingsStaging saved;
	Store(&saved, stored);
	SettingsStaging st = saved;
	SquadSlot(&st) = typed;
	ApplySettingsFloor(&st, zone::ZoneSquadRadiusFloor(fzh), NULL);
	std::vector<std::string> notes;
	ClampSettings(&st, saved, g_renderCfg, &DiscardLog, &notes);
	int z = ModuleFor(SQUAD);
	e->clear();
	ModuleStageEntries(kConfigModules[z], st.module[z], saved.module[z], e);
	return SquadField(&st);
}

static bool Saves(const std::vector<IniEntry>& e, const char* value)
{
	return e.size() == 1 && e[0].key == SQUAD && e[0].value == value;
}

static void CheckClose()
{
	std::vector<IniEntry> e;
	float nan = std::numeric_limits<float>::quiet_NaN();
	Check(Close(true, 0, 0.0f, &e) == 1 && Saves(e, "1"), "close: a stored 0 left at 0 with Fast zone hopping on saves 1");
	Check(Close(true, 0, 1.0f, &e) == 1 && Saves(e, "1"), "close: a stored 0 shown at 1 saves 1");
	Check(Close(true, 1, 0.0f, &e) == 1 && e.empty(), "close: a typed 0 over a stored 1 keeps 1 and writes nothing");
	Check(Close(true, 2, 0.0f, &e) == 1 && Saves(e, "1"), "close: a typed 0 over a stored 2 saves 1");
	Check(Close(true, 0, 0.4f, &e) == 1 && Saves(e, "1"), "close: a typed 0.4 saves the floor");
	Check(Close(true, 0, nan, &e) == 1 && Saves(e, "1"), "close: a value that is not a number saves the floor");
	Check(Close(true, 1, 3.0f, &e) == 3 && Saves(e, "3"), "close: a value above the floor passes");
	Check(Close(false, 1, 0.0f, &e) == 0 && Saves(e, "0"), "close: with Fast zone hopping off a typed 0 saves 0");
	Check(Close(false, 0, 0.0f, &e) == 0 && e.empty(), "close: with Fast zone hopping off a stored 0 stays 0");
}

static void CheckLiveApply()
{
	const zone::ZoneConfig held = zone::g_zoneCfg;
	zone::g_zoneCfg.cfg_zoneLifeSquadRadius = 0;
	SettingsStaging saved;
	Store(&saved, 0);
	SettingsStaging st = saved;
	ApplySettingsFloor(&st, zone::ZoneSquadRadiusFloor(true), NULL);
	std::vector<std::string> notes;
	ClampSettings(&st, saved, g_renderCfg, &DiscardLog, &notes);
	int z = ModuleFor(SQUAD);
	std::vector<std::string> applied;
	int n = ApplyLiveModuleRows(kConfigModules[z], st.module[z], &applied);
	Check(n == 1 && applied.size() == 1 && applied[0] == "zoneLifeSquadRadius=1" && zone::g_zoneCfg.cfg_zoneLifeSquadRadius == 1,
	      "close: the raised value applies live as zoneLifeSquadRadius=1");
	zone::g_zoneCfg = held;
}

static void CheckNoTarget()
{
	const ConfigFloor cases[] =
	{
		{ NULL, 1, "x" }, { "noSuchKey", 1, "x" }, { "preload", 1, "x" },
		{ "zoneLifeSquadRadius", 5, "x" }, { "zoneLifeSquadRadius", 0, "x" }, { "zoneLifeSquadRadius", -1, "x" }
	};
	const char* const names[] =
	{
		"a NULL key", "an unknown key", "a checkbox row", "a floor above the row's hi", "a floor at the row's lo",
		"a floor below the row's lo"
	};
	for (int c = 0; c < 6; ++c)
	{
		SettingsStaging st;
		Store(&st, 0);
		SettingsStaging before = st;
		std::vector<SettingsRow> plain = Rows(&st, true, NULL);
		std::vector<SettingsRow> rows = plain;
		bool raised = ApplySettingsFloor(&st, cases[c], &rows);
		bool same = memcmp(&st, &before, sizeof(st)) == 0;
		for (size_t r = 0; r < rows.size(); ++r)
			same = same && rows[r].lo == plain[r].lo && rows[r].tooltip == plain[r].tooltip;
		Check(!raised && same, (std::string("no target: ") + names[c] + " changes nothing").c_str());
	}
}

int main()
{
	CheckRule();
	const int stored[] = { 0, 1, 2 };
	for (int d = 0; d < 2; ++d)
	{
		for (int f = 0; f < 2; ++f)
		{
			for (int s = 0; s < 3; ++s)
				CheckRow(d == 0, f == 0, stored[s]);
		}
	}
	CheckClose();
	CheckLiveApply();
	CheckNoTarget();
	return CheckExit(SUITE_NAME);
}
