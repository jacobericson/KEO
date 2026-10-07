#include "bench/bench_slots.h"
#include "bench/bench_scenario.h"
#include "bench/bench_lever_ab.h"
#include "bench/bench_stats.h"
#include "bench/bench_report.h"
#include "bench/bench_game_math.h"
#include "bench/bench_recorders.h"
#include "bench/bench_run_report.h"
#include "bench/bench_sweep.h"
#include "render/render_config.h"
#include <cmath>
#include <cstring>
#include <cstdio>

#include "check.h"

// The facade calls the recorders make.
static int g_stubZones = 0;
int BenchLoadedZoneCount() { return g_stubZones; }
static bool g_stubForeground = true;
bool BenchWindowInForeground() { return g_stubForeground; }

// The lever A/B's settings go through ApplyRenderConfig; the stub keeps the last.
static RenderConfig g_applied;
static int g_applyCalls = 0;
bool ApplyRenderConfig(const RenderConfig& next) { g_applied = next; ++g_applyCalls; return true; }

static BenchScenario LeverScenario(float discardSec, float measureSec)
{
	BenchScenarioParams p = { discardSec, measureSec };
	BenchScenario sc;
	const char* why = NULL;
	BuildLeverAB(p, &sc, &why);
	return sc;
}

// BenchLeverField is gone; the tests know the levers' fields by name.
static bool LeverValue(const RenderConfig& cfg, const std::string& name)
{
	if (name == "reflectionHalfRate")    return cfg.reflectionHalfRate;
	if (name == "particleOffscreenSkip") return cfg.particleOffscreenSkip;
	if (name == "particleStepCap")       return cfg.particleStepCap;
	if (name == "oldAnimSkip")           return cfg.oldAnimSkip;
	if (name == "gpuParamCache")         return cfg.gpuParamCache;
	if (name == "shadowReachCull")       return cfg.shadowReachCull;
	if (name == "emptyPassSkip")         return cfg.emptyPassSkip;
	if (name == "foliagePageBudgetMs")   return cfg.foliagePageBudgetMs != 0.0f;
	if (name == "gpuUploadSkip")         return cfg.gpuUploadSkip;
	return false;
}

// A test-only lever entry in the float shape, on an existing float field,
// since no real float lever exists yet.
static void SetParticleOffscreenSecondsTestLever(RenderConfig* cfg, bool on, const RenderConfig& user)
{
	cfg->particleOffscreenSeconds = on
		? (user.particleOffscreenSeconds != 0.0f ? user.particleOffscreenSeconds : RenderConfigDefaults().particleOffscreenSeconds)
		: 0.0f;
}

static void BenchLeversKeyTests()
{
	std::vector<std::string> unknown;
	bool fallback = false;

	Check(!ParseBenchLeversKey("particleStepCap", "true", &unknown, &fallback), "non-bench.levers key refused");

	unknown.clear(); fallback = false;
	Check(ParseBenchLeversKey("bench.levers", "", &unknown, &fallback) && unknown.empty() && !fallback,
	      "empty value parses");
	int all = BenchLeverCount();
	Check(all >= 4 && std::string(BenchLeverName(0)) == "reflectionHalfRate", "empty value lists every lever");
	bool hasCull = false;
	for (int i = 0; i < all; ++i)
		hasCull = hasCull || std::string(BenchLeverName(i)) == "shadowReachCull";
	Check(hasCull, "shadowReachCull is a bench lever");

	unknown.clear(); fallback = false;
	Check(ParseBenchLeversKey("bench.levers", " particleStepCap , reflectionHalfRate ", &unknown, &fallback) &&
	      unknown.empty() && !fallback, "list parses, spaces trimmed");
	Check(BenchLeverCount() == 2 && std::string(BenchLeverName(0)) == "particleStepCap" &&
	      std::string(BenchLeverName(1)) == "reflectionHalfRate", "listed levers, order kept");

	unknown.clear(); fallback = false;
	Check(ParseBenchLeversKey("bench.levers", "bogus,particleStepCap", &unknown, &fallback) &&
	      unknown.size() == 1 && unknown[0] == "bogus" && !fallback, "unknown name reported");
	Check(BenchLeverCount() == 1 && std::string(BenchLeverName(0)) == "particleStepCap", "unknown name dropped, known one kept");

	unknown.clear(); fallback = false;
	Check(ParseBenchLeversKey("bench.levers", "bogus1,bogus2", &unknown, &fallback) &&
	      unknown.size() == 2 && fallback, "all unknown: every name reported");
	Check(BenchLeverCount() == all, "all unknown: falls back to every lever");

	// Set construction with a listed subset: an unlisted lever keeps the
	// user's value in every set; a listed lever is off in "off" and on only
	// in its own set.
	unknown.clear(); fallback = false;
	ParseBenchLeversKey("bench.levers", "particleStepCap", &unknown, &fallback);
	RenderConfig user = RenderConfigDefaults();
	user.reflectionHalfRate = true;
	user.particleOffscreenSkip = true;
	user.particleStepCap = true;
	RenderConfig off, on, def;
	BenchLeverSetConfig(user, 0, &off);
	BenchLeverSetConfig(user, 1, &on);
	BenchLeverSetConfig(user, 2, &def);
	Check(!off.particleStepCap && off.reflectionHalfRate && off.particleOffscreenSkip,
	      "listed lever off in 'off'; unlisted levers keep the user's true");
	Check(on.particleStepCap && on.reflectionHalfRate && on.particleOffscreenSkip,
	      "listed lever on in its own set; unlisted levers still the user's");
	Check(def.particleStepCap && def.reflectionHalfRate && def.particleOffscreenSkip && !def.shadowReachDiag,
	      "'defaults' is the user's config with the diagnostics off");

	BenchScenarioParams p = { 3.0f, 25.0f };
	BenchScenario sc;
	const char* why = NULL;
	Check(BuildLeverAB(p, &sc, &why) && sc.sets.size() == 3, "one listed lever: off + it + defaults");
	Check(sc.headerExtra == "levers=particleStepCap", "header text names the listed lever");

	// A float lever's set shape, through particleOffscreenSeconds since no
	// real one exists yet: 0 when off, the user's value on, or the compiled
	// default when the user's is 0.
	RenderConfig u = RenderConfigDefaults();
	u.particleOffscreenSeconds = 0.0f;
	RenderConfig fOff, fOn;
	SetParticleOffscreenSecondsTestLever(&fOff, false, u);
	SetParticleOffscreenSecondsTestLever(&fOn, true, u);
	Check(fOff.particleOffscreenSeconds == 0.0f, "float lever set shape: off writes 0");
	Check(fOn.particleOffscreenSeconds == RenderConfigDefaults().particleOffscreenSeconds,
	      "float lever set shape: on falls back to the compiled default when the user's is 0");
	u.particleOffscreenSeconds = 1.25f;
	SetParticleOffscreenSecondsTestLever(&fOn, true, u);
	Check(fOn.particleOffscreenSeconds == 1.25f, "float lever set shape: on keeps a non-zero user value");

	// The real float lever: off 0, on the user's budget or 4 ms when it is 0.
	unknown.clear(); fallback = false;
	Check(ParseBenchLeversKey("bench.levers", "foliagePageBudgetMs", &unknown, &fallback) &&
	      BenchLeverCount() == 1 && unknown.empty(), "foliagePageBudgetMs is a bench lever");
	u = RenderConfigDefaults();
	RenderConfig bOff, bOn;
	BenchLeverSetConfig(u, 0, &bOff);
	BenchLeverSetConfig(u, 1, &bOn);
	Check(bOff.foliagePageBudgetMs == 0.0f && bOn.foliagePageBudgetMs == 4.0f, "foliage budget: off 0, on 4 ms by default");
	u.foliagePageBudgetMs = 7.5f;
	BenchLeverSetConfig(u, 0, &bOff);
	BenchLeverSetConfig(u, 1, &bOn);
	Check(bOff.foliagePageBudgetMs == 0.0f && bOn.foliagePageBudgetMs == 7.5f, "foliage budget: on keeps the user's value");

	// Reset for the rest of the suite (BenchScenarioTests assumes every
	// lever listed).
	unknown.clear(); fallback = false;
	ParseBenchLeversKey("bench.levers", "", &unknown, &fallback);
}

