#include "bench/bench_group.h"
#include "bench/bench_sweep.h"
#include "bench/bench_recorders.h"
#include "gui/settings_factory.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include <windows.h>
#include <sstream>
#include <stdlib.h>
#include <string.h>

namespace bench_group_detail {

const int VALUE_MAX = 256;   // the widest field a lever writes: the render text
const int SET_OFF = -2;      // a set's lever: none
const int SET_ALL = -1;      // a set's lever: every one of the group's

struct Lever
{
	const ConfigModule* module;
	const ConfigKey*    key;
	unsigned char       bytes[VALUE_MAX];          // the field as the lever's sets write it
	char                text[VALUE_MAX + 64];      // "key=value", the value read back
};

struct Group
{
	char  name[16];
	int   sets;
	int   passes;
	int   discardSec, measureSec;
	int   devOnlyDrops;
	int   leverCount;
	Lever levers[BENCH_GROUP_LEVERS];
};

// A key some group names. Every group run holds all of them: each sits at its
// off value unless the current set turns it on.
struct Held
{
	const ConfigModule* module;
	const ConfigKey*    key;
	unsigned char       off[VALUE_MAX];    // unused for a text key: its off is the user's
	unsigned char       user[VALUE_MAX];   // taken at the run's first apply
};

// One run at a time, so one context; allocated at the first build.
struct GroupRun
{
	int               group;
	bool              holding;   // the user's values are taken and the run's applied
	int               setCount;
	int               setLever[BENCH_GROUP_LEVERS + 1];   // SET_OFF, SET_ALL or a lever index
	FrameTimeRecorder frames;
	DriftRecorder     drift;
	ZoneEventRecorder zones;
};

} // namespace bench_group_detail
using namespace bench_group_detail;

static Group             s_groups[BENCH_GROUP_MAX];
static int               s_groupCount = 0;
static Held              s_held[BENCH_GROUP_HELD];
static int               s_heldCount = 0;
static ConfigLogFn       s_log = NULL;
static GroupRun*         s_run = NULL;
static ConfigModuleStage s_stage;   // main thread only

static void Log(const std::string& line)
{
	if (s_log)
		s_log(line);
}

static long long Qpc()
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

static std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

static bool IsRender(const ConfigModule* m)
{
	return m->state == (void*)&g_renderCfg;
}

// The bytes a row's field takes; render rows leave size 0 but for text.
static size_t FieldWidth(const ConfigKey& k)
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

static bool BytesEqual(const ConfigKey& k, const unsigned char* a, const unsigned char* b)
{
	if (k.kind == CK_TEXT)
		return strcmp((const char*)a, (const char*)b) == 0;
	if (k.kind == CK_BOOL)
		return (*a != 0) == (*b != 0);
	return memcmp(a, b, FieldWidth(k)) == 0;
}

// A choice's value at the row's width, as ConfigFormatValue reads it back.
static void WriteChoice(const ConfigKey& k, int v, unsigned char* out)
{
	if (k.kind == CK_CUSTOM && k.size == 1)
		*out = v ? 1 : 0;
	else
		memcpy(out, &v, sizeof(v));
}

static void OffBytes(const ConfigKey& k, unsigned char* out)
{
	memset(out, 0, VALUE_MAX);
	if (k.choices && k.choiceCount > 0)
	{
		WriteChoice(k, k.choices[0].value, out);
		return;
	}
	switch (k.kind)
	{
	case CK_INT:
	{
		int v = 0 < k.minInt ? k.minInt : 0;
		ConfigClampValue(k, &v, NULL);
		memcpy(out, &v, sizeof(v));
		break;
	}
	case CK_FLOAT:
	{
		float v = 0.0f;
		ConfigClampValue(k, &v, NULL);
		memcpy(out, &v, sizeof(v));
		break;
	}
	case CK_DOUBLE:
	{
		double v = 0.0;
		ConfigClampValue(k, &v, NULL);
		memcpy(out, &v, sizeof(v));
		break;
	}
	default:   // a bool's off is false; a text row's is the user's own
		break;
	}
}

