#include "bench/bench_scenario.h"
#include <stddef.h>

BenchScenario::BenchScenario()
	: applySet(NULL), restoreSettings(NULL), ctx(NULL), recorderCount(0)
{
	for (int i = 0; i < BENCH_MAX_RECORDERS; ++i)
		recorders[i] = NULL;
}

bool BenchScenario::AddRecorder(BenchRecorder* r)
{
	if (!r || recorderCount >= BENCH_MAX_RECORDERS)
		return false;
	recorders[recorderCount++] = r;
	return true;
}

namespace bench_scenario_detail {
struct Kind { const char* name; BenchScenarioBuildFn build; };
const int MAX_KINDS = 8;
Kind s_kinds[MAX_KINDS];   // zero-initialised static storage
int  s_kindCount = 0;
}
using namespace bench_scenario_detail;

int BenchRegisterScenario(const char* name, BenchScenarioBuildFn build)
{
	if (!name || !build || s_kindCount >= MAX_KINDS)
		return -1;
	s_kinds[s_kindCount].name = name;
	s_kinds[s_kindCount].build = build;
	return s_kindCount++;
}

BenchScenarioBuildFn BenchScenarioBuilder(int kind)
{
	return kind >= 0 && kind < s_kindCount ? s_kinds[kind].build : NULL;
}

const char* BenchScenarioName(int kind)
{
	return kind >= 0 && kind < s_kindCount ? s_kinds[kind].name : NULL;
}

BenchStep BenchMakeStep(BenchStepKind kind)
{
	BenchStep s = { kind, -1, 0, 0.0f, 0.0f };
	return s;
}

BenchStep BenchMakeWindow(int set, int pass, const BenchScenarioParams& p)
{
	BenchStep s = { BS_WINDOW, set, pass, p.discardSec, p.measureSec };
	return s;
}

int BenchWindowCount(const BenchScenario& sc)
{
	int n = 0;
	for (size_t i = 0; i < sc.steps.size(); ++i)
	{
		if (sc.steps[i].kind == BS_WINDOW)
			++n;
	}
	return n;
}

void BenchInsertStepAfter(BenchScenario* sc, BenchStepKind after, BenchStepKind kind)
{
	for (size_t i = 0; i < sc->steps.size(); ++i)
	{
		if (sc->steps[i].kind == after)
		{
			sc->steps.insert(sc->steps.begin() + i + 1, BenchMakeStep(kind));
			return;
		}
	}
}
