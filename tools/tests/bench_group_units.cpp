// Benchmark groups, built twice at /DZONEHAND_STEP=3: with /DKEO_DEBUG (the
// DEV table, where the DEV-only switches are levers) and without it (the PROD
// table, where they are dropped). The PROD build exercises the same rules
// through rows both builds read: the render keys and the zone module's live rows.

#include "bench/bench_group.h"
#include "bench/bench_sweep.h"
#include "bench/bench_scenario.h"
#include "gui/settings_factory.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "base/config_table.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"

// The runtime the suite's sources call. ApplyRenderConfig stores the config
// as the real one does once its values are clamped.
static RenderConfig g_lastApplied;
static int g_applyCalls = 0;
bool ApplyRenderConfig(const RenderConfig& next)
{
	g_lastApplied = next;
	g_renderCfg = next;
	++g_applyCalls;
	return true;
}
bool BenchWindowInForeground() { return true; }
int BenchLoadedZoneCount() { return 0; }

#ifdef KEO_DEBUG
static const char* const SUITE_NAME = "bench_group_units";
// Two module switches in two modules, and their off and on texts.
static const char* const KEY_A = "hullSameSkip";
static const char* const KEY_B = "npcFailMemo";
static const char* const A_ON = "on";
static const char* const A_OFF = "off";
static const char* const B_ON = "on";
static const char* const B_OFF = "off";
#else
static const char* const SUITE_NAME = "bench_group_prod_units";
static const char* const KEY_A = "zoneLifeSquadRadius";
static const char* const KEY_B = "reflectionHalfRate";
static const char* const A_ON = "2";
static const char* const A_OFF = "0";
static const char* const B_ON = "true";
static const char* const B_OFF = "false";
#endif

static std::vector<std::string> g_lines;
static void TestLog(const std::string& line) { g_lines.push_back(line); }

static bool HasLine(const std::string& text)
{
	for (size_t i = 0; i < g_lines.size(); ++i)
	{
		if (g_lines[i] == text)
			return true;
	}
	return false;
}

static bool HasLineWith(const std::string& part)
{
	for (size_t i = 0; i < g_lines.size(); ++i)
	{
		if (g_lines[i].find(part) != std::string::npos)
			return true;
	}
	return false;
}

static void SetGroup(const std::string& name, const std::string& value)
{
	ParseBenchSweepFamilyKey("bench.group." + name, value, &TestLog);
}

static void ClearGroups()
{
	while (BenchGroupTextCount() > 0)
		SetGroup(BenchGroupTextName(0), "");
}

// Clears every group, stores these, and resolves them.
static void Groups(const char* n1, const char* v1, const char* n2 = NULL, const char* v2 = NULL)
{
	ClearGroups();
	SetGroup(n1, v1);
	if (n2)
		SetGroup(n2, v2);
	g_lines.clear();
	BenchGroupsResolve(&TestLog);
}

static int Find(const char* name)
{
	const char* why = NULL;
	return BenchGroupFind(name, &why);
}

static const ConfigModule* ModuleOf(const char* key)
{
	const ConfigModule* m = NULL;
	FindConfigKey(key, &m);
	return m;
}

// The running value as the INI writes it.
static std::string Running(const char* key)
{
	const ConfigModule* m = NULL;
	const ConfigKey* k = FindConfigKey(key, &m);
	return k ? ConfigFormatValue(*m, *k, m->state) : std::string("?");
}

static void SetRunning(const char* key, const char* val)
{
	const ConfigModule* m = NULL;
	const ConfigKey* k = FindConfigKey(key, &m);
	if (k)
		ConfigApplyValue(*m, *k, val, &TestLog);
}

static bool Build(const char* name, BenchScenario* sc)
{
	const char* why = NULL;
	return BuildBenchGroupAB(Find(name), sc, &why);
}

// Every lever the checks name starts off, the render text at its default.
static void ResetRunning()
{
	g_renderCfg = RenderConfigDefaults();
	g_renderCfg.renderLevers = true;
	g_renderCfg.reflectionHalfRate = false;
	g_renderCfg.particleStepCap = false;
	g_renderCfg.foliagePageBudgetMs = 0.0f;
	SetRunning(KEY_A, A_OFF);
	SetRunning(KEY_B, B_OFF);
}

// ---- parse ----

