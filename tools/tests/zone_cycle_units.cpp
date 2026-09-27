#include <cstdio>
#include "zone/preload/zone_cycle_math.h"

#include "check.h"
static bool Near(double a, double b) { double d = a - b; return d < 0.01 && d > -0.01; }

int main()
{
	// A whole ordinary cycle: 0 -> 1 -> 2 -> 3 -> 3 -> 4 -> 5 -> 0. The call
	// that advances 3 -> 4 carries the Ogre unload, the one that advances
	// 4 -> 5 the Set B publish.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleStep s;

		s = ZoneCycleSample(&m, 0, 1, 16.0, 0.2, false);
		Check(s.opened && !s.closed, "0 -> 1 opens a cycle");
		ZoneCycleNoteSetB(&m, 31);

		s = ZoneCycleSample(&m, 1, 2, 16.0, 0.3, false);
		Check(!s.opened && !s.closed, "1 -> 2 stays inside the cycle");
		s = ZoneCycleSample(&m, 2, 3, 16.0, 4.0, false);
		s = ZoneCycleSample(&m, 3, 3, 16.0, 0.1, false);   // waiting on the physics queues
		s = ZoneCycleSample(&m, 3, 4, 50.0, 0.1, false);
		s = ZoneCycleSample(&m, 4, 5, 16.0, 120.0, false);
		Check(!s.closed, "4 -> 5 does not close the cycle");
		s = ZoneCycleSample(&m, 5, 0, 16.0, 7.0, false);
		Check(s.closed && !s.opened, "5 -> 0 closes the cycle and opens nothing");

		Check(s.done.cause == ZC_CAUSE_ACTIVATE, "no save load: cause=activate");
		Check(s.done.firstPhase == 1, "first phase seen is 1");
		Check(s.done.setBAtEntry == 31, "Set B size recorded at the open");
		Check(Near(s.done.phaseMs[3], 66.0), "phase 3 dwell is both phase-3 samples");
		Check(Near(s.done.ogreUnloadMs, 0.1), "the 3 -> 4 call is the Ogre window");
		Check(Near(s.done.publishMs, 120.0), "the 4 -> 5 call is the publish window");
		Check(s.done.frames == 6, "six samples entered with the cycle open");
		Check(s.done.phaseFrames == 5, "five of them at phase 2..5");
		Check(Near(s.done.totalMs, 130.0 + 7.0), "total is the in-cycle dwell plus the closing call");
		Check(s.done.restarts == 0, "no restart");
		Check(m.desyncs == 0, "no desync");
	}

	// justLoadedAGame at the open tags the cycle, and it is read at the open:
	// the phase-5 branch clears it before the close.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 1, 0.0, 0.1, true);
		ZoneCycleSample(&m, 1, 2, 16.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 2, 0, 16.0, 0.1, false);
		Check(s.closed && s.done.cause == ZC_CAUSE_LOAD, "cause=load survives to the close");
	}

	// The phase-5 close and the tail's re-open happen in one call: 5 -> 1
	// closes the old cycle and opens a new one, which is never a save load.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 1, 0.0, 0.1, true);
		ZoneCycleSample(&m, 1, 5, 16.0, 0.1, true);
		ZoneCycleStep s = ZoneCycleSample(&m, 5, 1, 16.0, 0.2, true);
		Check(s.closed && s.opened, "5 -> 1 closes one cycle and opens the next");
		Check(s.done.cause == ZC_CAUSE_LOAD, "the closed cycle keeps its load tag");
		ZoneCycleStep t = ZoneCycleSample(&m, 1, 0, 16.0, 0.1, false);
		Check(t.closed && t.done.cause == ZC_CAUSE_ACTIVATE, "the re-opened cycle is not a load");
	}

	// The camera tail writes phase 1 with no check on the current phase, so a
	// cycle can be thrown back from 2, 3 or 4. That is a restart, not a close.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 1, 0.0, 0.1, false);
		ZoneCycleSample(&m, 1, 2, 16.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 2, 1, 16.0, 0.1, false);
		Check(!s.closed && !s.opened, "2 -> 1 stays in the same cycle");
		s = ZoneCycleSample(&m, 1, 4, 16.0, 0.1, false);
		s = ZoneCycleSample(&m, 4, 1, 16.0, 0.1, false);
		s = ZoneCycleSample(&m, 1, 0, 16.0, 0.1, false);
		Check(s.closed && s.done.restarts == 2, "both throw-backs counted");
	}

	// loadPhase1 runs in the call that leaves phase 0, so a cycle can open
	// straight at phase 2. Watching the manager leave 0 is what makes it an
	// activation rather than a cycle picked up late.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleStep s = ZoneCycleSample(&m, 0, 2, 0.0, 0.4, false);
		Check(s.opened && !s.closed, "0 -> 2 opens a cycle");
		Check(m.cur.cause == ZC_CAUSE_ACTIVATE, "leaving phase 0 is an activation, not midphase");
		Check(m.cur.firstPhase == 2, "the cycle is first seen at phase 2");
	}

	// Phases can be skipped between samples, and a cycle first seen above
	// phase 1 is tagged midphase rather than dropped.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleStep s = ZoneCycleSample(&m, 3, 5, 16.0, 0.1, false);
		Check(s.opened && !s.closed, "a cycle first seen at phase 3 is picked up");
		Check(m.cur.cause == ZC_CAUSE_MIDPHASE, "first seen at phase 3: midphase");
		s = ZoneCycleSample(&m, 5, 0, 16.0, 0.1, false);
		Check(s.closed && s.done.cause == ZC_CAUSE_MIDPHASE, "midphase tag survives");
		Check(s.done.ogreUnloadMs == 0.0 && s.done.publishMs == 0.0, "skipped edges leave no windows");
	}

	// A phase that moved outside processLoading is counted, not trusted, and
	// an out-of-range phase is refused outright.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 2, 0.0, 0.1, false);
		ZoneCycleSample(&m, 4, 5, 16.0, 0.1, false);      // 2 -> 4 happened elsewhere
		Check(m.desyncs == 1, "one desync");
		ZoneCycleStep s = ZoneCycleSample(&m, -1, 9, 16.0, 0.1, false);
		Check(!s.opened && !s.closed, "an unreadable phase is not a sample");
		Check(m.desyncs == 1, "a refused sample is not a desync");
		s = ZoneCycleSample(&m, 5, 0, 16.0, 0.1, false);
		Check(s.closed, "the cycle still closes after the refused sample");
	}

	// A cycle that never reaches phase 5 (the loader gives up at 0).
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 1, 0.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 1, 0, 16.0, 0.1, false);
		Check(s.closed && s.done.frames == 1, "a cycle that never leaves phase 1 closes at phase 0");
	}

	// Native-loading and Set B notes only reach an open cycle.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleNoteNativeLoading(&m);
		ZoneCycleNoteSetB(&m, 42);
		ZoneCycleSample(&m, 0, 1, 0.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 1, 0, 16.0, 0.1, false);
		Check(s.done.nativeLoading == 0, "a note before the open is dropped");
		Check(s.done.setBAtEntry == -1, "no Set B reading means -1");
	}

	// The wedge report fires once when a phase's cumulative dwell first
	// crosses the threshold, and not again while the cycle stays in that
	// phase.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 2, 0.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 2, 2, ZC_PHASE_WEDGE_THRESHOLD_MS - 1.0, 0.1, false);
		Check(!s.wedgeFire, "under threshold: no fire");
		s = ZoneCycleSample(&m, 2, 2, 2.0, 0.1, false);
		Check(s.wedgeFire && s.wedgePhase == 2, "crossing the threshold fires once");
		s = ZoneCycleSample(&m, 2, 2, 1000.0, 0.1, false);
		Check(!s.wedgeFire, "still wedged: latched, no repeat");
	}

	// Leaving the wedged phase clears the latch, so a later stall in a
	// different phase of the same cycle still reports.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 2, 0.0, 0.1, false);
		ZoneCycleSample(&m, 2, 2, ZC_PHASE_WEDGE_THRESHOLD_MS + 1.0, 0.1, false);  // fires on phase 2
		ZoneCycleSample(&m, 2, 3, 1.0, 0.1, false);   // still charged to phase 2: latch unchanged
		ZoneCycleStep s = ZoneCycleSample(&m, 3, 3, 1.0, 0.1, false);
		Check(!s.wedgeFire, "phase 3 has barely started: no fire yet");
		s = ZoneCycleSample(&m, 3, 3, ZC_PHASE_WEDGE_THRESHOLD_MS, 0.1, false);
		Check(s.wedgeFire && s.wedgePhase == 3, "a stall in the next phase fires again");
	}

	// A cycle close resets the latch, so the same phase can wedge again in a
	// later cycle.
	{
		ZoneCycleMachine m;
		ZoneCycleInit(&m);
		ZoneCycleSample(&m, 0, 2, 0.0, 0.1, false);
		ZoneCycleSample(&m, 2, 2, ZC_PHASE_WEDGE_THRESHOLD_MS + 1.0, 0.1, false);
		ZoneCycleSample(&m, 2, 0, 1.0, 0.1, false);   // gives up, closes the cycle
		ZoneCycleSample(&m, 0, 2, 0.0, 0.1, false);
		ZoneCycleStep s = ZoneCycleSample(&m, 2, 2, ZC_PHASE_WEDGE_THRESHOLD_MS + 1.0, 0.1, false);
		Check(s.wedgeFire && s.wedgePhase == 2, "a new cycle can wedge on the same phase again");
	}

	// Percentiles: nearest rank over what was kept, max over everything.
	{
		ZonePercentile p;
		ZonePercentileReset(&p);
		Check(ZonePercentileValue(&p, 50) == 0.0, "an empty accumulator reads 0");
		for (int i = 10; i >= 1; --i)
			ZonePercentileAdd(&p, (double)i);
		Check(p.n == 10 && p.dropped == 0, "ten kept");
		Check(Near(ZonePercentileValue(&p, 50), 5.0), "p50 of 1..10 is 5");
		Check(Near(ZonePercentileValue(&p, 90), 9.0), "p90 of 1..10 is 9");
		Check(Near(ZonePercentileValue(&p, 100), 10.0), "p100 is the largest kept");
		Check(Near(ZonePercentileValue(&p, 0), 1.0), "p0 is the smallest kept");
		Check(Near(p.max, 10.0), "max tracked");
	}
	{
		ZonePercentile p;
		ZonePercentileReset(&p);
		for (int i = 0; i < ZC_PCT_CAP + 5; ++i)
			ZonePercentileAdd(&p, 1.0);
		ZonePercentileAdd(&p, 99.0);
		Check(p.n == ZC_PCT_CAP && p.dropped == 6, "samples past the cap are counted");
		Check(Near(p.max, 99.0), "max still sees a dropped sample");
		Check(Near(ZonePercentileValue(&p, 90), 1.0), "percentiles read only what was kept");
	}

	return CheckExit("zone_cycle_units");
}
