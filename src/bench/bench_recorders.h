#pragma once
#include "bench/bench_scenario.h"
#include <string>
#include <vector>

// One tick of a window, after the pose check and before any re-apply.
struct BenchFrameSample
{
	double ms;           // wall time since the previous tick
	float  driftUnits;   // the pose's distance from the target (BenchPoseDrift)
	float  driftDeg;
	bool   measuring;    // in the measured part of the window, not the discard
};

// Drift above either limit is user input (or a clamp) moving the camera.
const float BENCH_DRIFT_UNITS = 1.0f;
const float BENCH_DRIFT_DEG   = 0.5f;

// The runner calls OnRunStart once, then per window OnWindowStart, OnFrame
// for every tick after the first, OnWindowEnd, and OnRunEnd last (also after
// an abort). Windows are numbered 0.. in run order. worldAlive is false when
// a save load ended the run: the game must not be read then. Main thread only.
class BenchRecorder
{
public:
	virtual ~BenchRecorder() {}
	virtual void OnRunStart(const BenchScenario& sc) = 0;   // the only place that allocates
	virtual void OnWindowStart(int window) = 0;
	virtual void OnFrame(const BenchFrameSample& f) = 0;
	virtual void OnWindowEnd(int window, bool worldAlive) = 0;
	virtual void OnRunEnd() = 0;
	// Non-NULL (a string literal) when the run must stop.
	virtual const char* AbortReason() const { return 0; }

	// What the recorder adds to the result, each field as " key=value": the
	// result header (RunFields), a window's result line (WindowFields) and
	// the runner's window end line (WindowEndFields).
	virtual void RunFields(std::string*) const {}
	virtual void WindowFields(int, std::string*) const {}
	virtual void WindowEndFields(int, std::string*) const {}
	// A window's measured frame times (ms); NULL when the recorder keeps none.
	// The result's statistics come from the first recorder that has them.
	virtual const std::vector<float>* FrameTimes(int) const { return 0; }
	// Frames over the hitch threshold in a window; 0 when the recorder keeps
	// none. Summed across a set's windows for the Bench set line's hitch=.
	virtual int HitchCount(int) const { return 0; }
};

// Measured frame times per window, in fixed buffers (500 fps worth of the
// window's measure length); frames past that are counted, not stored. Also
// tracks, per measured frame regardless of that buffer: hitches (longer than
// HITCH_MS) and whether the game window was in the foreground, through the
// facade's BenchWindowInForeground.
class FrameTimeRecorder : public BenchRecorder
{
public:
	FrameTimeRecorder() : m_cur(-1) {}
	void OnRunStart(const BenchScenario& sc);
	void OnWindowStart(int window) { m_cur = window; }
	void OnFrame(const BenchFrameSample& f);
	void OnWindowEnd(int, bool) { m_cur = -1; }
	void OnRunEnd() { m_cur = -1; }
	void RunFields(std::string* out) const;             // dropped=
	void WindowFields(int window, std::string* out) const;      // hitch= fg=
	void WindowEndFields(int window, std::string* out) const;   // frames= dropped=
	const std::vector<float>* FrameTimes(int window) const;
	int HitchCount(int window) const;

	int Windows() const { return (int)m_frames.size(); }
	const std::vector<float>& Frames(int window) const { return m_frames[window]; }
	int Dropped(int window) const { return m_dropped[window]; }
	float HitchMaxMs(int window) const { return m_hitchMaxMs[window]; }

private:
	int m_cur;
	std::vector<std::vector<float> > m_frames;
	std::vector<int> m_dropped;
	std::vector<int> m_hitchCount;
	std::vector<float> m_hitchMaxMs;
	std::vector<int> m_fgCount;
};

// Per window: the largest drift and the frames over the drift limits. More
// than 2 s of such frames in one window aborts the run ("camera moved").
class DriftRecorder : public BenchRecorder
{
public:
	DriftRecorder() : m_cur(-1), m_abort(false) {}
	void OnRunStart(const BenchScenario& sc);
	void OnWindowStart(int window) { m_cur = window; }
	void OnFrame(const BenchFrameSample& f);
	void OnWindowEnd(int, bool) { m_cur = -1; }
	void OnRunEnd() { m_cur = -1; }
	const char* AbortReason() const { return m_abort ? "camera moved" : 0; }
	void WindowFields(int window, std::string* out) const;   // drift= input=

	float MaxUnits(int window) const { return m_w[window].maxUnits; }
	float MaxDeg(int window) const { return m_w[window].maxDeg; }
	int   OverFrames(int window) const { return m_w[window].overFrames; }

private:
	struct Window { float maxUnits, maxDeg; int overFrames; double overMs; };
	int m_cur;
	bool m_abort;
	std::vector<Window> m_w;
};

// Zone loads and unloads per window: the sum of the loaded-zone count's
// changes, sampled at the window's start, once a second during the discard
// and at its end (never during the measured part: the count walks the zone
// grid), so a load and an unload between two samples cancel. Reads the game
// through the facade, as does FrameTimeRecorder's foreground check.
class ZoneEventRecorder : public BenchRecorder
{
public:
	ZoneEventRecorder() : m_cur(-1), m_last(-1), m_sinceMs(0.0) {}
	void OnRunStart(const BenchScenario& sc);
	void OnWindowStart(int window);
	void OnFrame(const BenchFrameSample& f);
	void OnWindowEnd(int window, bool worldAlive);
	void OnRunEnd() { m_cur = -1; }
	void WindowFields(int window, std::string* out) const;   // zone=

	int Events(int window) const { return m_events[window]; }

private:
	void Sample();
	int m_cur;
	int m_last;
	double m_sinceMs;
	std::vector<int> m_events;
};
