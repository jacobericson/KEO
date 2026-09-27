#include "bench/bench_recorders.h"
#include "bench/bench_game.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void AppendField(std::string* out, const char* fmt, ...)
{
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	*out += buf;
}

// ---- frame times ----

// A frame this slow is a hitch, not ordinary variance.
static const double HITCH_MS = 100.0;

void FrameTimeRecorder::OnRunStart(const BenchScenario& sc)
{
	m_cur = -1;
	m_frames.assign(BenchWindowCount(sc), std::vector<float>());
	m_dropped.assign(m_frames.size(), 0);
	m_hitchCount.assign(m_frames.size(), 0);
	m_hitchMaxMs.assign(m_frames.size(), 0.0f);
	m_fgCount.assign(m_frames.size(), 0);
	int w = 0;
	for (size_t i = 0; i < sc.steps.size(); ++i)
	{
		if (sc.steps[i].kind != BS_WINDOW)
			continue;
		m_frames[w++].reserve((size_t)ceil(sc.steps[i].measureSec * 500.0f) + 1);
	}
}

void FrameTimeRecorder::OnFrame(const BenchFrameSample& f)
{
	if (m_cur < 0 || m_cur >= (int)m_frames.size() || !f.measuring)
		return;
	if (f.ms > HITCH_MS)
	{
		++m_hitchCount[m_cur];
		if ((float)f.ms > m_hitchMaxMs[m_cur])
			m_hitchMaxMs[m_cur] = (float)f.ms;
	}
	if (BenchWindowInForeground())
		++m_fgCount[m_cur];
	std::vector<float>& v = m_frames[m_cur];
	if (v.size() < v.capacity())
		v.push_back((float)f.ms);
	else
		++m_dropped[m_cur];
}

void FrameTimeRecorder::RunFields(std::string* out) const
{
	int dropped = 0;
	for (size_t w = 0; w < m_dropped.size(); ++w)
		dropped += m_dropped[w];
	AppendField(out, " dropped=%d", dropped);
}

void FrameTimeRecorder::WindowFields(int window, std::string* out) const
{
	if (window < 0 || window >= Windows())
		return;
	int measured = (int)m_frames[window].size() + m_dropped[window];
	double pct = measured > 0 ? 100.0 * m_fgCount[window] / measured : 0.0;
	AppendField(out, " hitch=%d/%.1fms fg=%.1f%%", m_hitchCount[window], m_hitchMaxMs[window], pct);
}

void FrameTimeRecorder::WindowEndFields(int window, std::string* out) const
{
	if (window >= 0 && window < Windows())
		AppendField(out, " frames=%d dropped=%d", (int)m_frames[window].size(), m_dropped[window]);
}

const std::vector<float>* FrameTimeRecorder::FrameTimes(int window) const
{
	return window >= 0 && window < Windows() ? &m_frames[window] : 0;
}

int FrameTimeRecorder::HitchCount(int window) const
{
	return window >= 0 && window < (int)m_hitchCount.size() ? m_hitchCount[window] : 0;
}

// ---- pose drift ----

static const double DRIFT_ABORT_MS = 2000.0;

void DriftRecorder::OnRunStart(const BenchScenario& sc)
{
	Window zero = { 0.0f, 0.0f, 0, 0.0 };
	m_cur = -1;
	m_abort = false;
	m_w.assign(BenchWindowCount(sc), zero);
}

void DriftRecorder::OnFrame(const BenchFrameSample& f)
{
	if (m_cur < 0 || m_cur >= (int)m_w.size())
		return;
	Window& w = m_w[m_cur];
	if (f.driftUnits > w.maxUnits)
		w.maxUnits = f.driftUnits;
	if (f.driftDeg > w.maxDeg)
		w.maxDeg = f.driftDeg;
	if (f.driftUnits > BENCH_DRIFT_UNITS || f.driftDeg > BENCH_DRIFT_DEG)
	{
		++w.overFrames;
		w.overMs += f.ms;
		if (w.overMs > DRIFT_ABORT_MS)
			m_abort = true;
	}
}

void DriftRecorder::WindowFields(int window, std::string* out) const
{
	if (window >= 0 && window < (int)m_w.size())
		AppendField(out, " drift=%.1fu/%.1fdeg input=%d", m_w[window].maxUnits, m_w[window].maxDeg,
		            m_w[window].overFrames);
}

// ---- zone events ----

void ZoneEventRecorder::OnRunStart(const BenchScenario& sc)
{
	m_cur = -1;
	m_events.assign(BenchWindowCount(sc), 0);
}

void ZoneEventRecorder::Sample()
{
	int n = BenchLoadedZoneCount();
	if (n >= 0 && m_last >= 0 && m_cur >= 0 && m_cur < (int)m_events.size())
		m_events[m_cur] += abs(n - m_last);
	m_last = n;
}

void ZoneEventRecorder::WindowFields(int window, std::string* out) const
{
	if (window >= 0 && window < (int)m_events.size())
		AppendField(out, " zone=%d", m_events[window]);
}

void ZoneEventRecorder::OnWindowStart(int window)
{
	m_cur = window;
	m_last = BenchLoadedZoneCount();
	m_sinceMs = 0.0;
}

void ZoneEventRecorder::OnFrame(const BenchFrameSample& f)
{
	if (m_cur < 0 || f.measuring)
		return;
	m_sinceMs += f.ms;
	if (m_sinceMs >= 1000.0)
	{
		m_sinceMs = 0.0;
		Sample();
	}
}

void ZoneEventRecorder::OnWindowEnd(int, bool worldAlive)
{
	if (worldAlive)
		Sample();
	m_cur = -1;
}
