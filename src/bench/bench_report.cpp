#include "bench/bench_report.h"
#include <stdarg.h>
#include <stdio.h>

// Numbers only; names and free text are appended as strings, so nothing is cut.
static std::string Num(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	return buf;
}

// Every set's d/% is against sets[0], so d is 0 for sets[0] itself. A window
// names its set by looking setIndex up in sets.
std::vector<std::string> FormatBenchReport(const BenchReport& r)
{
	std::vector<std::string> lines;
	lines.push_back("Bench result " + r.runId + ": " + r.header);

	double baseMean = r.sets.empty() ? 0.0 : r.sets[0].both.meanMs;
	for (size_t i = 0; i < r.sets.size(); ++i)
	{
		const BenchSetResult& s = r.sets[i];
		double d = s.both.meanMs - baseMean;
		double pct = baseMean != 0.0 ? d / baseMean * 100.0 : 0.0;
		std::string line = "Bench set " + s.name + Num(
			": frames=%d mean=%.2fms low1=%.2fms fps=%.1f d=%+.2fms (%+.1f%%) pass1=%.2fms pass2=%.2fms",
			s.both.frames, s.both.meanMs, s.both.low1Ms, s.both.fps, d, pct, s.pass[0].meanMs, s.pass[1].meanMs);
		if (s.hitchTotal > 0)
			line += Num(" hitch=%d", s.hitchTotal);
		lines.push_back(line);
	}

	for (size_t i = 0; i < r.windows.size(); ++i)
	{
		const BenchWindowResult& w = r.windows[i];
		std::string setName = (w.setIndex >= 0 && w.setIndex < (int)r.sets.size()) ? r.sets[w.setIndex].name : "?";
		lines.push_back(Num("Bench window %d ", (int)i) + setName +
		                Num(" pass%d: frames=%d mean=%.2fms low1=%.2fms", w.pass + 1, w.stats.frames, w.stats.meanMs,
		                    w.stats.low1Ms) + w.fields);
	}

	lines.push_back("Bench end " + r.runId + ": " + r.endReason);
	return lines;
}
