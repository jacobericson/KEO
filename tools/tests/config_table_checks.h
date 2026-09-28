#ifndef ZONEOPT_TOOLS_TESTS_CONFIG_TABLE_CHECKS_H
#define ZONEOPT_TOOLS_TESTS_CONFIG_TABLE_CHECKS_H

// The config key table's checks, shared by the PROD suite (config_table_units)
// and the DEV one (config_table_dev_units, ZONEOPT_DEBUG). The working folder
// is the repository root, so the template and the golden files are read from
// there.

#include "base/config_table.h"
#include "base/config_values.h"
#include "base/ini_text.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "bench/bench_slots.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "check.h"

namespace config_table_checks_detail
{

typedef std::pair<std::string, std::string> KeyValue;

std::vector<std::string>& Captured()
{
	static std::vector<std::string> lines;
	return lines;
}

void CaptureLog(const std::string& line)
{
	Captured().push_back(line);
}

void DiscardLog(const std::string&)
{
}

bool IsDev()
{
#ifdef ZONEOPT_DEBUG
	return true;
#else
	return false;
#endif
}

const ConfigModule* ModuleNamed(const char* name)
{
	for (int m = 0; m < kConfigModuleCount; ++m)
		if (strcmp(kConfigModules[m].name, name) == 0)
			return &kConfigModules[m];
	return NULL;
}

size_t TargetWidth(const ConfigKey& k)
{
	switch (k.kind)
	{
	case CK_BOOL:   return sizeof(bool);
	case CK_INT:    return sizeof(int);
	case CK_FLOAT:  return sizeof(float);
	case CK_DOUBLE: return sizeof(double);
	default:        return k.size;
	}
}

IniValueKind IniKindOf(ConfigKind kind)
{
	switch (kind)
	{
	case CK_BOOL:   return INI_BOOL;
	case CK_INT:    return INI_INT;
	case CK_FLOAT:
	case CK_DOUBLE: return INI_FLOAT;
	default:        return INI_TEXT;
	}
}

// The row's default for this build.
const char* BuildDefault(const ConfigKey& k)
{
	return IsDev() ? k.devDefault : k.prodDefault;
}

// Every target global, g_renderCfg and the bench slots, as static
// initialisation left them.
struct Snapshot
{
	std::vector<std::string> states;
	std::vector<std::string> targets;   // one per core row, empty when it has no target
	RenderConfig render;
	BenchSlot slots[BENCH_SLOT_COUNT];
};

Snapshot& Initial()
{
	static Snapshot s;
	return s;
}

void TakeSnapshot(const ConfigModule& core)
{
	Snapshot& s = Initial();
	s.targets.clear();
	s.states.clear();
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		s.states.push_back(mod.state ? std::string((const char*)mod.state, mod.stateSize) : std::string());
	}
	for (int i = 0; core.keys[i].name; ++i)
	{
		const ConfigKey& k = core.keys[i];
		if (k.target)
			s.targets.push_back(std::string((const char*)k.target, TargetWidth(k)));
		else
			s.targets.push_back(std::string());
	}
	s.render = g_renderCfg;
	memcpy(s.slots, g_benchSlots, sizeof(s.slots));
}

void RestoreSnapshot(const ConfigModule& core)
{
	const Snapshot& s = Initial();
	for (int m = 0; m < kConfigModuleCount; ++m)
		if (kConfigModules[m].state)
			memcpy(kConfigModules[m].state, s.states[m].data(), s.states[m].size());
	for (int i = 0; core.keys[i].name; ++i)
	{
		const ConfigKey& k = core.keys[i];
		if (k.target)
			memcpy(k.target, s.targets[i].data(), s.targets[i].size());
	}
	g_renderCfg = s.render;
	memcpy(g_benchSlots, s.slots, sizeof(s.slots));
}

bool TargetAtInitial(const ConfigModule& core, int i)
{
	const ConfigKey& k = core.keys[i];
	const std::string& want = Initial().targets[i];
	return memcmp(k.target, want.data(), want.size()) == 0;
}