static void BenchSlotTests()
{
	BenchPose p = { { -1234.5f, 88.25f, 4567.125f }, { 0.923880f, 0.0f, 0.382683f, 0.0f }, -812.5f };
	std::string s = FormatBenchPose(p);
	Check(s == "-1234.500,88.250,4567.125;0.923880,0.000000,0.382683,0.000000;-812.50", "pose format, negative zoom");
	BenchPose q;
	Check(ParseBenchPose(s, &q), "pose parse round trip");
	Check(std::fabs(q.pos[2] - 4567.125f) < 1e-3f && std::fabs(q.rot[2] - 0.382683f) < 1e-5f && q.zoom == -812.5f, "pose values, zoom sign kept");
	Check(ParseBenchPose("1,2,3;1,0,0,0;150", &q) && q.zoom == 150.0f, "positive zoom still accepted");
	Check(!ParseBenchPose("1,2,3;1,0,0,0;-5000", &q) && !ParseBenchPose("1,2,3;1,0,0,0;-5", &q), "negative zoom out of range refused");
	Check(!ParseBenchPose("1,2,3;1,0,0,0", &q), "missing zoom refused");
	Check(!ParseBenchPose("1,2,3;2,0,0,0;100", &q), "non-unit quaternion refused");
	Check(!ParseBenchPose("1,2,3;1,0,0,0;5000", &q), "zoom out of range refused");

	BenchSlot slots[BENCH_SLOT_COUNT];
	memset(slots, 0, sizeof(slots));
	Check(ParseBenchSlotKey("bench.swamp.pose", s, slots) && slots[0].recorded, "slot pose key");
	Check(ParseBenchSlotKey("bench.city.hour", "30", slots) && slots[1].hour == 24.0f, "hour clamped");
	Check(!ParseBenchSlotKey("bench.road.speed", "7", slots), "speed other than 1 or 20 refused");
	Check(ParseBenchSlotKey("bench.road.speed", "20", slots) && slots[2].speed == 20, "speed 20");
	Check(!ParseBenchSlotKey("bench.nowhere.pose", s, slots), "unknown slot refused");
	Check(!ParseBenchSlotKey("particleStepCap", "true", slots), "non-bench key refused");

	std::vector<IniEntry> e;
	BenchSlotIniEntries(0, slots[0], &e);
	Check(e.size() == 3 && e[0].key == "bench.swamp.pose" && e[0].append, "slot entries");

	Check(BENCH_SLOT_COUNT == 5, "five slots");
	Check(strcmp(BenchSlotKey(4), "sand") == 0 && strcmp(BenchSlotLabel(4), "Sand") == 0, "slot 4 is Sand");
	Check(strcmp(BenchSlotKey(0), "swamp") == 0 && strcmp(BenchSlotKey(3), "custom") == 0, "existing slot keys unchanged");
	Check(BenchSlotIndex("sand") == BENCH_SLOT_SAND && BenchSlotIndex("city") == BENCH_SLOT_CITY &&
	      BenchSlotIndex("desert") == -1, "slot index by key");
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		Check(BenchSlotIndex(BenchSlotKey(i)) == i, "every slot's key maps back to its index");
	Check(ParseBenchSlotKey("bench.sand.pose", s, slots) && slots[4].recorded, "sand pose key");
	Check(ParseBenchSlotKey("bench.sand.speed", "20", slots) && slots[4].speed == 20, "sand speed key");
	e.clear();
	BenchSlotIniEntries(4, slots[4], &e);
	Check(e.size() == 3 && e[0].key == "bench.sand.pose" && e[1].key == "bench.sand.hour" && e[2].key == "bench.sand.speed",
	      "sand slot entries");
}

static void BenchCombinedTests()
{
	std::vector<std::string> unknown;
	bool fallback = false;
	Check(ParseBenchLeversKey("bench.levers", " combined ", &unknown, &fallback) && unknown.empty() && !fallback,
	      "combined parses");
	Check(BenchLeversCombined() && BenchLeverCount() == 0, "combined: no per-lever sets");

	RenderConfig user = RenderConfigDefaults();
	user.reflectionHalfRate = true;
	user.particleStepCap = true;
	user.oldAnimSkip = false;
	user.foliagePageBudgetMs = 6.0f;
	user.shadowReachDiag = true;
	user.gpuUploadDiag = true;
	g_renderCfg = user;
	BenchScenario sc = LeverScenario(3.0f, 25.0f);
	Check(sc.sets.size() == 2 && sc.sets[0] == "off" && sc.sets[1] == "defaults", "combined: two sets, off and defaults");
	Check(BenchWindowCount(sc) == 4 && sc.steps.size() == 4 + 4 + 1, "combined: four windows");
	Check(sc.steps[4].setIndex == 0 && sc.steps[5].setIndex == 1 && sc.steps[6].setIndex == 1 && sc.steps[7].setIndex == 0,
	      "combined: off, defaults, defaults, off");
	Check(sc.headerExtra == "levers=combined", "combined: header text");

	RenderConfig off, def;
	BenchLeverSetConfig(user, 0, &off);
	BenchLeverSetConfig(user, 1, &def);
	static const char* const kAll[] = { "reflectionHalfRate", "particleOffscreenSkip", "particleStepCap", "oldAnimSkip",
	                                    "gpuParamCache", "shadowReachCull", "emptyPassSkip", "foliagePageBudgetMs" };
	for (int i = 0; i < (int)(sizeof(kAll) / sizeof(kAll[0])); ++i)
	{
		Check(!LeverValue(off, kAll[i]), "combined 'off': every table lever off");
		Check(LeverValue(def, kAll[i]) == LeverValue(user, kAll[i]), "combined 'defaults': the user's levers");
	}
	Check(def.foliagePageBudgetMs == 6.0f, "combined 'defaults': a float lever keeps the user's value");
	Check(!off.shadowReachDiag && !off.gpuUploadDiag && !def.shadowReachDiag && !def.gpuUploadDiag,
	      "combined: diagnostics off in both sets");
	Check(off.particleStepCapSpeed == user.particleStepCapSpeed, "combined: non-lever fields are the user's");

	Check(ParseBenchLeversKey("bench.levers", "combined,particleStepCap", &unknown, &fallback) &&
	      !BenchLeversCombined() && unknown.size() == 1 && unknown[0] == "combined" && BenchLeverCount() == 1,
	      "combined inside a list is an unknown name");
	unknown.clear();
	ParseBenchLeversKey("bench.levers", "combined", &unknown, &fallback);
	ParseBenchLeversKey("bench.levers", "", &unknown, &fallback);
	Check(!BenchLeversCombined() && BenchLeverCount() >= 8, "an empty value leaves combined mode");

	// Durations: countdown 3 + settle 1 + 10, then 2 windows of 3 + 25 s per set.
	Check(BenchLeverRunSeconds(true) == 126.0, "combined run: 4 windows, 126 s");
	Check(BenchLeverRunSeconds(false) == 14.0 + 56.0 * (BenchLeverCount() + 2), "listed run: 2 windows per set");
	ParseBenchLeversKey("bench.levers", "particleStepCap", &unknown, &fallback);
	Check(BenchLeverRunSeconds(false) == 182.0, "one listed lever: 182 s");
	ParseBenchLeversKey("bench.levers", "", &unknown, &fallback);
	g_renderCfg = RenderConfigDefaults();
}