// A choice row's value comes from its table, never from its parser (which
// writes the running config); any other through a scratch copy of the state.
static bool ResolveValue(const ConfigModule& m, const ConfigKey& k, const std::string& val, unsigned char* out)
{
	memset(out, 0, VALUE_MAX);
	if (k.kind == CK_CUSTOM)
	{
		for (int i = 0; i < k.choiceCount; ++i)
		{
			if (_stricmp(k.choices[i].ini, val.c_str()) == 0)
			{
				WriteChoice(k, k.choices[i].value, out);
				return true;
			}
		}
		return false;
	}
	double scratch[CONFIG_STATE_MAX / sizeof(double)];
	memcpy(scratch, m.state, m.stateSize);
	ConfigModule copy = m;
	copy.state = scratch;
	char* field = (char*)scratch + k.offset;
	if (!ConfigApplyValue(copy, k, val, s_log) || ConfigClampValue(k, field, s_log) == CLAMP_REFUSED)
		return false;
	memcpy(out, field, FieldWidth(k));
	return true;
}

static std::string ValueText(const ConfigModule& m, const ConfigKey& k, const unsigned char* bytes)
{
	double scratch[CONFIG_STATE_MAX / sizeof(double)];
	memcpy(scratch, m.state, m.stateSize);
	memcpy((char*)scratch + k.offset, bytes, FieldWidth(k));
	return ConfigFormatValue(m, k, scratch);
}

// NULL when a lever may switch the row.
static const char* RowRefused(const ConfigModule& m, const ConfigKey& k)
{
	if (k.retired)
		return "is retired";
	if (!k.live)
		return "is startup-only";
	if (k.target || !m.state)
		return "has no field in a module's state";
	if (k.kind == CK_TEXT && !IsRender(&m))
		return "is a text key outside the render module";
	if (k.kind == CK_CUSTOM && (!k.choices || k.choiceCount <= 0))
		return "has no choice list";
	if (k.kind == CK_CUSTOM && k.size != 1 && k.size != sizeof(int))
		return "has no choice width";
	if (FieldWidth(k) > (size_t)VALUE_MAX || m.stateSize > (size_t)CONFIG_STATE_MAX)
		return "is too wide to hold";
	return NULL;
}

static int HeldIndex(const ConfigKey* k)
{
	for (int h = 0; h < s_heldCount; ++h)
	{
		if (s_held[h].key == k)
			return h;
	}
	return -1;
}

static bool ParseWhole(const std::string& s, int lo, int hi, int* out)
{
	if (s.empty() || s.size() > 4)
		return false;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
	}
	int v = atoi(s.c_str());
	if (v < lo || v > hi)
		return false;
	*out = v;
	return true;
}

// <sets>/<passes>/<discard>+<measure>; NULL when it parses.
static const char* ParseHead(const std::string& head, Group* g)
{
	size_t a = head.find('/');
	size_t b = a == std::string::npos ? a : head.find('/', a + 1);
	if (b == std::string::npos)
		return "the head is <sets>/<passes>/<discard>+<measure>";
	std::string sets = Trim(head.substr(0, a));
	std::string window = Trim(head.substr(b + 1));
	if (sets == "each")
		g->sets = BGS_EACH;
	else if (sets == "all")
		g->sets = BGS_ALL;
	else if (sets == "only")
		g->sets = BGS_ONLY;
	else
		return "the sets are each, all or only";
	if (!ParseWhole(Trim(head.substr(a + 1, b - a - 1)), 1, 8, &g->passes))
		return "the passes are 1-8";
	size_t plus = window.find('+');
	if (plus == std::string::npos)
		return "the window is <discard>+<measure>";
	if (!ParseWhole(Trim(window.substr(0, plus)), 0, 60, &g->discardSec))
		return "the discard is 0-60 s";
	if (!ParseWhole(Trim(window.substr(plus + 1)), 5, 300, &g->measureSec))
		return "the measure is 5-300 s";
	return NULL;
}

// Why the lever is dropped (empty: kept); a repeat, the lever cap or the held cap.
static std::string GroupRefuses(const Group& g, const ConfigKey* k, const unsigned char* bytes)
{
	for (int i = 0; i < g.leverCount; ++i)
	{
		if (g.levers[i].key != k)
			continue;
		if (g.sets == BGS_ALL)
			return "repeats a key in an all group";
		if (BytesEqual(*k, g.levers[i].bytes, bytes))
			return "repeats";
	}
	if (g.leverCount >= BENCH_GROUP_LEVERS)
		return "(past 16)";
	if (HeldIndex(k) < 0 && s_heldCount >= BENCH_GROUP_HELD)
		return "(past 32 held keys)";
	return "";
}