// The golden value text: a bool as true/false, an int or enum in decimal, a
// float or double streamed as a double, text as its characters; a custom row
// by its target's width (one byte a bool, else an integer).
std::string GoldenValue(const ConfigKey& k, const void* p)
{
	std::ostringstream ss;
	switch (k.kind)
	{
	case CK_BOOL:   return *(const bool*)p ? "true" : "false";
	case CK_INT:    ss << *(const int*)p; break;
	case CK_FLOAT:  ss << (double)*(const float*)p; break;
	case CK_DOUBLE: ss << *(const double*)p; break;
	case CK_TEXT:   return std::string((const char*)p);
	default:
		if (k.size == 1)
			return *(const bool*)p ? "true" : "false";
		ss << *(const int*)p;
		break;
	}
	return ss.str();
}

std::string ReadFile(const char* path, bool* ok)
{
	std::ifstream f(path, std::ios::binary);
	*ok = f.is_open();
	if (!*ok)
		return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? text.size() : nl + 1;
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		out.push_back(line);
	}
	return out;
}

// The template's documented "# key=value" lines. The value may be empty
// (SplitIniLine drops such a line, so it is not used here).
std::vector<KeyValue> TemplateKeys(const std::string& text)
{
	std::vector<KeyValue> out;
	std::vector<std::string> lines = SplitLines(text);
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string t = IniTrim(lines[i]);
		if (t.empty() || t[0] != '#')
			continue;
		t = IniTrim(t.substr(1));
		size_t eq = t.find('=');
		if (eq == std::string::npos || eq == 0)
			continue;
		std::string key = IniTrim(t.substr(0, eq));
		bool name = !key.empty();
		for (size_t c = 0; c < key.size() && name; ++c)
		{
			char ch = key[c];
			name = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.';
		}
		if (name)
			out.push_back(KeyValue(key, IniTrim(t.substr(eq + 1))));
	}
	return out;
}

const char* const kUndocumented[] =
{
	"caching", "camLogInterval", "deferral", "destroyListDefer", "destroyListDiag", "gatePassDiag",
	"graphPositionGuard", "groupCohesion", "islandDeletedReissue", "islandFix",
	"islandReadinessRule", "movementAware", "navmeshDiskCacheMaxMB", "navmeshNeighbourSeeds",
	"navmeshVanillaPruning", "npcWaitDiag", "pathfindDiag", "preload", "preloadKeepAliveSeconds",
	"priorityBoost", "readinessOverrides", "saveLoadUnload", "zoneLifeIdleSeconds",
	"zoneLifeRetainRadius", "zoneLifeUnload", NULL
};

void CheckNamed(bool ok, const std::string& what)
{
	Check(ok, what.c_str());
}

// Parses text into a custom row's target and reports whether the target then
// holds its static-initialisation value; the target is restored either way.
bool CustomParsesToInitial(const ConfigModule& core, int i, const std::string& text)
{
	const ConfigKey& k = core.keys[i];
	std::string before((const char*)k.target, TargetWidth(k));
	Captured().clear();
	bool ok = ConfigApplyValue(core, k, text, &CaptureLog) && Captured().empty() && TargetAtInitial(core, i);
	memcpy(k.target, before.data(), before.size());
	return ok;
}

// ---- 1-3: the template and the defaults ----------------------------------

