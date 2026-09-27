#include "movement/formation_members.h"
#include <math.h>

int FormationZeroMatchingMembers(uintptr_t* characters, uintptr_t* movements,
                                  int count, const uintptr_t* chars, int n)
{
	int zeroed = 0;
	for (int i = 0; i < count; ++i)
	{
		if (!characters[i]) continue;
		for (int c = 0; c < n; ++c)
		{
			if (characters[i] == chars[c])
			{
				characters[i] = 0;
				movements[i] = 0;
				++zeroed;
				break;
			}
		}
	}
	return zeroed;
}

bool FormationGroupExhausted(const uintptr_t* characters, int count)
{
	for (int i = 0; i < count; ++i)
		if (characters[i]) return false;
	return true;
}

bool FormationSameMemberSet(const uintptr_t* characters, int count,
                             const uintptr_t* chars, int n)
{
	int liveCount = 0;
	for (int i = 0; i < count; ++i)
		if (characters[i]) ++liveCount;
	if (liveCount != n) return false;

	for (int i = 0; i < count; ++i)
	{
		if (!characters[i]) continue;
		bool found = false;
		for (int c = 0; c < n; ++c)
		{
			if (characters[i] == chars[c]) { found = true; break; }
		}
		if (!found) return false;
	}
	return true;
}

bool FormationMemberNewlyHeld(bool holdAtCreation, bool holdNow,
                               bool inSomethingAtCreation, bool inSomethingNow)
{
	if (holdNow && !holdAtCreation) return true;
	if (inSomethingNow && !inSomethingAtCreation) return true;
	return false;
}

float FormationGatherRadius(int memberCount)
{
	if (memberCount < 1) memberCount = 1;
	float r = sqrtf((float)memberCount * 60.0f) + 10.0f;
	return r < 40.0f ? 40.0f : r;
}

float FormationMaxPairwiseSpread(const float* xs, const float* zs,
                                  const uintptr_t* characters, int count)
{
	if (!xs || !zs || !characters || count < 2) return 0.0f;
	float worstSq = 0.0f;
	for (int i = 0; i < count; ++i)
	{
		if (!characters[i]) continue;
		for (int j = i + 1; j < count; ++j)
		{
			if (!characters[j]) continue;
			float dx = xs[i] - xs[j];
			float dz = zs[i] - zs[j];
			float d2 = dx * dx + dz * dz;
			if (d2 > worstSq) worstSq = d2;
		}
	}
	return worstSq > 0.0f ? sqrtf(worstSq) : 0.0f;
}