static void ParseTests()
{
	ResetRunning();
	Groups("p1", "each/2/5+60:reflectionHalfRate=true; particleStepCap=true ;foliagePageBudgetMs=4");
	int g = Find("p1");
	Check(g >= 0 && BenchGroupLeverCount(g) == 3 && std::string(BenchGroupLeverText(g, 0)) == "reflectionHalfRate=true" &&
	      std::string(BenchGroupLeverText(g, 2)) == "foliagePageBudgetMs=4" && BenchGroupHeldCount() == 3 &&
	      HasLine("Bench: group p1 sets=each passes=2 window=5+60 "
	              "levers=reflectionHalfRate=true,particleStepCap=true,foliagePageBudgetMs=4 held=3"),
	      "parse: a group's head and three levers");

	ClearGroups();
	SetGroup("m1", "each/2/5:reflectionHalfRate=true");
	SetGroup("m2", "x/2/5+60:reflectionHalfRate=true");
	SetGroup("m3", "each/9/5+60:reflectionHalfRate=true");
	g_lines.clear();
	BenchGroupsResolve(&TestLog);
	const char* why = NULL;
	Check(BenchGroupCount() == 0 && BenchGroupFind("m1", &why) == -1 && std::string(why) == "unknown" &&
	      HasLine("Bench: group m1 refused (the window is <discard>+<measure>)") &&
	      HasLine("Bench: group m2 refused (the sets are each, all or only)") &&
	      HasLine("Bench: group m3 refused (the passes are 1-8)"),
	      "parse: a malformed head refuses the group");

	Groups("l8", "each/2/10+40:particleLoopingNames=fire,smoke,torch,rain,weather,poison gas;reflectionHalfRate=true");
	g = Find("l8");
	Check(g >= 0 && BenchGroupLeverCount(g) == 2 &&
	      std::string(BenchGroupLeverText(g, 0)) == "particleLoopingNames=fire,smoke,torch,rain,weather,poison gas",
	      "parse: the lever separator is a semicolon, so a value keeps its commas");

	std::string many = "each/1/3+25:";
	for (int i = 1; i <= 17; ++i)
	{
		char buf[40];
		sprintf_s(buf, sizeof(buf), "%sfoliagePageBudgetMs=%d", i > 1 ? ";" : "", i);
		many += buf;
	}
	Groups("big", many.c_str());
	g = Find("big");
	Check(g >= 0 && BenchGroupLeverCount(g) == BENCH_GROUP_LEVERS &&
	      HasLine("Bench: group big: foliagePageBudgetMs (past 16), dropped"),
	      "parse: a group past 16 levers keeps 16");
}

// ---- resolve ----

static const ConfigKey* StartupOnlyRow()
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		for (int i = 0; kConfigModules[m].keys[i].name; ++i)
		{
			const ConfigKey& k = kConfigModules[m].keys[i];
			if (!k.live && !k.retired && !k.debugOnlyReader && kConfigModules[m].state)
				return &k;
		}
	}
	return NULL;
}

static void ResolveTests()
{
	ResetRunning();
	Groups("r1", "each/1/3+25:noSuchKey=on;reflectionHalfRate=true");
	Check(BenchGroupLeverCount(Find("r1")) == 1 && HasLine("Bench: group r1: noSuchKey is unknown, dropped"),
	      "resolve: an unknown key is dropped");

	const ConfigKey* startup = StartupOnlyRow();
	std::string text = std::string("each/1/3+25:") + (startup ? startup->name : "none") + "=1;reflectionHalfRate=true";
	Groups("r2", text.c_str());
	Check(startup && BenchGroupLeverCount(Find("r2")) == 1 &&
	      HasLine(std::string("Bench: group r2: ") + startup->name + " is startup-only, dropped"),
	      "resolve: a startup-only row is dropped");

	Groups("r6", "each/1/3+25:squadPathCache=1;reflectionHalfRate=true");
	Check(BenchGroupLeverCount(Find("r6")) == 1 && HasLine("Bench: group r6: squadPathCache is retired, dropped"),
	      "resolve: a retired row is dropped as retired");

#ifdef KEO_DEBUG
	const char* refusedKey = "lightCache";
#else
	const char* refusedKey = "reflectionHalfRate";
#endif
	text = std::string("each/1/3+25:") + refusedKey + "=bright;particleStepCap=true";
	Groups("r3", text.c_str());
	Check(BenchGroupLeverCount(Find("r3")) == 1 &&
	      HasLine(std::string("Bench: group r3: ") + refusedKey + " does not take 'bright', dropped"),
	      "resolve: a value the row refuses is dropped");

	text = std::string("each/1/3+25:") + KEY_A + "=" + A_OFF + ";particleStepCap=true";
	Groups("r4", text.c_str());
	Check(BenchGroupLeverCount(Find("r4")) == 1 &&
	      HasLine(std::string("Bench: group r4: ") + KEY_A + " equals its off value, dropped"),
	      "resolve: a value equal to its off value is dropped");

	Groups("r5", "each/1/3+25:noSuchKey=1");
	const char* why = NULL;
	Check(BenchGroupFind("r5", &why) == -1 && why && std::string(why) == "empty" && HasLine("Bench: group r5 empty"),
	      "resolve: a group with no lever left is empty");
}