void CheckTemplateAndDefaults(const ConfigModule& core, const ConfigModule& render)
{
	bool ok = false;
	std::string text = ReadFile("KenshiZoneOpt.ini", &ok);
	Check(ok, "KenshiZoneOpt.ini found at the repository root");
	std::vector<KeyValue> tmpl = TemplateKeys(text);
	Check(tmpl.size() == 73, "template has 73 documented key lines");

	// 1. Every template line names a documented, active row...
	std::vector<std::string> missing;
	for (size_t i = 0; i < tmpl.size(); ++i)
	{
		const ConfigModule* m = NULL;
		const ConfigKey* k = FindConfigKey(tmpl[i].first, &m);
		if (!k)
		{
			missing.push_back(tmpl[i].first);
			continue;
		}
		CheckNamed(k->documented && !k->retired, "template names an undocumented row " + tmpl[i].first);
	}
	for (size_t i = 0; i < missing.size(); ++i)
		CheckNamed(false, "template line has no row " + missing[i]);
	// ...and every documented row has a line.
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		for (int i = 0; kConfigModules[m].keys[i].name; ++i)
		{
			const ConfigKey& k = kConfigModules[m].keys[i];
			if (k.retired || !k.documented)
				continue;
			bool found = false;
			for (size_t t = 0; t < tmpl.size() && !found; ++t)
				found = tmpl[t].first == k.name;
			CheckNamed(found, std::string("template has no line for row ") + k.name);
		}
	}

	// 2. The undocumented rows are exactly the known 25.
	{
		std::vector<std::string> undoc, want;
		for (int m = 0; m < kConfigModuleCount; ++m)
			for (int i = 0; kConfigModules[m].keys[i].name; ++i)
				if (!kConfigModules[m].keys[i].retired && !kConfigModules[m].keys[i].documented)
					undoc.push_back(kConfigModules[m].keys[i].name);
		for (int i = 0; kUndocumented[i]; ++i)
			want.push_back(kUndocumented[i]);
		std::sort(undoc.begin(), undoc.end());
		Check(undoc.size() == 25, "undocumented rows: 25");
		for (size_t i = 0; i < undoc.size(); ++i)
			CheckNamed(std::binary_search(want.begin(), want.end(), undoc[i]), "undocumented " + undoc[i]);
		for (size_t i = 0; i < want.size(); ++i)
			CheckNamed(std::binary_search(undoc.begin(), undoc.end(), want[i]), "undocumented " + want[i]);
	}

	// Every offset row starts at its module's constant defaults.
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		if (!mod.state) continue;
		CheckNamed(mod.defaults && memcmp(mod.state, mod.defaults, mod.stateSize) == 0,
		           std::string("default object ") + mod.name);
	}
	// The template describes PROD defaults; the probe fields use their DEV
	// defaults in both builds. A parser's storage is restored after comparison.
	for (size_t t = 0; t < tmpl.size(); ++t)
	{
		const ConfigModule* mod = NULL;
		const ConfigKey* k = FindConfigKey(tmpl[t].first, &mod);
		if (!k || !mod->state || (IsDev() && !k->debugOnlyReader)) continue;
		std::string what = "template default " + tmpl[t].first;
		if (k->kind == CK_CUSTOM)
		{
			std::string held((const char*)mod->state, mod->stateSize);
			bool parsed = ConfigApplyValue(*mod, *k, tmpl[t].second, &DiscardLog);
			CheckNamed(parsed && mod->defaults && ConfigOffsetValueEqual(*k, mod->state, mod->defaults), what);
			memcpy(mod->state, held.data(), held.size());
		}
		else CheckNamed(IniValueEquals(IniKindOf(k->kind), tmpl[t].second,
		                              ConfigFormatValue(*mod, *k, mod->defaults)), what);
	}
}

// ---- 4: the golden sample ------------------------------------------------

