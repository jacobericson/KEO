#include "bench/bench_lever_ab.h"
#include "bench/bench_recorders.h"
#include "render/render_config.h"
#include <stddef.h>

namespace bench_lever_ab_detail {

// A bool lever's set writes on into its field. A float lever (e.g. a page
// budget in milliseconds) writes 0 when off, and when on the user's value,
// or its compiled default when the user's value is 0.
struct LeverEntry { const char* name; void (*set)(RenderConfig* cfg, bool on, const RenderConfig& user); };

template <bool RenderConfig::*Field>
void SetBoolLever(RenderConfig* cfg, bool on, const RenderConfig&) { cfg->*Field = on; }

const float kFoliageBenchBudgetMs = 4.0f;

void SetFoliageBudgetLever(RenderConfig* cfg, bool on, const RenderConfig& user)
{
	cfg->foliagePageBudgetMs = !on ? 0.0f
		: (user.foliagePageBudgetMs > 0.0f ? user.foliagePageBudgetMs : kFoliageBenchBudgetMs);
}

const LeverEntry kLevers[] = {
	{ "reflectionHalfRate",    &SetBoolLever<&RenderConfig::reflectionHalfRate> },
	{ "particleOffscreenSkip", &SetBoolLever<&RenderConfig::particleOffscreenSkip> },
	{ "particleStepCap",       &SetBoolLever<&RenderConfig::particleStepCap> },
	{ "oldAnimSkip",           &SetBoolLever<&RenderConfig::oldAnimSkip> },
	{ "gpuParamCache",         &SetBoolLever<&RenderConfig::gpuParamCache> },
	{ "shadowReachCull",       &SetBoolLever<&RenderConfig::shadowReachCull> },
	{ "emptyPassSkip",         &SetBoolLever<&RenderConfig::emptyPassSkip> },
	{ "foliagePageBudgetMs",   &SetFoliageBudgetLever },
	{ "gpuUploadSkip",         &SetBoolLever<&RenderConfig::gpuUploadSkip> },
};
const int kLeverCount = sizeof(kLevers) / sizeof(kLevers[0]);

// DEV diagnostics, forced off in every set; a new *Diag key is appended here.
bool RenderConfig::* const kDiagOff[] = { &RenderConfig::shadowReachDiag, &RenderConfig::gpuParamLookupDiag, &RenderConfig::oldAnimDiag,
                                         &RenderConfig::gpuUploadDiag };
const int kDiagOffCount = sizeof(kDiagOff) / sizeof(kDiagOff[0]);

// The subset bench.levers named, as indices into kLevers. count < 0 means
// every lever: the default, and bench.levers's fallback.
int s_listedIdx[kLeverCount];
int s_listedCount = -1;
// bench.levers=combined: two sets, every lever off and the user's.
bool s_combined = false;

int ListedCount() { return s_listedCount < 0 ? kLeverCount : s_listedCount; }
int ListedIndex(int i) { return s_listedCount < 0 ? i : s_listedIdx[i]; }

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

// One run at a time, so one context; allocated at the first build.
struct LeverAB
{
	RenderConfig      user;
	bool              holding;   // the bench's settings are applied
	FrameTimeRecorder frames;
	DriftRecorder     drift;
	ZoneEventRecorder zones;
};
LeverAB* s_ab = NULL;

void ApplySet(int set, void* ctx)
{
	LeverAB* ab = (LeverAB*)ctx;
	if (!ab->holding)
	{
		ab->user = g_renderCfg;
		ab->holding = true;
	}
	RenderConfig cfg;
	BenchLeverSetConfig(ab->user, set, &cfg);
	ApplyRenderConfig(cfg);
}

void RestoreSettings(void* ctx)
{
	LeverAB* ab = (LeverAB*)ctx;
	if (!ab->holding)
		return;
	ApplyRenderConfig(ab->user);
	ab->holding = false;
}

} // namespace
using namespace bench_lever_ab_detail;

int         BenchLeverCount() { return s_combined ? 0 : ListedCount(); }
bool        BenchLeversCombined() { return s_combined; }