// ---- off values and sets ----

static void OffTests()
{
#ifdef KEO_DEBUG
	ResetRunning();
	SetRunning("ogreJoinSpinUs", "50");
	SetRunning("lightCache", "on");
	Groups("o1", "each/1/3+25:ogreJoinSpinUs=20;lightCache=shadow");
	BenchScenario sc;
	Check(Build("o1", &sc), "off: a choice row's off is its first choice");
	sc.applySet(0, sc.ctx);
	Check(Running("ogreJoinSpinUs") == "0" && Running("lightCache") == "off", "off: a choice row's off is its first choice");
	sc.restoreSettings(sc.ctx);
	Check(Running("ogreJoinSpinUs") == "50" && Running("lightCache") == "on", "off: a choice row's off is its first choice");
	SetRunning("ogreJoinSpinUs", "0");
	SetRunning("lightCache", "off");
#endif

	ResetRunning();
	strcpy_s(g_renderCfg.particleLoopingNames, sizeof(g_renderCfg.particleLoopingNames), "fire,smoke");
	Groups("o2", "each/1/3+25:particleLoopingNames=fire,smoke,poison gas");
	BenchScenario st;
	bool built = Build("o2", &st);
	if (built)
		st.applySet(1, st.ctx);
	bool on = std::string(g_renderCfg.particleLoopingNames) == "fire,smoke,poison gas";
	if (built)
		st.applySet(0, st.ctx);
	Check(built && on && std::string(g_renderCfg.particleLoopingNames) == "fire,smoke",
	      "off: a text row's off is the user's own text");
	if (built)
		st.restoreSettings(st.ctx);
}

static bool WindowsAre(const BenchScenario& sc, const int* sets, int n)
{
	int w = 0;
	for (size_t i = 0; i < sc.steps.size(); ++i)
	{
		if (sc.steps[i].kind != BS_WINDOW)
			continue;
		if (w >= n || sc.steps[i].setIndex != sets[w])
			return false;
		++w;
	}
	return w == n;
}

static void SetsTests()
{
	ResetRunning();
	BenchScenario sc;
	Groups("s1", "each/2/3+25:reflectionHalfRate=true;particleStepCap=true");
	static const int kAbba[] = { 0, 1, 2, 2, 1, 0 };
	Check(Build("s1", &sc) && sc.sets.size() == 3 && sc.sets[0] == "off" && sc.sets[1] == "reflectionHalfRate=true" &&
	      sc.sets[2] == "particleStepCap=true" && WindowsAre(sc, kAbba, 6),
	      "sets: each is off then one per lever, two passes A B B A");

	Groups("s2", "all/1/3+25:reflectionHalfRate=true;particleStepCap=true");
	Check(Build("s2", &sc) && sc.sets.size() == 2 && sc.sets[0] == "off" && sc.sets[1] == "all", "sets: all is off and all");

	Groups("s3", "only/1/3+25:reflectionHalfRate=true");
	Check(Build("s3", &sc) && sc.sets.size() == 1 && sc.sets[0] == "reflectionHalfRate=true", "sets: only has no off");

	Groups("s4", "each/1/3+25:particleLoopingNames=fire,poison gas");
	Check(Build("s4", &sc) && sc.sets.size() == 2 && sc.sets[1] == "particleLoopingNames=fire,poison_gas",
	      "sets: a value's spaces are underscores in the set name");

#ifdef KEO_DEBUG
	Groups("s5", "each/1/3+25:ogreJoinSpinUs=20;ogreJoinSpinUs=50");
	const char* twoA = "ogreJoinSpinUs=20";
	const char* twoB = "ogreJoinSpinUs=50";
#else
	Groups("s5", "each/1/3+25:foliagePageBudgetMs=2;foliagePageBudgetMs=5");
	const char* twoA = "foliagePageBudgetMs=2";
	const char* twoB = "foliagePageBudgetMs=5";
#endif
	Check(Build("s5", &sc) && sc.sets.size() == 3 && sc.sets[1] == twoA && sc.sets[2] == twoB,
	      "sets: one key with two values makes two sets");

	Groups("s6", "all/1/3+25:foliagePageBudgetMs=2;foliagePageBudgetMs=5;reflectionHalfRate=true");
	Check(BenchGroupLeverCount(Find("s6")) == 2 &&
	      HasLine("Bench: group s6: foliagePageBudgetMs repeats a key in an all group, dropped"),
	      "sets: an all group drops a repeated key");

	Groups("s7", "each/3/7+42:reflectionHalfRate=true");
	bool windows = Build("s7", &sc) && sc.steps.size() == 4 + 6 + 1 && sc.steps[0].kind == BS_ARM &&
	               sc.steps[3].kind == BS_SETTLE && sc.steps.back().kind == BS_RESTORE && BenchWindowCount(sc) == 6;
	for (size_t i = 0; windows && i < sc.steps.size(); ++i)
	{
		if (sc.steps[i].kind == BS_WINDOW)
			windows = sc.steps[i].discardSec == 7.0f && sc.steps[i].measureSec == 42.0f;
	}
	Check(windows, "sets: every window has the group's discard and measure");
}