void CheckGoldenSample(const ConfigModule& core)
{
	RestoreSnapshot(core);
	const char* const sample[] =
	{
		"preload=false", "islandFarSpan=5", "camFocusHardMult=2.5", "camLogInterval=20",
		"stitchSourceLines=-1", "islandEdgeRing=observe", "k7PostDeathHold=observe",
		"clusterGraphBypass=player", "clusterGraphBypass=measure", "zoneGeometryMode=bogus",
		"particleStepCapSpeed=4", "bench.sweep=swamp:1", "squadPathCache=true", "foo=1",
		"preload=true", "sectionKeyProbe=false", "bench.swamp.hour=12", "bench.levers=nosuchLever",
		NULL
	};
	Captured().clear();
	ConfigLoadState st;
	for (int i = 0; sample[i]; ++i)
	{
		std::string key, val;
		if (SplitIniLine(sample[i], &key, &val))
			ConfigApplyLine(key, val, i + 1, &st, &CaptureLog);
	}

	Check(zone::g_zoneCfg.preloadEnabled == true, "sample preloadEnabled");
	Check(movement::g_movementCfg.cfg_islandFarSpan == 5, "sample cfg_islandFarSpan");
	Check(zone::g_zoneCfg.cfg_camFocusHardMult == 2.5f, "sample cfg_camFocusHardMult");
	Check(zone::g_zoneCfg.cfg_camLogInterval == 20.0, "sample cfg_camLogInterval");
	Check(fixes::g_fixesCfg.cfg_stitchSourceLines == 32, "sample cfg_stitchSourceLines");
	Check(movement::g_movementCfg.islandEdgeRingEnabled == false, "sample islandEdgeRingEnabled");
	Check(movement::g_movementCfg.cfg_k7PostDeathHold == K7_HOLD_OBSERVE, "sample cfg_k7PostDeathHold");
	Check(pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_MEASURE, "sample clusterGraphBypassMode");
	Check(g_renderCfg.particleStepCapSpeed == 4.0f, "sample g_renderCfg.particleStepCapSpeed");
	Check(g_benchSlots[0].hour == 12.0f, "sample g_benchSlots[0].hour");

	Check(st.overrides == (IsDev() ? 15 : 14), "sample overrides");
	Check(st.unrecognised == 2, "sample unrecognised");
	Check(st.retired == 1, "sample retired");
	Check(st.debugIgnored.size() == (IsDev() ? 0u : 1u), "sample DEV-only ignored");
	Check(ConfigSummaryLine(ConfigLoadState(), 0).empty(), "empty config summary");
	std::string summary = IsDev()
		? "Config: 15 setting(s) loaded from INI, 2 unrecognised, 1 retired, dup=2 (unrecognised listed above, left at their defaults)"
		: "Config: 14 setting(s) loaded from INI, 2 unrecognised, 1 retired, dup=2, 1 DEV-only ignored (unrecognised listed above, left at their defaults)";
	Check(ConfigSummaryLine(st, 2) == summary, "sample summary");

	std::vector<std::string> want;
	want.push_back("Config: unknown or invalid key 'stitchSourceLines'");
	want.push_back("Config: islandEdgeRing=observe is a retired value; read as false");
	want.push_back("Config: zoneGeometryMode 'bogus' refused; only contentOnly is available");
	want.push_back("Config: retired key 'squadPathCache' ignored");
	want.push_back("Config: unknown or invalid key 'foo'");
	if (!IsDev())
		want.push_back("Config: DEV-only key 'sectionKeyProbe' ignored in this build");
	want.push_back("Bench: unknown lever 'nosuchLever' in bench.levers, ignored");
	want.push_back("Bench: bench.levers named no known lever, using every lever");
	Check(Captured() == want, "sample captured lines");
	for (size_t i = 0; i < Captured().size(); ++i)
		std::printf("  sample log: %s\n", Captured()[i].c_str());

	int dups = 0;
	bool preloadDup = false, cgbDup = false;
	for (size_t i = 0; i < st.dupSeen.size(); ++i)
	{
		const IniDupSeen& d = st.dupSeen[i];
		if (d.firstLine == d.lastLine)
			continue;
		++dups;
		if (d.key == "preload" && d.firstLine == 1 && d.lastLine == 15) preloadDup = true;
		if (d.key == "clusterGraphBypass" && d.firstLine == 8 && d.lastLine == 9) cgbDup = true;
	}
	Check(dups == 2 && preloadDup && cgbDup, "sample duplicates: preload 1..15, clusterGraphBypass 8..9");

	RestoreSnapshot(core);
}

// ---- 4b: the last value wins ---------------------------------------------

// preload's default is true, so a reader that applied no line, or kept the
// first, would still read true here.
void CheckLastValueWins(const ConfigModule& core)
{
	RestoreSnapshot(core);
	Check(zone::g_zoneCfg.preloadEnabled == true, "last value wins: preload's default is true");
	const char* const text[] = { "preload=true", "preload=false", NULL };
	Captured().clear();
	ConfigLoadState st;
	for (int i = 0; text[i]; ++i)
	{
		std::string key, val;
		if (SplitIniLine(text[i], &key, &val))
			ConfigApplyLine(key, val, i + 1, &st, &CaptureLog);
	}
	Check(zone::g_zoneCfg.preloadEnabled == false, "last value wins: preload=true then preload=false reads false");
	bool dup = false;
	for (size_t i = 0; i < st.dupSeen.size(); ++i)
		if (st.dupSeen[i].key == "preload" && st.dupSeen[i].firstLine == 1 && st.dupSeen[i].lastLine == 2)
			dup = true;
	Check(dup && st.overrides == 2 && Captured().empty(),
	      "last value wins: preload 1..2 is one duplicate, two overrides, no log line");
	RestoreSnapshot(core);
}

