#include "zone/preload/zone_cycle_math.h"

static void ClearTotals(ZoneCycleTotals* t)
{
	for (int i = 0; i < ZC_PHASE_COUNT; ++i)
		t->phaseMs[i] = 0.0;
	t->ogreUnloadMs = 0.0;
	t->publishMs    = 0.0;
	t->totalMs      = 0.0;
	t->cause        = ZC_CAUSE_ACTIVATE;
	t->frames       = 0;
	t->phaseFrames  = 0;
	t->restarts     = 0;
	t->setBAtEntry  = -1;
	t->nativeLoading = 0;
	t->firstPhase   = 0;
}

void ZoneCycleInit(ZoneCycleMachine* m)
{
	m->open            = false;
	m->lastPhase       = -1;
	m->desyncs         = 0;
	m->wedgeLatchPhase = -1;
	ClearTotals(&m->cur);
}

static bool PhaseInRange(int phase)
{
	return phase >= 0 && phase < ZC_PHASE_COUNT;
}

// `sawIdle` means the sampler watched the manager leave phase 0, so the open
// is a real one however far the same call then carried the phase (loadPhase1
// reaches phase 2 in the call that leaves 0).
static void OpenCycle(ZoneCycleMachine* m, int phase, bool justLoadedAGame, bool sawIdle)
{
	ClearTotals(&m->cur);
	m->open = true;
	m->wedgeLatchPhase = -1;
	m->cur.firstPhase = phase;
	if (justLoadedAGame)
		m->cur.cause = ZC_CAUSE_LOAD;
	else if (sawIdle || phase <= 1)
		m->cur.cause = ZC_CAUSE_ACTIVATE;
	else
		m->cur.cause = ZC_CAUSE_MIDPHASE;
}

ZoneCycleStep ZoneCycleSample(ZoneCycleMachine* m, int phaseBefore, int phaseAfter,
                              double dwellMs, double callMs, bool justLoadedAGame)
{
	ZoneCycleStep step;
	step.opened      = false;
	step.closed      = false;
	step.wedgeFire   = false;
	step.wedgePhase  = -1;
	step.wedgeDwellMs = 0.0;
	ClearTotals(&step.done);

	if (!PhaseInRange(phaseBefore) || !PhaseInRange(phaseAfter))
		return step;                      // an unreadable phase is not a sample

	if (m->lastPhase >= 0 && phaseBefore != m->lastPhase)
		m->desyncs++;                     // the phase moved outside processLoading

	// A cycle already in progress when the sampler starts, or one whose open
	// frame was missed, is picked up here rather than lost.
	if (!m->open && phaseBefore != 0)
	{
		OpenCycle(m, phaseBefore, justLoadedAGame, false);
		step.opened = true;
	}

	if (m->open)
	{
		m->cur.phaseMs[phaseBefore] += dwellMs;
		m->cur.totalMs += dwellMs;
		m->cur.frames++;
		if (phaseBefore >= 2)
			m->cur.phaseFrames++;

		// A prior wedge report was for a different phase (progress since, or a
		// restart): its latch no longer applies to the phase dwelling now.
		if (m->wedgeLatchPhase != phaseBefore)
			m->wedgeLatchPhase = -1;
		if (m->wedgeLatchPhase < 0 && m->cur.phaseMs[phaseBefore] > ZC_PHASE_WEDGE_THRESHOLD_MS)
		{
			m->wedgeLatchPhase = phaseBefore;
			step.wedgeFire     = true;
			step.wedgePhase    = phaseBefore;
			step.wedgeDwellMs  = m->cur.phaseMs[phaseBefore];
		}
	}

	// The branch that advances the phase does its work inside this call, so the
	// call's own duration is that edge's window.
	if (phaseBefore == 3 && phaseAfter == 4)
		m->cur.ogreUnloadMs = callMs;
	else if (phaseBefore == 4 && phaseAfter == 5)
		m->cur.publishMs = callMs;

	// The tail re-centres on the camera and writes phase 1 with no check on the
	// current phase, so a cycle can be thrown back mid-flight.
	if (m->open && phaseAfter == 1 && phaseBefore >= 2 && phaseBefore != 5)
		m->cur.restarts++;

	// Phase 5 is the last phase of a cycle. Leaving it closes the cycle, even
	// when the same call's tail has already opened the next one at phase 1.
	bool leavingFive = (phaseBefore == 5 && phaseAfter != 5);
	if (m->open && (phaseAfter == 0 || leavingFive))
	{
		m->cur.totalMs += callMs;
		step.closed = true;
		step.done   = m->cur;
		m->open     = false;
		m->wedgeLatchPhase = -1;
		ClearTotals(&m->cur);
	}

	if (!m->open && phaseAfter != 0)
	{
		// A close and a re-open in one call: the new cycle cannot be a save
		// load, because the phase-5 branch clears justLoadedAGame before it.
		OpenCycle(m, phaseAfter, step.closed ? false : justLoadedAGame, !step.closed);
		step.opened = true;
	}

	m->lastPhase = phaseAfter;
	return step;
}

void ZoneCycleNoteNativeLoading(ZoneCycleMachine* m)
{
	if (m->open)
		m->cur.nativeLoading = 1;
}

void ZoneCycleNoteSetB(ZoneCycleMachine* m, int setBSize)
{
	if (m->open)
		m->cur.setBAtEntry = setBSize;
}

const char* ZoneCycleCauseName(int cause)
{
	switch (cause)
	{
	case ZC_CAUSE_LOAD:     return "load";
	case ZC_CAUSE_ACTIVATE: return "activate";
	case ZC_CAUSE_MIDPHASE: return "midphase";
	default:                return "?";
	}
}

void ZonePercentileReset(ZonePercentile* p)
{
	p->n = 0;
	p->dropped = 0;
	p->max = 0.0;
}

void ZonePercentileAdd(ZonePercentile* p, double value)
{
	if (p->n == 0 || value > p->max)
		p->max = value;
	if (p->n < ZC_PCT_CAP)
		p->v[p->n++] = value;
	else
		p->dropped++;
}

double ZonePercentileValue(const ZonePercentile* p, int pct)
{
	if (p->n <= 0)
		return 0.0;
	if (pct < 0)   pct = 0;
	if (pct > 100) pct = 100;

	double sorted[ZC_PCT_CAP];
	for (int i = 0; i < p->n; ++i)
		sorted[i] = p->v[i];
	for (int i = 1; i < p->n; ++i)          // insertion sort: n <= 128
	{
		double key = sorted[i];
		int j = i - 1;
		while (j >= 0 && sorted[j] > key)
		{
			sorted[j + 1] = sorted[j];
			--j;
		}
		sorted[j + 1] = key;
	}

	int rank = (pct * p->n + 99) / 100;      // nearest rank, 1-based
	if (rank < 1)     rank = 1;
	if (rank > p->n)  rank = p->n;
	return sorted[rank - 1];
}