// ---- apply, held, restore and the tab guard ----

static bool ModulesEqual(const std::vector<std::vector<char> >& copy)
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		// The render module is compared key by key: the stub's struct copy need not keep padding.
		if (mod.state && mod.state != (void*)&g_renderCfg && memcmp(&copy[m][0], mod.state, mod.stateSize) != 0)
			return false;
	}
	return true;
}

static std::vector<std::vector<char> > CopyModules()
{
	std::vector<std::vector<char> > copy(kConfigModuleCount);
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigModule& mod = kConfigModules[m];
		copy[m].assign(mod.stateSize + 1, 0);
		if (mod.state)
			memcpy(&copy[m][0], mod.state, mod.stateSize);
	}
	return copy;
}

static void ApplyTests()
{
	ResetRunning();
	SetRunning(KEY_A, A_ON);   // the user's own values, which no set may carry along
	g_renderCfg.particleStepCap = true;
	std::string a = std::string(KEY_A) + "=" + A_ON;
	std::string b = std::string(KEY_B) + "=" + B_ON;
	std::string text = "each/1/3+25:" + a + ";" + b + ";particleLoopingNames=fire,poison gas";
	Groups("a1", text.c_str(), "a2", "each/1/3+25:particleStepCap=true");
	std::vector<std::vector<char> > before = CopyModules();
	RenderConfig renderBefore = g_renderCfg;

	BenchScenario sc;
	bool built = Build("a1", &sc);
	Check(built && sc.sets.size() == 4, "apply: the running config holds the set's value");
	if (!built)
		return;

	g_lines.clear();
	sc.applySet(2, sc.ctx);   // KEY_B's set
	Check(Running(KEY_B) == B_ON, "apply: the running config holds the set's value");
	Check(Running(KEY_A) == A_OFF, "apply: a key the set does not name sits at its off value");
	Check(!g_renderCfg.particleStepCap, "held: a key named only by another group sits at its off value");
	Check(HasLineWith("Bench: apply group=a1 set=" + b + " changed=3 ") && HasLineWith(" qpc="),
	      "apply: changed counts the fields that moved");

	// The tab, opened mid-run, stages the user's values for the held keys.
	const ConfigModule* ma = ModuleOf(KEY_A);
	std::vector<char> buf(ma->stateSize + 1, 0);
	memcpy(&buf[0], ma->state, ma->stateSize);
	bool wrote = BenchGroupUserState(*ma, &buf[0]);
	const ConfigKey* ka = FindConfigKey(KEY_A, NULL);
	Check(wrote && ConfigFormatValue(*ma, *ka, &buf[0]) == A_ON, "tab: the user's values replace the held keys while a run holds");
	RenderConfig tabRender = g_renderCfg;
	Check(BenchGroupUserRenderConfig(&tabRender) && !tabRender.particleStepCap == !renderBefore.particleStepCap,
	      "tab: the user's values replace the held keys while a run holds");

	sc.applySet(1, sc.ctx);   // KEY_A's set
	Check(Running(KEY_A) == A_ON && Running(KEY_B) == B_OFF, "apply: the running config holds the set's value");
	g_lines.clear();
	sc.applySet(1, sc.ctx);
	Check(HasLineWith("Bench: apply group=a1 set=" + a + " changed=0 qpc="), "apply: changed counts the fields that moved");

	sc.applySet(3, sc.ctx);   // the render text's set
	Check(std::string(g_renderCfg.particleLoopingNames) == "fire,poison gas" &&
	      std::string(g_lastApplied.particleLoopingNames) == "fire,poison gas",
	      "apply: the render text reaches the render config");
	Check(Running(KEY_A) == A_OFF && Running(KEY_B) == B_OFF, "apply: a key the set does not name sits at its off value");

	g_lines.clear();
	sc.restoreSettings(sc.ctx);
	Check(ModulesEqual(before), "restore: every module state is the user's again");
	std::vector<RenderChange> diff;
	DiffRenderConfig(renderBefore, g_renderCfg, &diff);
	Check(diff.empty(), "restore: the render config is the user's again");
	Check(HasLineWith("Bench: restore group=a1 changed="), "restore: every module state is the user's again");

	memcpy(&buf[0], ma->state, ma->stateSize);
	std::vector<char> untouched = buf;
	RenderConfig r = g_renderCfg;
	RenderConfig rBefore = r;
	Check(!BenchGroupUserState(*ma, &buf[0]) && buf == untouched && !BenchGroupUserRenderConfig(&r) &&
	      memcmp(&r, &rBefore, sizeof(r)) == 0,
	      "tab: nothing is written while no run holds");
	ResetRunning();
}

