#include "gui/settings_rows.h"
#include "gui/settings_layout.h"
#include "render/render_keys.h"
#include <cstdio>
#include <cstring>
#include <sstream>

static_assert(sizeof(RenderConfig) <= CONFIG_STATE_MAX, "the render module's state fits its staged copy");

const char* const RESTART_MARK = " *";
const char* const RESTART_NOTE = "Takes effect after restarting the game.";

static SettingsRow Row(SettingsRowKind kind, const std::string& label, const char* tooltip)
{
	SettingsRow r;
	r.kind = kind;
	r.label = label;
	r.tooltip = tooltip ? tooltip : "";
	r.boolPtr = NULL;
	r.floatPtr = NULL;
	r.intPtr = NULL;
	r.lo = r.hi = 0.0f;
	r.stepExp = 0;
	r.buttonId = 0;
	r.restart = false;
	return r;
}

static std::string IntText(int v)
{
	std::ostringstream ss;
	ss << v;
	return ss.str();
}

// "about 10 minutes, 2 with bench.levers=combined"; the second part only
// when it differs.
static std::string Duration(double sec, double combinedSec)
{
	int m = (int)(sec / 60.0 + 0.5), c = (int)(combinedSec / 60.0 + 0.5);
	if (m < 1) m = 1;
	if (c < 1) c = 1;
	std::string s = "about " + IntText(m) + (m == 1 ? " minute" : " minutes");
	if (c != m)
		s += ", " + IntText(c) + " with bench.levers=combined";
	return s;
}

static SettingsRow Button(const std::string& label, const char* caption, int id, const char* tooltip)
{
	SettingsRow r = Row(SR_BUTTON, label, tooltip);
	r.caption = caption;
	r.buttonId = id;
	return r;
}

static std::string SlotState(int slot, const BenchSlot& s)
{
	if (!s.recorded)
		return std::string(BenchSlotLabel(slot)) + ": not recorded";
	char buf[64];
	sprintf_s(buf, sizeof(buf), "%s: speed %dx, hour %.1f", BenchSlotLabel(slot), s.speed, s.hour);
	return buf;
}

static void AddBenchRows(SettingsStaging* staging, const SettingsBench& bench, std::vector<SettingsRow>* out)
{
	out->push_back(Row(SR_HEADER, "Benchmark", NULL));
	if (!bench.available || !bench.slots)
	{
		out->push_back(Row(SR_TEXT, "Benchmark unavailable (" + bench.reason + ")", NULL));
		return;
	}
	bool timed = bench.runSec > 0.0;
	std::string runTime = timed ? " (" + Duration(bench.runSec, bench.runSecCombined) + ")" : "";
	std::string sweepTime = timed ? " Its " + IntText(bench.sweepLegCount) + " legs take " +
		Duration(bench.sweepLegCount * bench.runSec, bench.sweepLegCount * bench.runSecCombined) +
		", plus any wait for zones to load between legs." : "";
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
	{
		std::string name = BenchSlotLabel(i);
		out->push_back(Row(SR_TEXT, SlotState(i, bench.slots[i]), NULL));

		SettingsRow speed = Row(SR_DROPBOX, name + " speed",
			"Game speed during the run. Record here and Run store it for this spot.");
		speed.intPtr = &staging->benchSpeed[i];
		speed.choices.push_back(std::make_pair(std::string("1x"), 1));
		speed.choices.push_back(std::make_pair(std::string("20x"), 20));
		out->push_back(speed);

		out->push_back(Button(name + " camera", "Record here", BENCH_BUTTON_RECORD + i,
			"Stores the camera's position, angle and zoom, the game hour and the chosen speed for this spot."
			" A player character must stay within reach of it for a run. Pressing it while a run is armed stops"
			" that run instead."));
		out->push_back(Button(name + " benchmark", i == bench.activeSlot ? "Stop" : "Run", BENCH_BUTTON_RUN + i,
			("Runs the benchmark at this spot once Options and the menu are closed and the game is unpaused" +
			 runTime + "; the result goes to KEO.log. Pressing it, or another spot's Run,"
			 " while a run is armed or running stops that run.").c_str()));
	}
	out->push_back(Button("Sweep", SweepCaption(bench.sweepLeg, bench.sweepLegs).c_str(), BENCH_BUTTON_SWEEP,
		("Runs the benchmark at each leg of bench.sweep in turn (by default Swamp, City and Sand, each at 1x"
		 " then 20x); every spot must be recorded, with a player character within reach." + sweepTime +
		 " Each leg writes its own result to KEO.log. Pressing any benchmark button while it runs"
		 " stops it.").c_str()));
}

