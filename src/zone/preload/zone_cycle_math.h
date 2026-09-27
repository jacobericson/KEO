#ifndef ZONE_CYCLE_MATH_H
#define ZONE_CYCLE_MATH_H

// Pure loading-cycle bookkeeping: no game state, no Windows, host-testable.
// The caller samples the zone manager's loadingPhase either side of one
// processLoading call and feeds the pair in; everything below is arithmetic on
// those pairs.

// What opened a cycle, as far as the sampler can tell.
enum ZoneCycleCause
{
	ZC_CAUSE_LOAD = 0,   // the manager's justLoadedAGame byte was set at the open
	ZC_CAUSE_ACTIVATE,   // opened at phase 1: an activation (camera re-centre or a character)
	ZC_CAUSE_MIDPHASE    // first seen at phase 2 or above: the open itself was missed
};

const int ZC_PHASE_COUNT = 6;   // loadingPhase 0..5

// A cycle that dwells in one phase this long is a hang, not a slow load: the
// slowest healthy cycle observed (a save-load ogre unload) was 8.0 s.
const double ZC_PHASE_WEDGE_THRESHOLD_MS = 10000.0;

struct ZoneCycleTotals
{
	double phaseMs[ZC_PHASE_COUNT];  // dwell per phase, from sample to sample
	double ogreUnloadMs;             // the processLoading call that advanced 3 -> 4
	double publishMs;                // the processLoading call that advanced 4 -> 5
	double totalMs;                  // open to close
	int    cause;                    // ZoneCycleCause
	int    frames;                   // samples whose entry phase was already in the cycle
	int    phaseFrames;              // samples whose entry phase was 2..5
	int    restarts;                 // the tail reset the phase to 1 from 2 or above
	int    setBAtEntry;              // Set B size at the open (-1 = unread)
	int    nativeLoading;            // 1 = a Set B member read +176=1/+177=0 during the cycle
	int    firstPhase;               // first phase the sampler saw the cycle at
};

// What one sample did. `closed` hands back the finished cycle; `opened` means
// a cycle is now open and the caller should fill in its entry readings.
struct ZoneCycleStep
{
	bool   opened;
	bool   closed;
	ZoneCycleTotals done;
	// Cumulative dwell in the phase held over this sample just crossed
	// ZC_PHASE_WEDGE_THRESHOLD_MS for the first time since the latch was last
	// cleared. Fires at most once per phase episode; see ZoneCycleMachine.
	bool   wedgeFire;
	int    wedgePhase;
	double wedgeDwellMs;
};

struct ZoneCycleMachine
{
	bool            open;
	int             lastPhase;      // phase after the previous sample (-1 = none yet)
	int             desyncs;        // samples whose entry phase was not the last exit phase
	int             wedgeLatchPhase;// phase the wedge report already fired for (-1 = none)
	ZoneCycleTotals cur;
};

void ZoneCycleInit(ZoneCycleMachine* m);

// One processLoading call. `dwellMs` is the wall time since the previous
// sample (charged to `phaseBefore`, the phase held over it); `callMs` is how
// long the call itself took. `justLoadedAGame` is read before the call, so a
// cycle that opens here is tagged with the value the open saw.
ZoneCycleStep ZoneCycleSample(ZoneCycleMachine* m, int phaseBefore, int phaseAfter,
                              double dwellMs, double callMs, bool justLoadedAGame);

// Marks the open cycle as having seen a natively-loading Set B member.
void ZoneCycleNoteNativeLoading(ZoneCycleMachine* m);
// Records the Set B size the caller read at the open.
void ZoneCycleNoteSetB(ZoneCycleMachine* m, int setBSize);

const char* ZoneCycleCauseName(int cause);

// Fixed-capacity percentile accumulator. Samples past the cap are dropped and
// counted, so a reader can tell a full window from a representative one.
const int ZC_PCT_CAP = 128;

struct ZonePercentile
{
	double v[ZC_PCT_CAP];
	int    n;
	int    dropped;
	double max;
};

void   ZonePercentileReset(ZonePercentile* p);
void   ZonePercentileAdd(ZonePercentile* p, double value);
// Nearest-rank percentile (pct 0..100) over what was kept; 0.0 when empty.
double ZonePercentileValue(const ZonePercentile* p, int pct);

#endif // ZONE_CYCLE_MATH_H
