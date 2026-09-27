#pragma once
#include "bench/bench_stats.h"
#include <string>
#include <vector>

struct BenchSetResult { std::string name; BenchStats both; BenchStats pass[2]; int hitchTotal; };
struct BenchWindowResult
{
	int         setIndex;
	int         pass;
	BenchStats  stats;
	std::string fields;   // the recorders' own fields, each " key=value"
};
struct BenchReport
{
	std::string runId;          // e.g. "swamp-20260918-051733"
	std::string header;         // free text: slot, speed, hour, weather, counts, banner
	std::vector<BenchSetResult>    sets;      // sets[0] is the baseline
	std::vector<BenchWindowResult> windows;
	std::string endReason;      // "ok" or the abort reason
};
std::vector<std::string> FormatBenchReport(const BenchReport& r);
