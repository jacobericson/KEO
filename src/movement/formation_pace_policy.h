// formation_pace_policy.h - The gather pacing's decisions, pure: a gathering member's speed factor
// from its distance to the gather point and the farthest member's, the farthest distance over the
// members a poll paces, the spread of their arrivals, and the published factor table's write and
// lock-free read. No game or Windows header; the table's writer is the main thread, its reader any
// thread.
#ifndef KEO_FORMATION_PACE_POLICY_H
#define KEO_FORMATION_PACE_POLICY_H

#include <stddef.h>

const float PACE_MIN_FACTOR = 0.5f;   // a paced member never runs slower than this share of the group
const int   PACE_TABLE_MAX  = 64;     // characters one publish carries; a member past it is not paced

// distSq: the member's x-z distance to the gather point, squared; maxDistSq: the farthest paced
// member's; gatherRadiusSq: the group's completion radius, squared. 1 inside the radius or with no
// positive farthest distance; otherwise sqrt(distSq / maxDistSq), clamped to [PACE_MIN_FACTOR, 1].
float FormationPaceFactor(float distSq, float maxDistSq, float gatherRadiusSq);
// The largest of distSq[0..n) at or above 0 (a negative entry is a member this poll does not pace);
// -1 when there is none.
float FormationPaceMaxDistSq(const float* distSq, int n);
// The seconds between the first and the last arrival in arrive[0..n), counting only entries above 0;
// 0 when fewer than two arrived.
double FormationGatherSpread(const double* arrive, int n);

struct PaceEntry { size_t character; float factor; };
struct PaceTable
{
	volatile long seq;   // odd while a publish writes, even when the entries are whole
	int           count;
	PaceEntry     entries[PACE_TABLE_MAX];
};
// One writer (the main thread): n clamped to [0, PACE_TABLE_MAX], the entries written between two
// increments of seq.
void  PaceTablePublish(PaceTable* t, const PaceEntry* e, int n);
// Any thread, lock-free, at most two attempts: the factor published for character; 1 when character
// is 0, when it is absent, or when both attempts meet a publish in progress.
float PaceTableRead(const PaceTable* t, size_t character);

#endif // KEO_FORMATION_PACE_POLICY_H