static void BenchSweepKeyTests()
{
	const char* kDefault = "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20";
	std::vector<std::string> bad, extra;
	bool def = false;
	Check(!ParseBenchSweepKey("bench.levers", "swamp:1", &bad, &extra, &def), "non-bench.sweep key refused");
	Check(BenchSweepLegCount() == 6 && BenchSweepListText() == kDefault, "absent: the default legs");

	Check(ParseBenchSweepKey("bench.sweep", "  ", &bad, &extra, &def) && bad.empty() && !def && BenchSweepListText() == kDefault,
	      "empty: the default legs");

	Check(ParseBenchSweepKey("bench.sweep", " city:20 , road:1,sand:20 ", &bad, &extra, &def) && bad.empty() && !def,
	      "custom list parses, spaces trimmed");
	Check(BenchSweepLegCount() == 3 && BenchSweepLegAt(0).slot == 1 && BenchSweepLegAt(0).speed == 20 &&
	      BenchSweepLegAt(2).slot == 4 && BenchSweepListText() == "city:20,road:1,sand:20", "custom legs, order kept");

	bad.clear();
	Check(ParseBenchSweepKey("bench.sweep", "swamp:1,desert:1,city:5,sand,road : 20", &bad, &extra, &def) && !def && extra.empty(),
	      "bad entries parse around");
	Check(bad.size() == 3 && bad[0] == "desert:1" && bad[1] == "city:5" && bad[2] == "sand", "bad entries reported in order");
	Check(BenchSweepListText() == "swamp:1,road:20", "bad entries dropped, good ones kept");

	bad.clear();
	Check(ParseBenchSweepKey("bench.sweep", "desert:1,swamp:2", &bad, &extra, &def) && bad.size() == 2 && def,
	      "all bad: reported and the default flagged");
	Check(BenchSweepListText() == kDefault, "all bad: the default legs");

	bad.clear();
	std::string many;
	for (int i = 0; i < BENCH_SWEEP_MAX_LEGS + 2; ++i)
		many += i ? ",city:1" : "city:1";
	many += ",desert:1";
	Check(ParseBenchSweepKey("bench.sweep", many, &bad, &extra, &def) && BenchSweepLegCount() == BENCH_SWEEP_MAX_LEGS &&
	      extra.size() == 2 && extra[0] == "city:1" && bad.size() == 1 && bad[0] == "desert:1",
	      "legs past the limit reported apart from bad entries, and dropped");

	ParseBenchSweepKey("bench.sweep", "", &bad, &extra, &def);
}

// ---- the sweep sequencer against a stub runner ----

struct StubRunner
{
	bool                     active;
	int                      arms;
	int                      lastSlot, lastSpeed, lastGroup;
	std::string              lastExtra;
	const char*              refuse;        // the next arm's refusal, or NULL
	const char*              blocked;       // armBlocked's answer
	bool                     blockedFinal;
	std::string              aborted;
	std::vector<std::string> lines;
};
static StubRunner g_sr;

static bool StubArm(int slot, int speed, int group, const std::string& extra, std::string* why)
{
	if (g_sr.refuse)
	{
		*why = g_sr.refuse;
		return false;
	}
	++g_sr.arms;
	g_sr.active = true;
	g_sr.lastSlot = slot;
	g_sr.lastSpeed = speed;
	g_sr.lastGroup = group;
	g_sr.lastExtra = extra;
	return true;
}
static bool StubActive() { return g_sr.active; }
static void StubAbort(const char* reason) { g_sr.aborted = reason; }
static const char* StubBlocked(bool* isFinal) { *isFinal = g_sr.blockedFinal; return g_sr.blocked; }
static void StubLog(const std::string& line) { g_sr.lines.push_back(line); }
// Groups a, b and c resolve to 0, 1 and 2; e is empty; any other is unknown.
static int StubGroup(const char* name, const char** why)
{
	static const char* const kNames[] = { "a", "b", "c" };
	for (int i = 0; i < 3; ++i)
	{
		if (strcmp(name, kNames[i]) == 0)
			return i;
	}
	*why = strcmp(name, "e") == 0 ? "empty" : "unknown";
	return -1;
}

static void ResetStub()
{
	g_sr.active = false;
	g_sr.arms = 0;
	g_sr.lastSlot = g_sr.lastSpeed = g_sr.lastGroup = -1;
	g_sr.lastExtra.clear();
	g_sr.refuse = NULL;
	g_sr.blocked = NULL;
	g_sr.blockedFinal = false;
	g_sr.aborted.clear();
	g_sr.lines.clear();
	// Any parse starts the stages over, so each case starts at stage 1, leg 1.
	std::vector<std::string> bad, extra;
	bool def = false;
	ParseBenchSweepKey("bench.sweep", "", &bad, &extra, &def);
}

// What the runner does at a run's end: goes idle, then calls back.
static void StubEnd(bool ok, const char* reason)
{
	g_sr.active = false;
	BenchSweepOnRunEnd(ok, reason);
}

static bool LastLine(const char* text)
{
	return !g_sr.lines.empty() && g_sr.lines.back() == text;
}

