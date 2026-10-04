// formation_pace_policy.cpp - The gather pacing's pure decisions and the factor table's sequence.
// Any thread; no lock, no allocation.
#include <intrin.h>
#include <math.h>
#include "movement/formation_pace_policy.h"

#pragma intrinsic(_ReadWriteBarrier, _InterlockedIncrement)

float FormationPaceFactor(float distSq, float maxDistSq, float gatherRadiusSq)
{
	if (!(distSq > gatherRadiusSq) || !(maxDistSq > 0.0f))
		return 1.0f;
	float f = sqrtf(distSq / maxDistSq);
	if (!(f >= PACE_MIN_FACTOR))
		return PACE_MIN_FACTOR;
	return f > 1.0f ? 1.0f : f;
}

bool FormationPaceWalksGather(float destX, float destZ, float gatherX, float gatherZ)
{
	float dx = destX - gatherX, dz = destZ - gatherZ;
	return dx * dx + dz * dz <= PACE_GATHER_MATCH * PACE_GATHER_MATCH;
}

float FormationPaceMaxDistSq(const float* distSq, int n)
{
	float best = -1.0f;
	for (int i = 0; distSq && i < n; ++i)
		if (distSq[i] >= 0.0f && distSq[i] > best)
			best = distSq[i];
	return best;
}

double FormationGatherSpread(const double* arrive, int n)
{
	double first = 0.0, last = 0.0;
	int seen = 0;
	for (int i = 0; arrive && i < n; ++i)
	{
		if (!(arrive[i] > 0.0))
			continue;
		if (seen == 0 || arrive[i] < first)
			first = arrive[i];
		if (seen == 0 || arrive[i] > last)
			last = arrive[i];
		++seen;
	}
	return seen >= 2 ? last - first : 0.0;
}

void PaceTablePublish(PaceTable* t, const PaceEntry* e, int n)
{
	if (n < 0 || !e)
		n = 0;
	if (n > PACE_TABLE_MAX)
		n = PACE_TABLE_MAX;
	_InterlockedIncrement(&t->seq);   // odd: writing
	_ReadWriteBarrier();
	t->count = n;
	for (int i = 0; i < n; ++i)
		t->entries[i] = e[i];
	_ReadWriteBarrier();
	_InterlockedIncrement(&t->seq);   // even: whole
}

float PaceTableRead(const PaceTable* t, size_t character)
{
	if (!character)
		return 1.0f;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		long seq1 = t->seq;
		if (seq1 & 1)
			continue;
		_ReadWriteBarrier();
		int n = t->count;
		float factor = 1.0f;
		for (int i = 0; i < n && i < PACE_TABLE_MAX; ++i)
		{
			if (t->entries[i].character == character)
			{
				factor = t->entries[i].factor;
				break;
			}
		}
		_ReadWriteBarrier();
		if (t->seq == seq1)
			return factor;
	}
	return 1.0f;
}
