#include "zone/preload/preload_pipe_stats.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "navmesh/generation/nm_misspar.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "movement/islands.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/preload/preload_internal.h"
#include <cstdio>     // _snprintf_s (hook_resetUnloadZones builds its line without CRT streams)
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX only; the function is resolved at runtime


// =========================================================================
// Preload pipeline latency instrumentation
// =========================================================================
// Per-window sample sets for the 5 latency series in the "PreloadPipe:" line.
// Fixed capacity, main-thread only, no STL: a window prints and resets every
// PIPE_STATS_INTERVAL seconds. Bounded by preload throughput (~1 zone/2s per
// the design doc), so 64 is generous headroom for a 10-30s window; a window
// that saturates it just caps the sample (percentiles then read low -- this
// is diagnostic-only, not a release gate). PIPE_SAMPLE_CAP and PipeSampleSet
// are declared in preload_internal.h: preload_queue.cpp and
// preload_prepare.cpp fill g_pipe's counters too.
PipeSampleSet g_pipe;
static double g_pipeWindowStart = 0.0;

#ifdef ZONEOPT_DEBUG
static const double PIPE_STATS_INTERVAL = 10.0;  // DEV
#else
static const double PIPE_STATS_INTERVAL = 30.0;  // PROD
#endif

void PipeSampleAdd(double* arr, int* n, double ms)
{
	if (*n < PIPE_SAMPLE_CAP)
		arr[(*n)++] = ms;
}

static void PipeSortAscending(double* arr, int n)
{
	for (int i = 1; i < n; ++i)
	{
		double v = arr[i];
		int j = i - 1;
		while (j >= 0 && arr[j] > v) { arr[j + 1] = arr[j]; --j; }
		arr[j + 1] = v;
	}
}

// Floor-rank percentiles: the value at sorted rank floor(frac*n).
static void PipeAppendPercentiles(std::ostringstream& ss, double* arr, int n)
{
	if (n <= 0) { ss << "-/-/-"; return; }
	double sorted[PIPE_SAMPLE_CAP];
	for (int i = 0; i < n; ++i) sorted[i] = arr[i];
	PipeSortAscending(sorted, n);
	int p50i = (int)(0.5 * n); if (p50i >= n) p50i = n - 1;
	int p90i = (int)(0.9 * n); if (p90i >= n) p90i = n - 1;
	ss << std::fixed << std::setprecision(1)
	   << sorted[p50i] << "/" << sorted[p90i] << "/" << sorted[n - 1];
}

static void PrintPipeStats()
{
	// Skip the line entirely when every
	// counter in the window is zero (preload idle this window) -- a
	// PIPE_STATS_INTERVAL-cadence line of all zeros/dashes was pure log
	// volume with nothing to grep.
	if (g_pipe.loaded == 0 && g_pipe.registered == 0 &&
	    g_pipe.readyEmpty == 0 && g_pipe.notReadyAtCall == 0 &&
	    g_pipe.thingsAfter0 == 0 && g_pipe.thingsAfter1 == 0 &&
	    g_pipe.toReadyN == 0 && g_pipe.to264N == 0 && g_pipe.toRegN == 0 &&
	    g_pipe.procContentN == 0)
		return;

	std::ostringstream ss;
	ss << "PreloadPipe: loaded=" << g_pipe.loaded
	   << " registered=" << g_pipe.registered
	   << " toReady "; PipeAppendPercentiles(ss, g_pipe.toReadyMs, g_pipe.toReadyN);
	ss << " ms to264 "; PipeAppendPercentiles(ss, g_pipe.to264Ms, g_pipe.to264N);
	ss << " ms toReg "; PipeAppendPercentiles(ss, g_pipe.toRegMs, g_pipe.toRegN);
	ss << " ms procContent " << g_pipe.procContentN << " ";
	PipeAppendPercentiles(ss, g_pipe.procContentMs, g_pipe.procContentN);
	ss << " ms readyEmpty=" << g_pipe.readyEmpty
	   << " notReadyAtCall=" << g_pipe.notReadyAtCall
	   << " thingsAfter0=" << g_pipe.thingsAfter0
	   << " thingsAfter1=" << g_pipe.thingsAfter1;
	LogMsg(ss.str());
}


// =========================================================================
// Per-transition benchmark (screen only)
// =========================================================================
// This file detects the loading-screen dismissal (isTransitionActive going
// false between frames, transition.h) and logs a "screen=<ms>ms" line, the
// "Transition: ... ms" duration restated so the line stands alone.
static bool h15WasTransitionActive = false;

static void H15Tick()
{
	bool active = isTransitionActive;
	if (h15WasTransitionActive && !active)
	{
		std::ostringstream ss;
		ss << "H15: screen=" << std::fixed << std::setprecision(1)
		   << QPCToMs(transitionStartTime, transitionEndQpc) << "ms";
		LogMsg(ss.str());
	}
	h15WasTransitionActive = active;
}

void H15Reset()
{
	// Full/save-load reset: resync the edge detector to the flag's current
	// value, so a reset never reads as a dismissal.
	h15WasTransitionActive = isTransitionActive;
}

// Called every main-thread frame from PreloadCheckSaveLoad (the only
// preload entry camera_zone_hook.cpp calls unconditionally every frame), so the
// periodic print and the dismissal edge detector do not need a new call
// site in camera_zone_hook.cpp.
void PreloadStep1Tick(void* zoneMgr)
{
	double now = ElapsedSec();
	if (now - g_pipeWindowStart >= PIPE_STATS_INTERVAL)
	{
		PrintPipeStats();
		memset(&g_pipe, 0, sizeof(g_pipe));
		g_pipeWindowStart = now;
	}
	(void)zoneMgr;
	H15Tick();
}