// ---- 5: the clamps -------------------------------------------------------

void CheckClamps(const ConfigModule& core)
{
	RestoreSnapshot(core);
	navmesh::g_navmeshCfg.cfg_navmeshWorkerCount = 99;
	zone::g_zoneCfg.cfg_camFocusMaxDist = 100.0f;
	zone::g_zoneCfg.cfg_camFocusHardMult = 0.5f;
	Captured().clear();
	ConfigClampLoaded(&CaptureLog);
	std::vector<std::string> want;
	want.push_back("Config: navmeshWorkerCount=99 clamped to max 6");
	want.push_back("Config: camFocusMaxDist=100 clamped to min 500");
	want.push_back("Config: camFocusHardMult=0.5 clamped to min 1");
	Check(Captured() == want, "clamp lines");
	Check(navmesh::g_navmeshCfg.cfg_navmeshWorkerCount == 6 && zone::g_zoneCfg.cfg_camFocusMaxDist == 500.0f && zone::g_zoneCfg.cfg_camFocusHardMult == 1.0f,
	      "clamped values");

	const float unclamped[] = { 0.0f, -5.0f };
	for (int i = 0; i < 2; ++i)
	{
		RestoreSnapshot(core);
		zone::g_zoneCfg.cfg_camFocusMaxDist = unclamped[i];
		Captured().clear();
		ConfigClampLoaded(&CaptureLog);
		bool line = false;
		for (size_t j = 0; j < Captured().size(); ++j)
			line = line || Captured()[j].find("camFocusMaxDist") != std::string::npos;
		Check(!line && zone::g_zoneCfg.cfg_camFocusMaxDist == unclamped[i], "camFocusMaxDist at or below 0 is not clamped");
	}

	// Each bound, from the other side.
	RestoreSnapshot(core);
	zone::g_zoneCfg.cfg_preloadKeepAliveSeconds = 90000.0f;
	navmesh::g_navmeshCfg.cfg_navmeshGenConcurrency = -1;
	navmesh::g_navmeshCfg.cfg_navmeshDiskCacheMaxMB = 9000;
	zone::g_zoneCfg.cfg_camLogInterval = 0.5;
	navmesh::g_navmeshCfg.cfg_reprioritizeInterval = 31.0;
	zone::g_zoneCfg.cfg_camFocusMaxDist = 60000.0f;
	zone::g_zoneCfg.cfg_camFocusHardMult = 11.0f;
	zone::g_zoneCfg.cfg_camFocusHysteresis = -1.0f;
	zone::g_zoneCfg.cfg_zoneLifeRetainRadius = 5;
	movement::g_movementCfg.cfg_islandFarSpan = 9;
	zone::g_zoneCfg.cfg_zoneLifeIdleSeconds = 4.0;
	Captured().clear();
	ConfigClampLoaded(&CaptureLog);
	want.clear();
	want.push_back("Config: preloadKeepAliveSeconds=90000 clamped to max 86400");
	want.push_back("Config: navmeshGenConcurrency=-1 clamped to min 0");
	want.push_back("Config: navmeshDiskCacheMaxMB=9000 clamped to max 8192");
	want.push_back("Config: camLogInterval=0.5 clamped to min 1");
	want.push_back("Config: reprioritizeInterval=31 clamped to max 30");
	want.push_back("Config: camFocusMaxDist=60000 clamped to max 50000");
	want.push_back("Config: camFocusHardMult=11 clamped to max 10");
	want.push_back("Config: camFocusHysteresis=-1 clamped to min 0");
	want.push_back("Config: zoneLifeRetainRadius=5 clamped to max 4");
	want.push_back("Config: islandFarSpan=9 clamped to max 8");
	want.push_back("Config: zoneLifeIdleSeconds=4 clamped to min 5");
	Check(Captured() == want, "every other clamp bound, in order");
	for (size_t j = 0; j < Captured().size(); ++j)
		std::printf("  clamp log: %s\n", Captured()[j].c_str());
	RestoreSnapshot(core);
}

