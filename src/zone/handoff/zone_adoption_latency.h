#pragma once
// Adoption latency in two legs plus the total. "Ready" is
// the policy-clock time a cell entered ReadyForAdoption; "admitted" is when
// the cohort inserted it into Set A; "active" is when the game published it
// (NativeActive). Main-thread only: every timestamp fed in comes from a
// main-thread state transition in zone_prep_ledger.h.

#include "zone/preload/zone_cycle_math.h"   // ZonePercentile

struct ZoneAdoptionLatency
{
	ZonePercentile readyToAdmitted;
	ZonePercentile admittedToActive;
	ZonePercentile readyToActive;
};

void ZoneAdoptionLatencyInit(ZoneAdoptionLatency* a);

// Records one cell's completed adoption. Timestamps must be non-decreasing
// (readyAt <= admittedAt <= activeAt); a caller with a broken sample should
// not call this rather than pass negative durations.
//
// Open question: what to record for a cell whose journey
// is cut short before reaching NativeActive (retired from NativeA or
// NativeBLoading, or geometry-invalidated after ReadyForAdoption). Nothing
// in the design specifies a convention for a partial journey, and none is
// guessed here; do not call this function with a synthesized `activeAt` for
// such a cell.
void ZoneAdoptionLatencyRecord(ZoneAdoptionLatency* a, double readyAt, double admittedAt, double activeAt);

// Convenience accessor for section 9's headline number.
double ZoneAdoptionLatencyP99ReadyToActive(const ZoneAdoptionLatency* a);

// The two legs that make up the headline number, reported separately because
// they answer different questions: readyToAdmitted is how long the mod holds
// a cell privately before the cohort admits it (the mod's own backlog),
// admittedToActive is how long the native state machine then takes to carry
// an admitted cell to NativeActive (whether Set B membership actually
// completes once granted). A large combined p99 with a small readyToAdmitted
// points at the native machine, not the mod's admission cadence.
double ZoneAdoptionLatencyP99ReadyToAdmitted(const ZoneAdoptionLatency* a);
double ZoneAdoptionLatencyP99AdmittedToActive(const ZoneAdoptionLatency* a);

// Every leg is recorded from the same completed-journey event, so all three
// share one sample count; callers print this once, alongside any of the
// three percentiles, so "0ms" and "no samples" are distinguishable.
int ZoneAdoptionLatencySampleCount(const ZoneAdoptionLatency* a);