// One key=value of a group's list: kept as a lever, or dropped with one line.
// The active row named key, else a retired row of that name (so its drop says
// so); NULL when neither exists.
static const ConfigKey* FindLeverKey(const std::string& key, const ConfigModule** module)
{
	const ConfigKey* k = FindConfigKey(key, module);
	for (int m = 0; !k && m < kConfigModuleCount; ++m)
	{
		for (int i = 0; kConfigModules[m].keys[i].name; ++i)
		{
			if (kConfigModules[m].keys[i].retired && key == kConfigModules[m].keys[i].name)
			{
				*module = &kConfigModules[m];
				return &kConfigModules[m].keys[i];
			}
		}
	}
	return k;
}

static void ResolveLever(Group& g, const std::string& text)
{
	size_t eq = text.find('=');
	std::string key = Trim(eq == std::string::npos ? text : text.substr(0, eq));
	std::string val = eq == std::string::npos ? "" : Trim(text.substr(eq + 1));
	const ConfigModule* m = NULL;
	const ConfigKey* k = FindLeverKey(key, &m);
	Lever& lv = g.levers[g.leverCount < BENCH_GROUP_LEVERS ? g.leverCount : BENCH_GROUP_LEVERS - 1];
	unsigned char bytes[VALUE_MAX];
	std::string why;
	const char* refused = NULL;
	if (!k)
		why = "is unknown";
#ifndef KEO_DEBUG
	else if (k->debugOnlyReader)
	{
		++g.devOnlyDrops;
		return;
	}
#endif
	else if ((refused = RowRefused(*m, *k)) != NULL)
		why = refused;
	else if (eq == std::string::npos || !ResolveValue(*m, *k, val, bytes))
		why = "does not take '" + val + "'";
	else
	{
		unsigned char off[VALUE_MAX];
		OffBytes(*k, off);
		if (k->kind == CK_TEXT ? strcmp((const char*)bytes, (const char*)m->state + k->offset) == 0
		                       : BytesEqual(*k, bytes, off))
			why = "equals its off value";
		else
			why = GroupRefuses(g, k, bytes);
	}
	if (!why.empty())
	{
		Log("Bench: group " + std::string(g.name) + ": " + key + " " + why + ", dropped");
		return;
	}
	lv.module = m;
	lv.key = k;
	memcpy(lv.bytes, bytes, sizeof(lv.bytes));
	_snprintf_s(lv.text, sizeof(lv.text), _TRUNCATE, "%s=%s", k->name, ValueText(*m, *k, bytes).c_str());
	++g.leverCount;
	if (HeldIndex(k) < 0)
	{
		Held& h = s_held[s_heldCount++];
		h.module = m;
		h.key = k;
		OffBytes(*k, h.off);
		memset(h.user, 0, sizeof(h.user));
	}
}

// One stored bench.group text into s_groups; false when its head is refused.
static bool ResolveGroup(const char* name, const std::string& text, Group* g)
{
	memset(g, 0, sizeof(*g));
	_snprintf_s(g->name, sizeof(g->name), _TRUNCATE, "%s", name);
	size_t colon = text.find(':');
	const char* why = colon == std::string::npos ? "the levers follow a ':'" : ParseHead(text.substr(0, colon), g);
	if (why)
	{
		Log("Bench: group " + std::string(name) + " refused (" + why + ")");
		return false;
	}
	std::string list = text.substr(colon + 1);
	size_t pos = 0;
	while (pos <= list.size())
	{
		size_t semi = list.find(';', pos);
		std::string item = Trim(semi == std::string::npos ? list.substr(pos) : list.substr(pos, semi - pos));
		if (!item.empty())
			ResolveLever(*g, item);
		if (semi == std::string::npos)
			break;
		pos = semi + 1;
	}
	return true;
}

static std::string SetName(const Lever& lv)
{
	std::string s = lv.text;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == ' ')
			s[i] = '_';
	}
	return s;
}

static const char* SetsWord(int sets)
{
	return sets == BGS_ALL ? "all" : sets == BGS_ONLY ? "only" : "each";
}

static void LogGroupLine(const Group& g)
{
	std::ostringstream ss;
	ss << "Bench: group " << g.name;
	if (g.leverCount == 0)
	{
		ss << (g.devOnlyDrops > 0 ? " empty (DEV-only levers)" : " empty");
		Log(ss.str());
		return;
	}
	ss << " sets=" << SetsWord(g.sets) << " passes=" << g.passes << " window=" << g.discardSec << "+" << g.measureSec
	   << " levers=";
	for (int i = 0; i < g.leverCount; ++i)
	{
		const Lever& lv = g.levers[i];
		if (i)
			ss << ",";
		if (lv.key->kind == CK_TEXT)
			ss << lv.key->name << "=\"" << (lv.text + strlen(lv.key->name) + 1) << "\"";
		else
			ss << lv.text;
	}
	ss << " held=" << s_heldCount;
	if (g.devOnlyDrops > 0)
		ss << " (" << g.devOnlyDrops << " DEV-only levers dropped)";
	Log(ss.str());
}

