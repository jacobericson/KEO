// nm_force_rebuild_policy.cpp - see nm_force_rebuild_policy.h. Pure.
#include "navmesh/cache/nm_force_rebuild_policy.h"
#include <math.h>
#include <string.h>

long NmMarkNextSeq(long seq)
{
	return (seq <= 0 || seq >= NM_MARK_SEQ_MAX) ? 1 : seq + 1;
}

NmMarkClaim NmMarkClaimDecide(long word, double ageSec, int jobType)
{
	if (jobType != 0 || NmMarkStateOf(word) != NM_MARK_MARKED)
		return NM_CLAIM_NONE;
	return ageSec > NM_REBUILD_MARK_TTL_SEC ? NM_CLAIM_EXPIRED : NM_CLAIM_CONSUME;
}

NmMarkPress NmMarkPressDecide(long word, double ageSec)
{
	return (NmMarkStateOf(word) == NM_MARK_CLAIMED && ageSec <= NM_REBUILD_MARK_TTL_SEC) ? NM_PRESS_KEEP : NM_PRESS_MARK;
}

bool NmMarkPressFinished(long pressWord, long nowWord)
{
	if (NmMarkSeqOf(nowWord) != NmMarkSeqOf(pressWord))
		return true;
	const int state = NmMarkStateOf(nowWord);
	return state == NM_MARK_NONE || state == NM_MARK_DONE;
}

bool NmMarkWantsFront(long word, double ageSec, int jobType)
{
	return jobType == 0 && NmMarkStateOf(word) == NM_MARK_MARKED && ageSec <= NM_REBUILD_MARK_TTL_SEC;
}

static int EdgeStep(float frac)
{
	if (frac < NM_REBUILD_EDGE_FRACTION)
		return -1;
	if (frac > 1.0f - NM_REBUILD_EDGE_FRACTION)
		return 1;
	return 0;
}

static void AddNeighbour(NmRebuildSelection* out, int gx, int gy)
{
	if (gx < 0 || gx >= NM_REBUILD_GRID || gy < 0 || gy >= NM_REBUILD_GRID)
	{
		++out->dropped;
		return;
	}
	out->n[out->count].gx = gx;
	out->n[out->count].gy = gy;
	++out->count;
}

void NmRebuildSelect(float worldX, float worldZ, float sizeX, float sizeZ, int cellX, int cellY,
                     NmRebuildSelection* out)
{
	memset(out, 0, sizeof(*out));
	if (!(sizeX > 0.0f) || !(sizeZ > 0.0f))
		return;
	const float qx = (sizeX * 32.0f + worldX) / sizeX;
	const float qz = (sizeZ * 32.0f + worldZ) / sizeZ;
	const float fx = floorf(qx);
	const float fz = floorf(qz);
	if (!(fx >= 0.0f && fx < (float)NM_REBUILD_GRID && fz >= 0.0f && fz < (float)NM_REBUILD_GRID))
		return;
	if ((int)fx != cellX || (int)fz != cellY)
		return;
	out->matched = true;
	out->fracX = qx - fx;
	out->fracZ = qz - fz;
	const int dx = EdgeStep(out->fracX);
	const int dz = EdgeStep(out->fracZ);
	if (dx)
		AddNeighbour(out, cellX + dx, cellY);
	if (dz)
		AddNeighbour(out, cellX, cellY + dz);
	if (dx && dz)
		AddNeighbour(out, cellX + dx, cellY + dz);
}

NmRebuildSkip NmRebuildEligible(bool haveZone, bool accessible, bool beingLoaded, bool content, bool terrain)
{
	if (!haveZone)    return NM_SKIP_NO_ZONE;
	if (beingLoaded)  return NM_SKIP_PRIVATE;
	if (!accessible)  return NM_SKIP_NOT_ACCESSIBLE;
	if (!content)     return NM_SKIP_NO_CONTENT;
	if (!terrain)     return NM_SKIP_NO_TERRAIN;
	return NM_SKIP_NONE;
}

const char* NmRebuildSkipName(int skip)
{
	switch (skip)
	{
	case NM_SKIP_NONE:           return "none";
	case NM_SKIP_NO_ZONE:        return "noZone";
	case NM_SKIP_PRIVATE:        return "private";
	case NM_SKIP_NOT_ACCESSIBLE: return "notAccessible";
	case NM_SKIP_NO_CONTENT:     return "noContent";
	case NM_SKIP_NO_TERRAIN:     return "noTerrain";
	default:                     return "?";
	}
}

bool NmRebuildIsKeyCaller(const unsigned __int64* offsets, int n, unsigned __int64 keyRet)
{
	for (int i = 0; i < n; ++i)
		if (offsets[i] == keyRet)
			return true;
	return false;
}

bool NmHoldSuppresses(bool active, bool onMainThread, double sincePressSec)
{
	return active && !onMainThread && sincePressSec >= 0.0 && sincePressSec < NM_REBUILD_HOLD_CAP_SEC;
}

NmHoldVerdict NmHoldDecide(bool active, int unfinished, double sincePressSec)
{
	if (!active)
		return NM_HOLD_IDLE;
	if (unfinished <= 0)
		return NM_HOLD_DONE;
	if (sincePressSec >= NM_REBUILD_HOLD_CAP_SEC)
		return NM_HOLD_CAPPED;
	return NM_HOLD_PENDING;
}

bool NmHoldReleaseOwes(NmHoldVerdict v, bool heldOwed, bool pressShowOpen)
{
	const bool released = v == NM_HOLD_DONE || v == NM_HOLD_CAPPED;
	return released && (heldOwed || pressShowOpen);
}

bool NmHoldReplayNow(bool owed, bool haveZoneManager, int loadingPhase, bool saveLoading)
{
	return owed && haveZoneManager && loadingPhase == 0 && !saveLoading;
}