std::string SweepCaption(int leg, int legs)
{
	if (leg <= 0)
		return "Full sweep";
	char buf[48];
	sprintf_s(buf, sizeof(buf), "Sweep %d/%d (stop)", leg, legs);
	return buf;
}

int RenderModuleIndex()
{
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		if (strcmp(kConfigModules[m].name, "render") == 0)
			return m;
	}
	return -1;
}

RenderConfig& StagedRender(SettingsStaging* s)
{
	static RenderConfig none;
	int r = RenderModuleIndex();
	return r >= 0 ? *(RenderConfig*)s->module[r].state : none;
}

void BuildSettingsRows(SettingsStaging* staging, bool devBuild, const SettingsBench* bench,
                       std::vector<SettingsRow>* out)
{
	if (devBuild)
	{
		for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
			AddModuleRows(kConfigModules[m], &staging->module[m], true, out);
	}
	else
		keo_gui::AddPlayerSections(keo_gui::kSettingsPlaces, staging, out);

	for (size_t i = 0; i < out->size(); ++i)
	{
		if ((*out)[i].restart)
		{
			out->push_back(Row(SR_NOTE, std::string("* ") + RESTART_NOTE, NULL));
			break;
		}
	}

	if (bench && devBuild)
		AddBenchRows(staging, *bench, out);
}

void StageBenchSpeeds(SettingsStaging* staging, const BenchSlot* slots)
{
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		staging->benchSpeed[i] = slots[i].speed == 20 ? 20 : 1;
}

SettingsDiff DiffSettings(const SettingsStaging& staged, const RenderConfig& live,
                          const SettingsStaging& saved)
{
	SettingsDiff d;
	d.applied = 0;
	d.saved = 0;
	int r = RenderModuleIndex();
	if (r >= 0)
	{
		const RenderConfig& render = *(const RenderConfig*)staged.module[r].state;
		for (int i = 0; g_renderKeys[i].name; ++i)
		{
			const RenderKey& k = g_renderKeys[i];
			if (k.live && !RenderValueEqual(render, live, k))
				++d.applied;
		}
	}
	std::vector<IniEntry> entries;
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		if (m != r)
			d.applied += LiveModuleRowsDiffering(kConfigModules[m], staged.module[m]);
		d.saved += ModuleStageEntries(kConfigModules[m], staged.module[m], saved.module[m], &entries);
	}
	return d;
}

void ClampSettings(SettingsStaging* staged, const SettingsStaging& saved, const RenderConfig& fallback,
                   ConfigLogFn log, std::vector<std::string>* renderNotes)
{
	int r = RenderModuleIndex();
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		if (m != r)
			ClampModuleStage(kConfigModules[m], &staged->module[m], saved.module[m], log);
	}
	if (r >= 0)
		ClampRenderValues(&StagedRender(staged), fallback, renderNotes);
}

// The module and key index of the row f names, when it is a slot-bound
// whole-number slider the floor lies inside; false otherwise.
static bool FloorTarget(const ConfigFloor& f, int* module, int* key)
{
	if (!f.key)
		return false;
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; i < CONFIG_STAGE_MAX && mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (k.retired || strcmp(k.name, f.key) != 0)
				continue;
			if (!mod.state || k.target || k.kind != CK_INT || k.choices || !k.label || k.lo > k.hi
			    || f.floor <= (int)k.lo || f.floor > (int)k.hi)
				return false;
			*module = m;
			*key = i;
			return true;
		}
	}
	return false;
}

bool ApplySettingsFloor(SettingsStaging* staging, const ConfigFloor& f, std::vector<SettingsRow>* rows)
{
	int m = -1;
	int i = -1;
	if (!FloorTarget(f, &m, &i))
		return false;
	const ConfigKey& k = kConfigModules[m].keys[i];
	ConfigModuleStage& s = staging->module[m];
	float lo = (float)f.floor;
	bool raised = !(s.slots[i].f >= lo);
	if (raised)
	{
		s.slots[i].f = lo;
		*(int*)((char*)s.state + k.offset) = f.floor;
	}
	if (rows)
	{
		for (size_t r = 0; r < rows->size(); ++r)
		{
			SettingsRow& row = (*rows)[r];
			if (row.kind != SR_SLIDER || row.floatPtr != &s.slots[i].f)
				continue;
			row.lo = lo;
			if (f.note && *f.note)
				row.tooltip += row.tooltip.empty() ? std::string(f.note) : std::string(" ") + f.note;
		}
	}
	return raised;
}
