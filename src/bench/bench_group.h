#pragma once
#include "base/config_table.h"
#include "bench/bench_scenario.h"
#include "bench/bench_sweep.h"

struct RenderConfig;

// Benchmark groups: bench.group.<name>=<sets>/<passes>/<discard>+<measure>:<key>=<value>;...
// switch any live setting per window, through the settings tab's own live
// paths, and put the user's values back at the run's end. Main thread only.

// One cap for the texts the INI keeps and the groups resolved from them, so
// resolving never drops a kept text.
const int BENCH_GROUP_MAX    = BENCH_GROUP_TEXT_MAX;
const int BENCH_GROUP_LEVERS = 16;
const int BENCH_GROUP_HELD   = 64;
static_assert(BENCH_GROUP_MAX == BENCH_GROUP_TEXT_MAX, "a group per kept bench.group text");
static_assert(BENCH_GROUP_HELD >= BENCH_GROUP_LEVERS, "every lever of one group can be held");

enum BenchGroupSets { BGS_EACH, BGS_ALL, BGS_ONLY };

// Startup, main thread, after the INI: resolves every bench.group text against
// the config tables; one line per group and per dropped lever through log, then
// one per bench.group key the INI dropped past the group limit. Again on a
// second call, from scratch.
void BenchGroupsResolve(ConfigLogFn log);

int         BenchGroupCount();
int         BenchGroupFind(const char* name, const char** whyNot);  // -1: "unknown" or "empty"
const char* BenchGroupName(int g);
int         BenchGroupLeverCount(int g);
const char* BenchGroupLeverText(int g, int i);   // "key=value" as resolved
int         BenchGroupHeldCount();

// The run's scenario: sets, windows (the group's own discard, measure and
// passes), recorders (frames, drift, zones), applySet and restoreSettings.
bool BuildBenchGroupAB(int g, BenchScenario* out, const char** whyNot);

// While a group run holds keys: writes the user's values of m's held keys into
// state (a copy of m.state); false and nothing written otherwise.
bool BenchGroupUserState(const ConfigModule& m, void* state);
// The same for the render module.
bool BenchGroupUserRenderConfig(RenderConfig* out);