static void BenchSweepSequencerTests()
{
	BenchSweepRunner runner = { &StubArm, &StubActive, &StubAbort, &StubBlocked, &StubLog, &StubGroup };
	BenchSweepSetRunner(runner);
	std::vector<std::string> bad, extra;
	bool def = false;
	ParseBenchSweepKey("bench.sweep", "", &bad, &extra, &def);
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		g_benchSlots[i].recorded = false;

	// Refused while a leg's slot is unrecorded; each missing slot named once.
	ResetStub();
	g_benchSlots[0].recorded = true;
	Check(!BenchSweepStart() && !BenchSweepActive() && g_sr.arms == 0, "unrecorded slot: refused, nothing armed");
	Check(LastLine("Bench sweep: refused (not recorded: city, sand)"), "unrecorded slots named once each");

	// A full sweep: advance on every ok end.
	g_benchSlots[1].recorded = true;
	g_benchSlots[4].recorded = true;
	g_benchSlots[1].speed = 1;
	ResetStub();
	Check(BenchSweepStart() && BenchSweepActive() && g_sr.arms == 1, "start arms the first leg");
	Check(g_sr.lastSlot == 0 && g_sr.lastSpeed == 1 && g_sr.lastExtra == "sweep=1/6 stage=1/1", "leg 1: swamp 1x, tagged 1/6");
	Check(BenchSweepLegNumber() == 1 && BenchSweepLegTotal() == 6, "progress 1/6");
	Check(!BenchSweepStart() && LastLine("Bench sweep: refused (a run is active)"), "a second start is refused");

	BenchSweepMainThreadTick(1.0);
	Check(g_sr.arms == 1, "no advance while the leg runs");
	static const int kSlot[6]  = { 0, 0, 1, 1, 4, 4 };
	static const int kSpeed[6] = { 1, 20, 1, 20, 1, 20 };
	double now = 2.0;
	for (int leg = 1; leg < 6; ++leg)
	{
		StubEnd(true, "ok");
		BenchSweepMainThreadTick(now);
		now += 1.0;
		char tag[32];
		sprintf_s(tag, sizeof(tag), "sweep=%d/6 stage=1/1", leg + 1);
		Check(g_sr.arms == leg + 1 && g_sr.lastSlot == kSlot[leg] && g_sr.lastSpeed == kSpeed[leg] && g_sr.lastExtra == tag,
		      "ok end: the next leg armed at its slot and speed");
		Check(BenchSweepLegNumber() == leg + 1, "progress follows the legs");
	}
	Check(g_benchSlots[1].speed == 1, "a leg's speed never changes the slot's");
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(now);
	Check(!BenchSweepActive() && g_sr.arms == 6 && LastLine("Bench sweep: done 1 stages"), "the last ok end completes the sweep");
	Check(BenchSweepLegNumber() == 0 && BenchSweepLegTotal() == 0, "idle after the sweep");

	// Any other end stops the sweep with the run's reason.
	ResetStub();
	BenchSweepStart();
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(10.0);
	StubEnd(false, "player order");
	BenchSweepMainThreadTick(11.0);
	Check(!BenchSweepActive() && g_sr.arms == 2 && LastLine("Bench sweep: stopped at 2/6 (player order), Sweep resumes there"),
	      "an abort stops the sweep at its leg");

	ResetStub();
	BenchSweepStart();
	StubEnd(false, "save load");
	BenchSweepMainThreadTick(12.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 1/6 (save load), Sweep resumes there"), "a save load stops the sweep");

	// A button: the leg's run is aborted, and the sweep stops once it ends.
	ResetStub();
	BenchSweepStart();
	BenchSweepAbort("button");
	Check(g_sr.aborted == "button" && BenchSweepActive() && BenchSweepLegNumber() == 0, "button: run aborted, sweep stopping");
	StubEnd(false, "button");
	BenchSweepMainThreadTick(13.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 1/6 (button), Sweep resumes there"), "button: stopped once the run ended");

	// The first leg refused by the runner (e.g. out of reach).
	ResetStub();
	g_sr.refuse = "no player character within reach";
	Check(!BenchSweepStart() && !BenchSweepActive() &&
	      LastLine("Bench sweep: stopped at 1/6 (no player character within reach), Sweep resumes there"), "first leg refused: stopped");

	// A later leg refused at arm.
	ResetStub();
	BenchSweepStart();
	StubEnd(true, "ok");
	g_sr.refuse = "no player character within reach";
	BenchSweepMainThreadTick(14.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 2/6 (no player character within reach), Sweep resumes there"),
	      "a later leg refused: stopped at that leg");

	// Between legs the sweep waits for a transient block, up to the limit.
	ResetStub();
	BenchSweepStart();
	StubEnd(true, "ok");
	g_sr.blocked = "no world or a transition in flight";
	BenchSweepMainThreadTick(100.0);
	BenchSweepMainThreadTick(100.0 + BENCH_SWEEP_GAP_LIMIT_SEC - 1.0);
	Check(BenchSweepActive() && g_sr.arms == 1 && BenchSweepLegNumber() == 2, "gap: waits while blocked");
	g_sr.blocked = NULL;
	BenchSweepMainThreadTick(100.0 + BENCH_SWEEP_GAP_LIMIT_SEC - 0.5);
	Check(g_sr.arms == 2 && g_sr.lastExtra == "sweep=2/6 stage=1/1", "gap: arms once the block clears");

	StubEnd(true, "ok");
	g_sr.blocked = "restore pending";
	BenchSweepMainThreadTick(200.0);
	BenchSweepMainThreadTick(200.0 + BENCH_SWEEP_GAP_LIMIT_SEC + 0.5);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 3/6 (restore pending), Sweep resumes there"), "gap: stops past the limit");

	ResetStub();
	BenchSweepStart();
	StubEnd(true, "ok");
	g_sr.blocked = "save load in flight";
	g_sr.blockedFinal = true;
	BenchSweepMainThreadTick(300.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 2/6 (save load in flight), Sweep resumes there"), "gap: a final block stops at once");

	// A button between legs stops at once; nothing is left to abort.
	ResetStub();
	BenchSweepStart();
	StubEnd(true, "ok");
	g_sr.blocked = "restore pending";
	BenchSweepMainThreadTick(400.0);
	BenchSweepAbort("button");
	Check(!BenchSweepActive() && g_sr.aborted.empty() && LastLine("Bench sweep: stopped at 2/6 (button), Sweep resumes there"),
	      "button in the gap: stopped at once");

	// A single run's end outside a sweep is ignored.
	ResetStub();
	BenchSweepOnRunEnd(false, "button");
	BenchSweepMainThreadTick(500.0);
	Check(!BenchSweepActive() && g_sr.lines.empty(), "a single run's end is not the sweep's");

	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		g_benchSlots[i].recorded = false;
}

static void BenchScenarioTests()
{
	RenderConfig user = RenderConfigDefaults();
	user.shadowReachDiag = true;
	user.gpuParamLookupDiag = true;
	user.oldAnimDiag = true;
	user.gpuUploadDiag = true;
	user.particleStepCap = true;
	g_renderCfg = user;
	BenchScenario sc = LeverScenario(3.0f, 25.0f);
	int n = BenchLeverCount();
	Check((int)sc.sets.size() == n + 2, "sets: off + levers + defaults");
	Check(sc.sets[0] == "off" && sc.sets[n + 1] == "defaults", "set names");
	std::string listed = "levers=";
	for (int i = 0; i < n; ++i)
		listed += std::string(i ? "," : "") + BenchLeverName(i);
	Check(sc.headerExtra == listed &&
	      sc.headerExtra.find("levers=reflectionHalfRate,particleOffscreenSkip,particleStepCap") == 0 &&
	      sc.headerExtra.find(",oldAnimSkip") != std::string::npos,
	      "header text names every listed lever");
	std::vector<RenderConfig> cfg(sc.sets.size());
	for (int s = 0; s < (int)sc.sets.size(); ++s)
	{
		BenchLeverSetConfig(user, s, &cfg[s]);
		Check(!cfg[s].shadowReachDiag && !cfg[s].gpuParamLookupDiag && !cfg[s].oldAnimDiag &&
		      !cfg[s].gpuUploadDiag,
		      "diagnostics off in every set");
	}
	for (int i = 0; i < n; ++i)
	{
		Check(sc.sets[1 + i] == BenchLeverName(i), "lever set named after its lever");
		Check(!LeverValue(cfg[0], BenchLeverName(i)), "off set has every lever off");
		for (int j = 0; j < n; ++j)
			Check(LeverValue(cfg[1 + i], BenchLeverName(j)) == (i == j), "lever set has only its lever on");
		Check(LeverValue(cfg[n + 1], BenchLeverName(i)) == LeverValue(user, BenchLeverName(i)), "defaults set keeps the user's levers");
	}
	Check(cfg[0].particleStepCapSpeed == user.particleStepCapSpeed, "non-lever fields are the user's");
	Check(sc.recorderCount == 3 && sc.applySet && sc.restoreSettings, "lever A/B supplies settings and 3 recorders");

	// The settings go through the scenario: the user's are taken at the
	// first apply and put back by the restore.
	RenderConfig out;
	Check(!BenchLeverUserRenderConfig(&out), "no user settings held before the first window");
	g_applyCalls = 0;
	sc.applySet(1, sc.ctx);
	Check(g_applyCalls == 1 && LeverValue(g_applied, BenchLeverName(0)) && !LeverValue(g_applied, BenchLeverName(1)) &&
	      !g_applied.shadowReachDiag, "applySet applies its set");
	g_renderCfg = g_applied;   // what the real ApplyRenderConfig does
	sc.applySet(0, sc.ctx);
	Check(BenchLeverUserRenderConfig(&out) && out.shadowReachDiag && out.particleStepCap,
	      "the user's settings are the ones before the first window");
	sc.restoreSettings(sc.ctx);
	Check(g_applyCalls == 3 && g_applied.shadowReachDiag && g_applied.particleStepCap, "restore applies the user's");
	Check(!BenchLeverUserRenderConfig(&out), "nothing held after the restore");

	g_renderCfg.renderLevers = false;
	BenchScenarioParams p = { 3.0f, 25.0f };
	const char* why = NULL;
	BenchScenario refused;
	Check(!BuildLeverAB(p, &refused, &why) && why && strcmp(why, "renderLevers off") == 0,
	      "refused while renderLevers is off");
	g_renderCfg = RenderConfigDefaults();

	int sets = (int)sc.sets.size();
	Check((int)sc.steps.size() == 4 + 2 * sets + 1, "step count");
	Check(sc.steps[0].kind == BS_ARM && sc.steps[3].kind == BS_SETTLE && sc.steps.back().kind == BS_RESTORE, "fixed steps");
	for (int k = 0; k < sets; ++k)
	{
		Check(sc.steps[4 + k].kind == BS_WINDOW && sc.steps[4 + k].setIndex == k && sc.steps[4 + k].pass == 0, "forward pass order");
		Check(sc.steps[4 + sets + k].setIndex == sets - 1 - k && sc.steps[4 + sets + k].pass == 1, "reverse pass order");
	}
	Check(sc.steps[4].discardSec == 3.0f && sc.steps[4].measureSec == 25.0f, "window lengths");
}

static void BenchStatsTests()
{
	float ms[200];
	for (int i = 0; i < 200; ++i) ms[i] = 10.0f;
	ms[7] = 40.0f; ms[99] = 30.0f;
	BenchStats s = ComputeBenchStats(ms, 200);
	Check(s.frames == 200, "stats frames");
	Check(std::fabs(s.meanMs - 10.25) < 1e-9, "stats mean");
	Check(std::fabs(s.low1Ms - 35.0) < 1e-9, "stats 1% low = mean of the 2 slowest of 200");
	Check(std::fabs(s.fps - 1000.0 / 10.25) < 1e-9, "stats fps");
	float one[1] = { 16.0f };
	Check(ComputeBenchStats(one, 1).low1Ms == 16.0, "1% low of one frame");
	Check(ComputeBenchStats(one, 0).frames == 0 && ComputeBenchStats(one, 0).fps == 0.0, "empty");
	float neg[3] = { -1.0f, 0.0f, 20.0f };
	Check(ComputeBenchStats(neg, 3).frames == 1, "non-positive frames skipped");
}

static void BenchReportTests()
{
	BenchReport r;
	r.runId = "swamp-20260918-051733";
	r.header = "slot=swamp speed=1 hour=12.00";
	BenchSetResult off = { "off" };
	off.both.frames = 1500; off.both.meanMs = 20.0; off.both.low1Ms = 31.5; off.both.fps = 50.0;
	off.pass[0] = off.both; off.pass[1] = off.both;
	BenchSetResult refl = { "reflectionHalfRate" };
	refl.both.frames = 1650; refl.both.meanMs = 18.0; refl.both.low1Ms = 28.0; refl.both.fps = 1000.0 / 18.0;
	refl.pass[0] = refl.both; refl.pass[1] = refl.both;
	refl.hitchTotal = 3;
	r.sets.push_back(off);
	r.sets.push_back(refl);
	BenchWindowResult w = { 1, 0, refl.both, " hitch=1/210.0ms fg=92.0% drift=0.5u/0.2deg input=0 zone=1" };
	r.windows.push_back(w);
	r.endReason = "ok";
	std::vector<std::string> lines = FormatBenchReport(r);
	Check(lines.size() == 5, "report line count: header, 2 sets, 1 window, end");
	Check(lines[0] == "Bench result swamp-20260918-051733: slot=swamp speed=1 hour=12.00", "report header");
	Check(lines[1] == "Bench set off: frames=1500 mean=20.00ms low1=31.50ms fps=50.0 d=+0.00ms (+0.0%) pass1=20.00ms pass2=20.00ms",
	      "report set line: no hitches, no suffix");
	Check(lines[2] == "Bench set reflectionHalfRate: frames=1650 mean=18.00ms low1=28.00ms fps=55.6 d=-2.00ms (-10.0%) pass1=18.00ms pass2=18.00ms hitch=3",
	      "report set line: hitch total appended");
	Check(lines[3] == "Bench window 0 reflectionHalfRate pass1: frames=1650 mean=18.00ms low1=28.00ms hitch=1/210.0ms fg=92.0% drift=0.5u/0.2deg input=0 zone=1", "report window line");
	Check(lines[4] == "Bench end swamp-20260918-051733: ok", "report end line");
}

static void BenchGameMathTests()
{
	// Instruction bytes from the game: getTimeStamp_inGameHours+0,
	// userPause+0x41 and OptionsWindow::getSingleton+0x47.
	static const unsigned char MOV_RAX_RIP[] = { 0x48, 0x8B, 0x05 };
	static const unsigned char MOVSS_RIP[]   = { 0xF3, 0x0F, 0x10, 0x35 };
	unsigned char code[0x60];
	memset(code, 0xCC, sizeof(code));
	const unsigned char ts[] = { 0x48, 0x8B, 0x05, 0x39, 0x32, 0xAC, 0x01 };
	memcpy(code, ts, sizeof(ts));
	uintptr_t t = 0;
	Check(BenchRipTarget(code, 0, MOV_RAX_RIP, 3, 0x14066C180ULL, &t) && t == 0x14212F3C0ULL, "rip target: sky instance");
	const unsigned char up[] = { 0xF3, 0x0F, 0x10, 0x35, 0xCF, 0xA9, 0x9A, 0x01 };
	memcpy(code + 0x41, up, sizeof(up));
	Check(BenchRipTarget(code, 0x41, MOVSS_RIP, 4, 0x140787470ULL, &t) && t == 0x142131E88ULL, "rip target: userPause speed");
	const unsigned char gs[] = { 0x48, 0x8B, 0x05, 0xA2, 0x74, 0xD2, 0x01 };
	memcpy(code + 0x47, gs, sizeof(gs));
	Check(BenchRipTarget(code, 0x47, MOV_RAX_RIP, 3, 0x140406B90ULL, &t) && t == 0x14212E080ULL, "rip target: options instance");
	Check(!BenchRipTarget(code, 0x41, MOV_RAX_RIP, 3, 0x140787470ULL, &t), "rip target: opcode mismatch refused");
	const unsigned char neg[] = { 0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF };
	memcpy(code, neg, sizeof(neg));
	Check(BenchRipTarget(code, 0, MOV_RAX_RIP, 3, 0x1000ULL, &t) && t == 0x1000ULL + 7 - 16, "rip target: negative displacement");

	float q[4] = { 2.0f, 0.0f, 0.0f, 0.0f };
	Check(BenchNormalizeQuat(q) && q[0] == 1.0f, "quaternion normalised");
	float z[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	Check(!BenchNormalizeQuat(z), "zero quaternion refused");
	Check(BenchClampZoom(5000.0f) == 2000.0f && BenchClampZoom(2.0f) == 10.0f && BenchClampZoom(812.5f) == 812.5f, "zoom clamp");
	Check(BenchClampZoom(-5000.0f) == -2000.0f && BenchClampZoom(-2.0f) == -10.0f && BenchClampZoom(-812.5f) == -812.5f,
	      "zoom clamp keeps the game's negative sign");
	Check(BenchClampZoom(0.0f) == -10.0f, "zoom 0 clamps behind the centre");
	float a[3] = { 0.0f, 0.0f, 0.0f }, b[3] = { 3.0f, 4.0f, 12.0f };
	Check(BenchDistance3(a, b) == 13.0f, "3d distance");
	Check(FormatBenchWeather("Dust storm", 0.4f, 3.14f) == "Dust_storm 0.40 wind 3.1", "weather text");
	Check(FormatBenchWeather("", 0.0f, 0.0f) == "none 0.00 wind 0.0", "weather text, no weather");
}

static void BenchDriftTests()
{
	const float id[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	const float yaw90[4] = { 0.70710678f, 0.0f, 0.70710678f, 0.0f };
	const float neg[4] = { -1.0f, 0.0f, 0.0f, 0.0f };
	Check(fabsf(BenchQuatAngleDeg(id, yaw90) - 90.0f) < 0.01f, "quaternion angle 90");
	Check(BenchQuatAngleDeg(id, id) == 0.0f, "quaternion angle 0");
	Check(BenchQuatAngleDeg(id, neg) == 0.0f, "q and -q are one orientation");
	const float yawSmall[4] = { cosf(0.25f * 3.14159265f / 180.0f), 0.0f, sinf(0.25f * 3.14159265f / 180.0f), 0.0f };
	Check(fabsf(BenchQuatAngleDeg(id, yawSmall) - 0.5f) < 0.05f, "quaternion angle 0.5");

	float units = -1.0f, deg = -1.0f;
	const float tp[3] = { 100.0f, 50.0f, 200.0f };
	const float up[3] = { 103.0f, 90.0f, 204.0f };
	BenchPoseDrift(tp, id, 800.0f, up, id, 801.0f, &units, &deg);
	Check(units == 5.0f && deg == 0.0f, "drift: horizontal distance, height ignored");
	BenchPoseDrift(tp, id, 800.0f, tp, yaw90, 810.0f, &units, &deg);
	Check(units == 10.0f && fabsf(deg - 90.0f) < 0.01f, "drift: zoom difference and angle");
}

static void BenchRecorderTests()
{
	BenchScenario sc = LeverScenario(1.0f, 0.01f);   // capacity 0.01 s x 500 + 1 = 6 frames

	FrameTimeRecorder frames;
	frames.OnRunStart(sc);
	int windows = 2 * (int)sc.sets.size();   // every set forward, then reverse
	Check(frames.Windows() == windows, "frame recorder: one buffer per window");
	frames.OnWindowStart(0);
	BenchFrameSample discard = { 16.0, 0.0f, 0.0f, false };
	frames.OnFrame(discard);
	BenchFrameSample m = { 20.0, 0.0f, 0.0f, true };
	BenchFrameSample hitch1 = { 140.0, 0.0f, 0.0f, true };
	BenchFrameSample hitch2 = { 180.0, 0.0f, 0.0f, true };
	g_stubForeground = true;
	frames.OnFrame(hitch1);
	g_stubForeground = false;
	frames.OnFrame(hitch2);
	g_stubForeground = true;
	for (int i = 0; i < 6; ++i)
		frames.OnFrame(m);
	frames.OnWindowEnd(0, true);
	frames.OnFrame(m);   // between windows: ignored
	Check(frames.Frames(0).size() == 6 && frames.Dropped(0) == 2, "frame recorder: discard skipped, overflow counted");
	Check(frames.Frames(1).empty(), "frame recorder: other windows untouched");
	Check(frames.HitchCount(0) == 2 && frames.HitchMaxMs(0) == 180.0f, "frame recorder: hitch count and max, over all measured frames");
	std::string wf;
	frames.WindowFields(0, &wf);
	Check(wf == " hitch=2/180.0ms fg=87.5%", "frame recorder: window fields (8 measured, 7 foregrounded)");

	DriftRecorder drift;
	drift.OnRunStart(sc);
	drift.OnWindowStart(3);
	BenchFrameSample still = { 500.0, 0.4f, 0.1f, false };
	BenchFrameSample moved = { 500.0, 3.0f, 0.1f, true };
	drift.OnFrame(still);
	for (int i = 0; i < 4; ++i)
		drift.OnFrame(moved);
	Check(drift.AbortReason() == 0 && drift.OverFrames(3) == 4, "drift recorder: 2 s over the limit is not yet an abort");
	drift.OnFrame(moved);
	Check(drift.AbortReason() != 0 && strcmp(drift.AbortReason(), "camera moved") == 0, "drift recorder: past 2 s aborts");
	Check(drift.MaxUnits(3) == 3.0f && drift.MaxDeg(3) == 0.1f, "drift recorder: maxima");

	ZoneEventRecorder zones;
	zones.OnRunStart(sc);
	g_stubZones = 30;
	zones.OnWindowStart(0);
	BenchFrameSample discardSec = { 1000.0, 0.0f, 0.0f, false };
	BenchFrameSample measureSec = { 1000.0, 0.0f, 0.0f, true };
	g_stubZones = 32;
	zones.OnFrame(discardSec);
	Check(zones.Events(0) == 2, "zone recorder: samples once a second in the discard");
	g_stubZones = 29;
	zones.OnFrame(measureSec);
	zones.OnFrame(measureSec);
	Check(zones.Events(0) == 2, "zone recorder: no sample while measuring");
	g_stubZones = 31;
	zones.OnWindowEnd(0, true);
	Check(zones.Events(0) == 3, "zone recorder: sums the count's changes up to the window end");
	zones.OnWindowStart(1);
	g_stubZones = 40;
	zones.OnWindowEnd(1, false);
	Check(zones.Events(1) == 0, "zone recorder: no read at a save-load end");
}

static void BenchRunReportTests()
{
	BenchScenario sc = LeverScenario(0.0f, 1.0f);
	FrameTimeRecorder frames;
	DriftRecorder drift;
	ZoneEventRecorder zones;
	sc.recorderCount = 0;
	sc.AddRecorder(&frames);
	sc.AddRecorder(&drift);
	sc.AddRecorder(&zones);
	frames.OnRunStart(sc);
	drift.OnRunStart(sc);
	zones.OnRunStart(sc);
	// Window 0 is "off" (pass 1), window 1 the first lever.
	BenchFrameSample f20 = { 20.0, 0.2f, 0.1f, true };
	BenchFrameSample f10 = { 10.0, 0.0f, 0.0f, true };
	frames.OnWindowStart(0);
	drift.OnWindowStart(0);
	frames.OnFrame(f20);
	frames.OnFrame(f20);
	drift.OnFrame(f20);
	frames.OnWindowEnd(0, true);
	drift.OnWindowEnd(0, true);
	frames.OnWindowStart(1);
	frames.OnFrame(f10);
	frames.OnWindowEnd(1, true);

	BenchRunHeader h;
	h.slotKey = "swamp";
	h.speed = 20;
	h.hourStart = 12.5f;
	h.hourEnd = -1.0f;
	h.hourEndRead = false;
	h.recordedHour = 13.0f;
	h.weather = "";
	h.chars = 5;
	h.zones = 31;
	h.follow = "none";
	h.orderAbort = false;
	h.banner = "render=3/3 gate=ok bench=ok";
	h.qpcFreq = 10000000;
	BenchReport rep = BuildBenchRunReport(sc, 2, h, "swamp-x", "save load (window 1)");
	Check(rep.header == "slot=swamp speed=20 hour=12.50->- recordedHour=13.00 weather=unknown chars=5 zones=31 "
	                    "follow=none orderAbort=off dropped=0 render=3/3 gate=ok bench=ok qpcFreq=10000000 " + sc.headerExtra &&
	      sc.headerExtra.find("levers=reflectionHalfRate,particleOffscreenSkip,particleStepCap,") == 0,
	      "run report header: save load, order abort off");
	Check(rep.windows.size() == 2 && rep.windows[0].setIndex == 0 && rep.windows[1].setIndex == 1,
	      "run report: started windows only, in order");
	Check(rep.windows[0].stats.frames == 2 &&
	      rep.windows[0].fields == " hitch=0/0.0ms fg=100.0% drift=0.2u/0.1deg input=0 zone=0",
	      "run report: window stats and the recorders' fields in order");
	std::vector<std::string> lines = FormatBenchReport(rep);
	Check(lines[1 + sc.sets.size()] ==
	      "Bench window 0 off pass1: frames=2 mean=20.00ms low1=20.00ms hitch=0/0.0ms fg=100.0% drift=0.2u/0.1deg input=0 zone=0",
	      "run report: window line has hitch and fg before drift and zone");
	std::string endFields;
	frames.WindowEndFields(0, &endFields);
	Check(endFields == " frames=2 dropped=0", "frame recorder: window end fields");
	Check(rep.sets.size() == sc.sets.size() && rep.sets[0].both.meanMs == 20.0 && rep.sets[1].both.meanMs == 10.0 &&
	      rep.sets[2].both.frames == 0, "run report: sets merged per set, unmeasured sets empty");
	Check(rep.sets[0].hitchTotal == 0 && rep.sets[1].hitchTotal == 0, "run report: no hitches in this run, set totals 0");
	Check(BenchHourText(-1.0f) == "?" && BenchHourText(7.25f) == "7.25", "hour text");

	h.orderAbort = true;
	h.hourEndRead = true;
	h.hourEnd = 13.0f;
	rep = BuildBenchRunReport(sc, 99, h, "swamp-x", "ok");
	Check(rep.header.find("hour=12.50->13.00") != std::string::npos && rep.header.find("orderAbort") == std::string::npos,
	      "run report header: end hour read, order abort on");
	Check(rep.windows.size() == 2 * sc.sets.size(), "run report: window count clamped to the scenario");
}

// A second scenario through the same seam: two windows, no settings, one
// recorder that counts measured frames and keeps no frame times.
class CountingRecorder : public BenchRecorder
{
public:
	CountingRecorder() : m_cur(-1) {}
	void OnRunStart(const BenchScenario& sc) { m_counts.assign(BenchWindowCount(sc), 0); }
	void OnWindowStart(int window) { m_cur = window; }
	void OnFrame(const BenchFrameSample& f) { if (m_cur >= 0 && f.measuring) ++m_counts[m_cur]; }
	void OnWindowEnd(int, bool) { m_cur = -1; }
	void OnRunEnd() {}
	void RunFields(std::string* out) const { *out += " counted=yes"; }
	void WindowFields(int window, std::string* out) const
	{
		char buf[32];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, " count=%d", m_counts[window]);
		*out += buf;
	}

private:
	int m_cur;
	std::vector<int> m_counts;
};

static CountingRecorder g_counter;

static bool BuildTwoWindows(const BenchScenarioParams& p, BenchScenario* out, const char**)
{
	*out = BenchScenario();
	out->name = "twoWindows";
	out->sets.push_back("a");
	out->sets.push_back("b");
	out->steps.push_back(BenchMakeStep(BS_ARM));
	out->steps.push_back(BenchMakeWindow(0, 0, p));
	out->steps.push_back(BenchMakeWindow(1, 0, p));
	out->steps.push_back(BenchMakeStep(BS_RESTORE));
	out->AddRecorder(&g_counter);
	return true;
}

static void BenchSecondScenarioTests()
{
	int lever = BenchRegisterScenario("leverAB", BuildLeverAB);
	int two = BenchRegisterScenario("twoWindows", BuildTwoWindows);
	Check(lever == 0 && two == 1 && strcmp(BenchScenarioName(two), "twoWindows") == 0, "scenario table");
	Check(BenchScenarioBuilder(2) == NULL && BenchScenarioName(-1) == NULL, "unregistered kinds");

	BenchScenarioParams p = { 1.0f, 2.0f };
	BenchScenario sc;
	const char* why = NULL;
	Check(BenchScenarioBuilder(two)(p, &sc, &why), "second scenario builds");
	Check(!sc.applySet && !sc.restoreSettings && sc.recorderCount == 1 && BenchWindowCount(sc) == 2,
	      "second scenario: no settings, one recorder, two windows");

	// What the runner does with it, minus the game.
	for (int i = 0; i < sc.recorderCount; ++i)
		sc.recorders[i]->OnRunStart(sc);
	BenchFrameSample discard = { 16.0, 0.0f, 0.0f, false };
	BenchFrameSample m = { 16.0, 0.0f, 0.0f, true };
	for (int w = 0; w < 2; ++w)
	{
		for (int i = 0; i < sc.recorderCount; ++i)
			sc.recorders[i]->OnWindowStart(w);
		for (int k = 0; k < 3 + w; ++k)
			for (int i = 0; i < sc.recorderCount; ++i)
				sc.recorders[i]->OnFrame(k == 0 ? discard : m);
		for (int i = 0; i < sc.recorderCount; ++i)
			sc.recorders[i]->OnWindowEnd(w, true);
	}
	BenchRunHeader h;
	h.slotKey = "custom";
	h.speed = 1;
	h.hourStart = h.hourEnd = h.recordedHour = 6.0f;
	h.hourEndRead = true;
	h.chars = 1;
	h.zones = 9;
	h.follow = "none";
	h.orderAbort = true;
	h.banner = "bench=ok";
	h.qpcFreq = 100;
	std::vector<std::string> lines = FormatBenchReport(BuildBenchRunReport(sc, 2, h, "custom-x", "ok"));
	Check(lines.size() == 6, "second scenario: header, 2 sets, 2 windows, end");
	Check(lines[0] == "Bench result custom-x: slot=custom speed=1 hour=6.00->6.00 recordedHour=6.00 weather=unknown "
	                  "chars=1 zones=9 follow=none counted=yes bench=ok qpcFreq=100", "second scenario: header fields");
	Check(lines[3] == "Bench window 0 a pass1: frames=0 mean=0.00ms low1=0.00ms count=2" &&
	      lines[4] == "Bench window 1 b pass1: frames=0 mean=0.00ms low1=0.00ms count=3",
	      "second scenario: its recorder's fields, no frame times");
}

static void SetStage(const char* key, const char* val)
{
	ParseBenchSweepFamilyKey(key, val, &StubLog);
}

static void BenchSweepStageTests()
{
	BenchSweepRunner runner = { &StubArm, &StubActive, &StubAbort, &StubBlocked, &StubLog, &StubGroup };
	BenchSweepSetRunner(runner);
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		g_benchSlots[i].recorded = true;
	std::vector<std::string> bad, extra;
	bool def = false;

	// A leg's third field names a group; speed 0 is the paused leg.
	bad.clear();
	Check(ParseBenchSweepKey("bench.sweep", "city:0:paused, swamp:20:ai ,road:1,city:0:Bad,sand:1:abcdefghijklm", &bad,
	                         &extra, &def) && !def,
	      "sweep: a leg names a group and speed 0");
	Check(BenchSweepLegCount() == 3 && BenchSweepLegAt(0).speed == 0 && strcmp(BenchSweepLegAt(0).group, "paused") == 0 &&
	      BenchSweepLegAt(1).speed == 20 && strcmp(BenchSweepLegAt(1).group, "ai") == 0 && BenchSweepLegAt(2).group[0] == 0 &&
	      bad.size() == 2 && BenchSweepListText() == "city:0:paused,swamp:20:ai,road:1",
	      "sweep: a leg names a group and speed 0");

	// Stage keys in number order, whatever the INI order; bench.sweep ignored.
	ResetStub();
	ParseBenchSweepKey("bench.sweep", "sand:1", &bad, &extra, &def);
	SetStage("bench.sweep.3", "road:1:b");
	SetStage("bench.sweep.1", "city:1:a, swamp:20:a ,custom:0:c");
	SetStage("bench.sweep.9", "city:1");
	Check(LastLine("Bench: bench.sweep.9 ignored (the stages are bench.sweep.1 to bench.sweep.8)"),
	      "stage: bench.sweep.<n> keys are the stages, in number order");
	Check(BenchSweepLegCount() == 3 && BenchSweepListText() == "city:1:a,swamp:20:a,custom:0:c",
	      "stage: bench.sweep.<n> keys are the stages, in number order");
	g_sr.lines.clear();
	Check(BenchSweepStart() && g_sr.arms == 1 && g_sr.lastSlot == BENCH_SLOT_CITY && g_sr.lastGroup == 0,
	      "stage: bench.sweep.<n> keys are the stages, in number order");
	Check(g_sr.lines.size() >= 3 && g_sr.lines[0] == "Bench sweep: bench.sweep ignored (bench.sweep.<n> keys are set)",
	      "stage: bench.sweep is ignored while any stage key is set");
	Check(g_sr.lines.size() >= 3 &&
	      g_sr.lines[1] == "Bench sweep: started stage 1/2 at leg 1/3 city:1:a,swamp:20:a,custom:0:c",
	      "stage: bench.sweep.<n> keys are the stages, in number order");
	Check(g_sr.lastExtra == "sweep=1/3 stage=1/2", "stage: the header names the leg and the stage");

	// Leg 2 ends badly: stopped, and the next press resumes there.
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(1.0);
	Check(g_sr.lastExtra == "sweep=2/3 stage=1/2" && g_sr.lastSpeed == 20 && g_sr.lastGroup == 0,
	      "stage: the header names the leg and the stage");
	StubEnd(false, "player order");
	BenchSweepMainThreadTick(2.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stopped at 2/3 (player order), Sweep resumes there"),
	      "stage: Sweep resumes a stopped stage at the stopped leg");
	Check(BenchSweepListText() == "city:1:a,swamp:20:a,custom:0:c",
	      "stage: Sweep resumes a stopped stage at the stopped leg");
	Check(BenchSweepStart() && g_sr.arms == 3 && g_sr.lastSlot == BENCH_SLOT_SWAMP && g_sr.lastExtra == "sweep=2/3 stage=1/2",
	      "stage: Sweep resumes a stopped stage at the stopped leg");
	Check(g_sr.lines.size() >= 2 &&
	      g_sr.lines[g_sr.lines.size() - 2] == "Bench sweep: started stage 1/2 at leg 2/3 city:1:a,swamp:20:a,custom:0:c",
	      "stage: Sweep resumes a stopped stage at the stopped leg");

	// The stage runs out; the next press runs stage 2, then back to stage 1.
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(3.0);
	Check(g_sr.lastSlot == BENCH_SLOT_CUSTOM && g_sr.lastSpeed == 0 && g_sr.lastGroup == 2,
	      "stage: Sweep runs the next stage after a stage is done");
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(4.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: stage 1/2 done") && BenchSweepListText() == "road:1:b",
	      "stage: Sweep runs the next stage after a stage is done");
	Check(BenchSweepStart() && g_sr.lastSlot == BENCH_SLOT_ROAD && g_sr.lastGroup == 1 &&
	      g_sr.lastExtra == "sweep=1/1 stage=2/2", "stage: Sweep runs the next stage after a stage is done");
	StubEnd(true, "ok");
	BenchSweepMainThreadTick(5.0);
	Check(!BenchSweepActive() && LastLine("Bench sweep: done 2 stages") &&
	      BenchSweepListText() == "city:1:a,swamp:20:a,custom:0:c", "stage: Sweep runs the next stage after a stage is done");

	// A group that does not resolve refuses the whole stage before any arm.
	SetStage("bench.sweep.1", "city:1:a,road:1:zz");
	ResetStub();
	Check(!BenchSweepStart() && g_sr.arms == 0 && !BenchSweepActive() &&
	      LastLine("Bench sweep: refused (group 'zz' unknown)"), "stage: an unknown group refuses the stage, nothing armed");
	SetStage("bench.sweep.1", "city:1:e");
	ResetStub();
	Check(!BenchSweepStart() && g_sr.arms == 0 && LastLine("Bench sweep: refused (group 'e' empty)"),
	      "stage: an unknown group refuses the stage, nothing armed");

	SetStage("bench.sweep.1", "");
	SetStage("bench.sweep.3", "");
	ParseBenchSweepKey("bench.sweep", "", &bad, &extra, &def);
	Check(BenchSweepListText() == "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20",
	      "stage: bench.sweep.<n> keys are the stages, in number order");
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
		g_benchSlots[i].recorded = false;
}

int main()
{
	BenchSlotTests();
	BenchScenarioTests();
	BenchLeversKeyTests();
	BenchCombinedTests();
	BenchSweepKeyTests();
	BenchSweepSequencerTests();
	BenchSweepStageTests();
	BenchStatsTests();
	BenchReportTests();
	BenchGameMathTests();
	BenchDriftTests();
	BenchRecorderTests();
	BenchRunReportTests();
	BenchSecondScenarioTests();

	return CheckExit("bench_units");
}