// ---- 6: the writer -------------------------------------------------------

bool SameEntries(const std::vector<IniEntry>& a, const std::vector<IniEntry>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].key != b[i].key || a[i].value != b[i].value || a[i].kind != b[i].kind || a[i].append != b[i].append)
			return false;
	return true;
}

void CheckWriter(const ConfigModule& render)
{
	const RenderConfig& defaults = RenderConfigDefaults();
	RenderConfig cfg = defaults;
	for (int pass = 0; pass < 2; ++pass)
	{
		if (pass == 1)
		{
			cfg.particleStepCap = !cfg.particleStepCap;
			cfg.particleStepCapSpeed = 4.25f;
			cfg.particleOffscreenSeconds = 0.1f;
			strcpy_s(cfg.particleLoopingNames, sizeof(cfg.particleLoopingNames), "torch");
		}
		std::vector<IniEntry> viaModule, viaRender;
		ConfigIniEntries(render, &cfg, &defaults, &viaModule);
		RenderIniEntries(cfg, defaults, &viaRender);
		Check(viaModule.size() == 22 && SameEntries(viaModule, viaRender),
		      pass == 0 ? "writer: render module entries equal RenderIniEntries (defaults)"
		                : "writer: render module entries equal RenderIniEntries (changed)");
	}
}

// ---- 6b: the writer over the core module's target rows -------------------

void CheckCoreWriter(const ConfigModule& core)
{
	RestoreSnapshot(core);
	std::vector<IniEntry> e;
	for (int m = 0; m < kConfigModuleCount; ++m)
		if (kConfigModules[m].state && strcmp(kConfigModules[m].name, "render"))
			ConfigIniEntries(kConfigModules[m], kConfigModules[m].state, kConfigModules[m].defaults, &e);
	Check(e.size() == 73u,
	      "writer: core entries, every active row but the custom rows without choices");
	bool appends = false;
	for (size_t i = 0; i < e.size(); ++i)
		appends = appends || e[i].append;
	Check(!appends, "writer: core entries at the defaults append nothing");

	zone::g_zoneCfg.preloadEnabled = false;
	movement::g_movementCfg.cfg_islandFarSpan = 5;
	pathfind::g_pathfindCfg.clusterGraphBypassMode = CGB_MEASURE;
	e.clear();
	for (int m = 0; m < kConfigModuleCount; ++m)
		if (kConfigModules[m].state && strcmp(kConfigModules[m].name, "render"))
			ConfigIniEntries(kConfigModules[m], kConfigModules[m].state, kConfigModules[m].defaults, &e);
	int changed = 0;
	bool preload = false, span = false, cgb = false;
	for (size_t i = 0; i < e.size(); ++i)
	{
		if (!e[i].append)
			continue;
		++changed;
		if (e[i].key == "preload" && e[i].value == "false" && e[i].kind == INI_BOOL) preload = true;
		if (e[i].key == "islandFarSpan" && e[i].value == "5" && e[i].kind == INI_INT) span = true;
		if (e[i].key == "clusterGraphBypass" && e[i].value == "measure" && e[i].kind == INI_TEXT) cgb = true;
	}
	Check(changed == 3 && preload && span && cgb,
	      "writer: core entries append exactly the three changed rows, with their INI text");
	RestoreSnapshot(core);
}

// ---- 7: the golden record ------------------------------------------------

