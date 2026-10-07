#include "bench/bench_run_report.h"
#include "bench/bench_stats.h"
#include <stdio.h>

std::string BenchHourText(float h)
{
	if (!(h >= 0.0f))
		return "?";
	char buf[16];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f", h);
	return buf;
}

static BenchStats Stats(const std::vector<float>& v)
{
	return ComputeBenchStats(v.empty() ? NULL : &v[0], (int)v.size());
}

BenchReport BuildBenchRunReport(const BenchScenario& sc, int windows, const BenchRunHeader& h,
                                const std::string& runId, const std::string& endReason)
{
	std::vector<int> windowStep;
	for (size_t i = 0; i < sc.steps.size(); ++i)
	{
		if (sc.steps[i].kind == BS_WINDOW)
			windowStep.push_back((int)i);
	}
	if (windows > (int)windowStep.size())
		windows = (int)windowStep.size();
	if (windows < 0)
		windows = 0;

	const BenchRecorder* timer = NULL;
	for (int i = 0; i < sc.recorderCount && !timer; ++i)
	{
		if (sc.recorders[i]->FrameTimes(0))
			timer = sc.recorders[i];
	}
	const std::vector<float> none;
	std::vector<const std::vector<float>*> frames(windows, &none);
	for (int w = 0; w < windows && timer; ++w)
	{
		if (timer->FrameTimes(w))
			frames[w] = timer->FrameTimes(w);
	}

	BenchReport rep;
	rep.runId = runId;
	rep.endReason = endReason;

	char buf[512];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, " speed=%d hour=%s->%s recordedHour=%.2f", h.speed,
	            BenchHourText(h.hourStart).c_str(), h.hourEndRead ? BenchHourText(h.hourEnd).c_str() : "-",
	            h.recordedHour);
	rep.header = "slot=" + h.slotKey + buf + " weather=" + (h.weather.empty() ? std::string("unknown") : h.weather);
	rep.header += " pin=" + (h.pin.empty() ? std::string("none") : h.pin);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, " chars=%d zones=%d", h.chars, h.zones);
	rep.header += buf;
	rep.header += " follow=" + h.follow + (h.orderAbort ? "" : " orderAbort=off");
	for (int i = 0; i < sc.recorderCount; ++i)
		sc.recorders[i]->RunFields(&rep.header);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, " qpcFreq=%lld", h.qpcFreq);
	rep.header += " " + h.banner + buf;
	if (!sc.headerExtra.empty())
		rep.header += " " + sc.headerExtra;

	for (size_t s = 0; s < sc.sets.size(); ++s)
	{
		BenchSetResult res;
		res.name = sc.sets[s];
		res.hitchTotal = 0;
		std::vector<float> both, pass[2];
		for (int w = 0; w < windows; ++w)
		{
			const BenchStep& step = sc.steps[windowStep[w]];
			if (step.setIndex != (int)s)
				continue;
			const std::vector<float>& f = *frames[w];
			both.insert(both.end(), f.begin(), f.end());
			pass[step.pass & 1].insert(pass[step.pass & 1].end(), f.begin(), f.end());
			if (timer)
				res.hitchTotal += timer->HitchCount(w);
		}
		res.both = Stats(both);
		res.pass[0] = Stats(pass[0]);
		res.pass[1] = Stats(pass[1]);
		rep.sets.push_back(res);
	}

	for (int w = 0; w < windows; ++w)
	{
		const BenchStep& step = sc.steps[windowStep[w]];
		BenchWindowResult wr;
		wr.setIndex = step.setIndex;
		wr.pass = step.pass;
		wr.stats = Stats(*frames[w]);
		for (int i = 0; i < sc.recorderCount; ++i)
			sc.recorders[i]->WindowFields(w, &wr.fields);
		rep.windows.push_back(wr);
	}
	return rep;
}