// ---- the run ----

// What a held key holds in set: the set's lever value when it turns the key
// on, else the key's off value.
static const unsigned char* HeldTarget(const GroupRun& run, int set, int h)
{
	const Group& g = s_groups[run.group];
	int on = run.setLever[set];
	for (int i = 0; i < g.leverCount; ++i)
	{
		if (g.levers[i].key == s_held[h].key && (on == SET_ALL || on == i))
			return g.levers[i].bytes;
	}
	return s_held[h].key->kind == CK_TEXT ? s_held[h].user : s_held[h].off;
}

// Writes every held key (set's values, or the user's when set < 0) through
// the tab's live paths; returns the fields that moved and their pairs.
static int WriteHeld(const GroupRun& run, int set, std::string* pairs)
{
	int changed = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		if (!mod.state || IsRender(&mod))
			continue;
		bool staged = false;
		for (int h = 0; h < s_heldCount; ++h)
		{
			if (s_held[h].module != &mod)
				continue;
			if (!staged)
				StageModule(mod, &s_stage);
			staged = true;
			const unsigned char* src = set < 0 ? s_held[h].user : HeldTarget(run, set, h);
			memcpy((char*)s_stage.state + s_held[h].key->offset, src, FieldWidth(*s_held[h].key));
		}
		if (!staged)
			continue;
		std::vector<std::string> applied;
		changed += ApplyLiveModuleRows(mod, s_stage, &applied);
		for (size_t i = 0; i < applied.size(); ++i)
			*pairs += " " + applied[i];
	}

	RenderConfig before = g_renderCfg;
	RenderConfig next = g_renderCfg;
	bool render = false;
	for (int h = 0; h < s_heldCount; ++h)
	{
		if (!IsRender(s_held[h].module))
			continue;
		const unsigned char* src = set < 0 ? s_held[h].user : HeldTarget(run, set, h);
		memcpy((char*)&next + s_held[h].key->offset, src, FieldWidth(*s_held[h].key));
		render = true;
	}
	if (!render)
		return changed;
	ApplyRenderConfig(next);
	for (int h = 0; h < s_heldCount; ++h)
	{
		const ConfigKey& k = *s_held[h].key;
		if (!IsRender(s_held[h].module) || RenderValueEqual(before, g_renderCfg, k))
			continue;
		++changed;
		std::string v = FormatRenderValue(g_renderCfg, k);
		*pairs += " " + std::string(k.name) + "=" + (k.kind == CK_TEXT ? "\"" + v + "\"" : v);
	}
	return changed;
}

static std::string SetLabel(const GroupRun& run, int set)
{
	int on = run.setLever[set];
	if (on == SET_OFF)
		return "off";
	if (on == SET_ALL)
		return "all";
	return SetName(s_groups[run.group].levers[on]);
}

static void ApplySet(int set, void* ctx)
{
	GroupRun* run = (GroupRun*)ctx;
	if (!run->holding)
	{
		for (int h = 0; h < s_heldCount; ++h)
			memcpy(s_held[h].user, (const char*)s_held[h].module->state + s_held[h].key->offset,
			       FieldWidth(*s_held[h].key));
		run->holding = true;
	}
	std::string pairs;
	int changed = WriteHeld(*run, set, &pairs);
	std::ostringstream ss;
	ss << "Bench: apply group=" << s_groups[run->group].name << " set=" << SetLabel(*run, set) << " changed=" << changed
	   << pairs << " qpc=" << Qpc();
	Log(ss.str());
}

static void RestoreSettings(void* ctx)
{
	GroupRun* run = (GroupRun*)ctx;
	if (!run->holding)
		return;
	std::string pairs;
	int changed = WriteHeld(*run, -1, &pairs);
	run->holding = false;
	std::ostringstream ss;
	ss << "Bench: restore group=" << s_groups[run->group].name << " changed=" << changed << " qpc=" << Qpc();
	Log(ss.str());
}