void CheckGoldenRecord(const ConfigModule& core)
{
	RestoreSnapshot(core);
	bool ok = false;
	std::string sample = ReadFile("tools\\tests\\config_golden\\sample.ini", &ok);
	Check(ok, "golden sample.ini found");
	std::vector<std::string> lines = SplitLines(sample);
	Check(lines.size() == 18, "golden sample.ini has 18 lines");
	ConfigLoadState st;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string key, val;
		if (SplitIniLine(lines[i], &key, &val))
			ConfigApplyLine(key, val, (int)i + 1, &st, &DiscardLog);
	}

	std::vector<KeyValue> got;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		for (int i = 0; mod.keys[i].name; ++i)
		{
			const ConfigKey& k = mod.keys[i];
			if (k.retired)
				continue;
			const void* p = k.target ? k.target : (mod.state ? (const char*)mod.state + k.offset : NULL);
			if (p)
				got.push_back(KeyValue(k.name, GoldenValue(k, p)));
		}
	}
	std::sort(got.begin(), got.end());

	const char* path = IsDev() ? "tools\\tests\\config_golden\\dev.txt" : "tools\\tests\\config_golden\\prod.txt";
	std::string file = ReadFile(path, &ok);
	Check(ok, "golden record file found");
	std::vector<KeyValue> want;
	std::vector<std::string> fl = SplitLines(file);
	for (size_t i = 0; i < fl.size(); ++i)
	{
		size_t eq = fl[i].find('=');
		if (!fl[i].empty() && eq != std::string::npos)
			want.push_back(KeyValue(fl[i].substr(0, eq), fl[i].substr(eq + 1)));
	}
	Check(want.size() == 96u, "golden record key count");
	CheckNamed(got.size() == want.size(), "golden count");
	for (size_t i = 0; i < got.size(); ++i)
	{
		bool found = false;
		for (size_t j = 0; j < want.size() && !found; ++j)
		{
			if (want[j].first != got[i].first)
				continue;
			found = true;
			CheckNamed(want[j].second == got[i].second, "golden " + got[i].first);
		}
		CheckNamed(found, "golden count " + got[i].first + " (extra)");
	}
	for (size_t j = 0; j < want.size(); ++j)
	{
		bool found = false;
		for (size_t i = 0; i < got.size() && !found; ++i)
			found = got[i].first == want[j].first;
		CheckNamed(found, "golden count " + want[j].first + " (missing)");
	}
	RestoreSnapshot(core);
}

// ---- 8: the tables' ends -------------------------------------------------

void CheckTables()
{
	int active = 0, retired = 0, coreActive = 0, renderActive = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		int i = 0;
		for (; mod.keys[i].name; ++i)
		{
			if (mod.keys[i].retired)
				++retired;
			else
			{
				++active;
				if (strcmp(mod.name, "core") == 0) ++coreActive;
				if (strcmp(mod.name, "render") == 0) ++renderActive;
			}
		}
		Check(mod.keys[i].name == NULL, "tables end");
	}
	Check(kConfigModuleCount == 8, "tables end: eight modules");
	Check(active == 98 && retired == 22, "tables end: active and retired rows");
	Check(coreActive == 2 && renderActive == 22, "tables end: rows per module");
	std::printf("  tables: %d module(s), %d active row(s) (core %d, render %d), %d retired\n",
	            kConfigModuleCount, active, coreActive, renderActive, retired);
}

} // namespace config_table_checks_detail

inline int RunConfigTableChecks(const char* suite)
{
	using namespace config_table_checks_detail;
	const ConfigModule* core = ModuleNamed("core");
	const ConfigModule* render = ModuleNamed("render");
	Check(core && render, "the core and render modules exist");
	if (!core || !render)
		return CheckExit(suite);
	Check(render->keys == g_renderKeys && render->state == &g_renderCfg &&
	      render->defaults == &RenderConfigDefaults() && render->stateSize == sizeof(RenderConfig),
	      "render module is g_renderKeys over g_renderCfg");
	Check(core->state == NULL && core->defaults == NULL && core->stateSize == 0, "core module has no state");

	TakeSnapshot(*core);
	CheckTemplateAndDefaults(*core, *render);
	CheckGoldenSample(*core);
	CheckLastValueWins(*core);
	CheckClamps(*core);
	CheckWriter(*render);
	CheckCoreWriter(*core);
	CheckGoldenRecord(*core);
	CheckTables();
	return CheckExit(suite);
}

#endif
