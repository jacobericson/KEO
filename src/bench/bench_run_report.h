#pragma once
#include "bench/bench_recorders.h"
#include "bench/bench_report.h"
#include "bench/bench_scenario.h"
#include <string>

// Turns a finished (or aborted) run's recorders into a BenchReport. Pure.

struct BenchRunHeader
{
	std::string slotKey;
	int         speed;
	float       hourStart;      // negative: unknown
	float       hourEnd;        // negative: unknown
	bool        hourEndRead;    // false: not read (a save load), printed "-"
	float       recordedHour;
	std::string weather;
	int         chars, zones;
	std::string follow;         // restored, none, lost, off or "-"
	bool        orderAbort;     // a player order can abort the run
	std::string banner;         // render=, gate=, bench= tokens
	long long   qpcFreq;
};

// "12.34", or "?" when negative.
std::string BenchHourText(float h);

// windows = the windows that started, in run order (partial ones included).
// Statistics come from the first of sc's recorders with frame times; every
// recorder adds its own header and window fields, in list order.
BenchReport BuildBenchRunReport(const BenchScenario& sc, int windows, const BenchRunHeader& h,
                                const std::string& runId, const std::string& endReason);
