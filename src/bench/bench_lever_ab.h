#pragma once
#include "bench/bench_scenario.h"
#include <string>
#include <vector>

struct RenderConfig;

// The lever A/B. Sets: "off" (every listed lever off), one per listed lever
// with only that lever on, and "defaults" (the user's levers); an unlisted
// lever and the DEV diagnostics keep a fixed value (the user's, and off,
// respectively) in every set. With bench.levers=combined there are two sets:
// "off" (every lever off) and "defaults". Steps: ARM, SET_POSE, SET_SPEED, SETTLE, the
// windows forward then reversed, RESTORE.
// Recorders: frame times, pose drift, zone events.

// The registered builder; refuses while renderLevers is off. The user's
// render settings are taken at the first window, where the run first
// changes them, and put back by the scenario's restore.
bool BuildLeverAB(const BenchScenarioParams& p, BenchScenario* out, const char** whyNot);

// Set `set` of a lever A/B, from the user's settings.
void BenchLeverSetConfig(const RenderConfig& user, int set, RenderConfig* out);

// The listed levers a lever A/B steps through, in order (one set per lever);
// none in combined mode.
int         BenchLeverCount();
const char* BenchLeverName(int i);
bool        BenchLeversCombined();
// The shortest lever A/B run, seconds (countdown, settle and every window),
// in combined mode or with the listed levers.
double      BenchLeverRunSeconds(bool combined);

// Startup: true when key is bench.levers; the value (a comma list, spaces
// trimmed around each name) becomes the run's listed levers, replacing the
// default of every table lever. The single name "combined" selects combined
// mode instead. A name the table doesn't recognize is appended to *unknown,
// in order, and dropped; when none of the value's names matched,
// *usedFallback is set true and every lever is listed anyway.
bool ParseBenchLeversKey(const std::string& key, const std::string& val,
                         std::vector<std::string>* unknown, bool* usedFallback);

// While a lever A/B holds the bench's render settings, copies the user's into
// out (the settings tab stages those, so closing Options mid-run never saves
// the bench's); false and out untouched otherwise.
bool BenchLeverUserRenderConfig(RenderConfig* out);