static void BuildTests()
{
	ResetRunning();
	g_renderCfg.renderLevers = false;
	Groups("b1", "each/1/3+25:reflectionHalfRate=true");
	BenchScenario sc;
	const char* why = NULL;
	Check(!BuildBenchGroupAB(Find("b1"), &sc, &why) && why && std::string(why) == "renderLevers off",
	      "build: a group holding a render key needs renderLevers");
	Groups("b2", "each/1/3+25:noSuchKey=1");
	why = NULL;
	Check(!BuildBenchGroupAB(0, &sc, &why) && why && std::string(why) == "empty group", "build: an empty group is refused");
	ResetRunning();
}

static void DevOnlyTests()
{
	ResetRunning();
	Groups("d1", "each/1/3+25:hullSameSkip=on", "d2", "each/1/3+25:hullSameSkip=on;reflectionHalfRate=true");
#ifndef KEO_DEBUG
	std::ostringstream d2;
	d2 << "Bench: group d2 sets=each passes=1 window=3+25 levers=reflectionHalfRate=true held=" << BenchGroupHeldCount()
	   << " (1 DEV-only levers dropped)";
	Check(Find("d1") == -1 && HasLine("Bench: group d1 empty (DEV-only levers)") &&
	      BenchGroupLeverCount(Find("d2")) == 1 && HasLine(d2.str()) && !HasLineWith("DEV-only in this build"),
	      "prod: a DEV-only key is dropped");
#else
	Check(BenchGroupLeverCount(Find("d1")) == 1 && BenchGroupLeverCount(Find("d2")) == 2 && !HasLineWith("DEV-only"),
	      "dev: a DEV-only key is kept");
#endif
	ClearGroups();
}

static void CapTests()
{
	ResetRunning();
	ClearGroups();
	for (int i = 0; i < BENCH_GROUP_TEXT_MAX; ++i)
	{
		std::ostringstream name;
		name << "g" << i;
		SetGroup(name.str(), "each/1/3+25:reflectionHalfRate=true");
	}
	g_lines.clear();
	BenchGroupsResolve(&TestLog);
	Check(BENCH_GROUP_TEXT_MAX == 64 && BenchGroupCount() == 64 && BenchGroupFind("g0", NULL) == 0 &&
	      BenchGroupFind("g63", NULL) == 63 && !HasLineWith("not resolved"), "group: 64 groups resolve");

	SetGroup("g64", "each/1/3+25:reflectionHalfRate=true");
	g_lines.clear();
	BenchGroupsResolve(&TestLog);
	Check(BenchGroupCount() == 64 && BenchGroupFind("g64", NULL) == -1 &&
	      HasLine("Bench: group g64 not resolved (past the 64-group limit)"),
	      "group: a group past the limit is named where the groups resolve");
	ClearGroups();
}

int main()
{
	ParseTests();
	ResolveTests();
	OffTests();
	SetsTests();
	ApplyTests();
	BuildTests();
	DevOnlyTests();
	CapTests();
	return CheckExit(SUITE_NAME);
}