void BenchGroupsResolve(ConfigLogFn log)
{
	s_log = log;
	s_groupCount = 0;
	s_heldCount = 0;
	for (int i = 0; i < BenchGroupTextCount() && s_groupCount < BENCH_GROUP_MAX; ++i)
	{
		if (ResolveGroup(BenchGroupTextName(i), BenchGroupTextValue(i), &s_groups[s_groupCount]))
			++s_groupCount;
	}
	for (int g = 0; g < s_groupCount; ++g)
		LogGroupLine(s_groups[g]);
}

int BenchGroupCount()
{
	return s_groupCount;
}

int BenchGroupFind(const char* name, const char** whyNot)
{
	for (int g = 0; name && g < s_groupCount; ++g)
	{
		if (strcmp(s_groups[g].name, name) != 0)
			continue;
		if (s_groups[g].leverCount > 0)
			return g;
		if (whyNot)
			*whyNot = "empty";
		return -1;
	}
	if (whyNot)
		*whyNot = "unknown";
	return -1;
}

const char* BenchGroupName(int g)
{
	return g >= 0 && g < s_groupCount ? s_groups[g].name : "";
}

int BenchGroupLeverCount(int g)
{
	return g >= 0 && g < s_groupCount ? s_groups[g].leverCount : 0;
}

const char* BenchGroupLeverText(int g, int i)
{
	return i >= 0 && i < BenchGroupLeverCount(g) ? s_groups[g].levers[i].text : "";
}

int BenchGroupHeldCount()
{
	return s_heldCount;
}

bool BuildBenchGroupAB(int g, BenchScenario* out, const char** whyNot)
{
	if (g < 0 || g >= s_groupCount)
	{
		*whyNot = "no such group";
		return false;
	}
	const Group& grp = s_groups[g];
	if (grp.leverCount == 0)
	{
		*whyNot = "empty group";
		return false;
	}
	for (int i = 0; i < grp.leverCount; ++i)
	{
		if (IsRender(grp.levers[i].module) && !g_renderCfg.renderLevers)
		{
			*whyNot = "renderLevers off";
			return false;
		}
	}
	if (!s_run)
		s_run = new GroupRun;
	GroupRun& run = *s_run;
	run.group = g;
	run.holding = false;
	run.setCount = 0;
	if (grp.sets != BGS_ONLY)
		run.setLever[run.setCount++] = SET_OFF;
	if (grp.sets == BGS_ALL)
		run.setLever[run.setCount++] = SET_ALL;
	else
	{
		for (int i = 0; i < grp.leverCount; ++i)
			run.setLever[run.setCount++] = i;
	}

	*out = BenchScenario();
	out->name = "group";
	std::string levers;
	for (int s = 0; s < run.setCount; ++s)
		out->sets.push_back(SetLabel(run, s));
	for (int i = 0; i < grp.leverCount; ++i)
		levers += (i ? "," : "") + SetName(grp.levers[i]);
	out->headerExtra = std::string("group=") + grp.name + " levers=" + levers;

	BenchScenarioParams p = { (float)grp.discardSec, (float)grp.measureSec };
	out->steps.push_back(BenchMakeStep(BS_ARM));
	out->steps.push_back(BenchMakeStep(BS_SET_POSE));
	out->steps.push_back(BenchMakeStep(BS_SET_SPEED));
	out->steps.push_back(BenchMakeStep(BS_SETTLE));
	int n = run.setCount;
	for (int pass = 0; pass < grp.passes; ++pass)
		for (int k = 0; k < n; ++k)
			out->steps.push_back(BenchMakeWindow(pass % 2 == 0 ? k : n - 1 - k, pass, p));
	out->steps.push_back(BenchMakeStep(BS_RESTORE));

	out->applySet = ApplySet;
	out->restoreSettings = RestoreSettings;
	out->ctx = s_run;
	out->AddRecorder(&run.frames);
	out->AddRecorder(&run.drift);
	out->AddRecorder(&run.zones);
	return true;
}

bool BenchGroupUserState(const ConfigModule& m, void* state)
{
	if (!state || !s_run || !s_run->holding)
		return false;
	for (int h = 0; h < s_heldCount; ++h)
	{
		if (s_held[h].module == &m)
			memcpy((char*)state + s_held[h].key->offset, s_held[h].user, FieldWidth(*s_held[h].key));
	}
	return true;
}

bool BenchGroupUserRenderConfig(RenderConfig* out)
{
	if (!out || !s_run || !s_run->holding)
		return false;
	for (int h = 0; h < s_heldCount; ++h)
	{
		if (IsRender(s_held[h].module))
			memcpy((char*)out + s_held[h].key->offset, s_held[h].user, FieldWidth(*s_held[h].key));
	}
	return true;
}