double BenchLeverRunSeconds(bool combined)
{
	int sets = combined ? 2 : ListedCount() + 2;
	return BENCH_COUNTDOWN_SEC + BENCH_STABLE_SEC + BENCH_SETTLE_SEC +
	       2.0 * sets * (BENCH_DISCARD_SEC + BENCH_MEASURE_SEC);
}
const char* BenchLeverName(int i) { return kLevers[ListedIndex(i)].name; }

void BenchLeverSetConfig(const RenderConfig& user, int set, RenderConfig* out)
{
	*out = user;
	for (int d = 0; d < kDiagOffCount; ++d)
		out->*kDiagOff[d] = false;
	if (s_combined)
	{
		if (set == 0)
			for (int i = 0; i < kLeverCount; ++i)
				kLevers[i].set(out, false, user);
		return;
	}
	int n = ListedCount();
	if (set > n)   // "defaults"
		return;
	for (int i = 0; i < n; ++i)
		kLevers[ListedIndex(i)].set(out, set == i + 1, user);
}

bool ParseBenchLeversKey(const std::string& key, const std::string& val,
                         std::vector<std::string>* unknown, bool* usedFallback)
{
	if (key != "bench.levers")
		return false;
	if (usedFallback)
		*usedFallback = false;

	std::string t = Trim(val);
	s_combined = t == "combined";
	if (t.empty() || s_combined)
	{
		s_listedCount = -1;
		return true;
	}

	int found[kLeverCount];
	int foundCount = 0;
	size_t pos = 0;
	while (pos <= t.size())
	{
		size_t comma = t.find(',', pos);
		std::string name = Trim(comma == std::string::npos ? t.substr(pos) : t.substr(pos, comma - pos));
		if (!name.empty())
		{
			int idx = -1;
			for (int i = 0; i < kLeverCount; ++i)
			{
				if (name == kLevers[i].name)
				{
					idx = i;
					break;
				}
			}
			if (idx >= 0 && foundCount < kLeverCount)
				found[foundCount++] = idx;
			else if (idx < 0 && unknown)
				unknown->push_back(name);
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}

	if (foundCount == 0)
	{
		s_listedCount = -1;
		if (usedFallback)
			*usedFallback = true;
	}
	else
	{
		s_listedCount = foundCount;
		for (int i = 0; i < foundCount; ++i)
			s_listedIdx[i] = found[i];
	}
	return true;
}

bool BuildLeverAB(const BenchScenarioParams& p, BenchScenario* out, const char** whyNot)
{
	if (!g_renderCfg.renderLevers)
	{
		*whyNot = "renderLevers off";
		return false;
	}
	if (!s_ab)
		s_ab = new LeverAB;
	s_ab->holding = false;

	*out = BenchScenario();
	out->name = "leverAB";
	out->sets.push_back("off");
	int leverN = BenchLeverCount();
	std::string levers = s_combined ? "combined" : "";
	for (int i = 0; i < leverN; ++i)
	{
		const char* name = kLevers[ListedIndex(i)].name;
		out->sets.push_back(name);
		if (i)
			levers += ",";
		levers += name;
	}
	out->sets.push_back("defaults");
	out->headerExtra = "levers=" + levers;

	out->steps.push_back(BenchMakeStep(BS_ARM));
	out->steps.push_back(BenchMakeStep(BS_SET_POSE));
	out->steps.push_back(BenchMakeStep(BS_SET_SPEED));
	out->steps.push_back(BenchMakeStep(BS_SETTLE));
	int n = (int)out->sets.size();
	for (int pass = 0; pass < 2; ++pass)
		for (int k = 0; k < n; ++k)
			out->steps.push_back(BenchMakeWindow(pass == 0 ? k : n - 1 - k, pass, p));
	out->steps.push_back(BenchMakeStep(BS_RESTORE));

	out->applySet = ApplySet;
	out->restoreSettings = RestoreSettings;
	out->ctx = s_ab;
	// The report's window line lists the fields in this order.
	out->AddRecorder(&s_ab->frames);
	out->AddRecorder(&s_ab->drift);
	out->AddRecorder(&s_ab->zones);
	return true;
}

bool BenchLeverUserRenderConfig(RenderConfig* out)
{
	if (!out || !s_ab || !s_ab->holding)
		return false;
	*out = s_ab->user;
	return true;
}
