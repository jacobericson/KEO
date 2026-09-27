#include "zone/reset/zone_reset_fence.h"

unsigned ZoneResetLockBudgetMs(unsigned totalMs, unsigned drainWaitedMs, unsigned floorMs)
{
	unsigned left = (drainWaitedMs < totalMs) ? totalMs - drainWaitedMs : 0;
	return (left < floorMs) ? floorMs : left;
}

ZoneResetSurvivorAction ZoneResetDecideSurvivor(bool fenceComplete, bool claimed)
{
	if (fenceComplete)
		return ZONE_RESET_UNLOAD;
	return claimed ? ZONE_RESET_SKIP_CLAIMED : ZONE_RESET_UNLOAD;
}

ZoneResetAdmission ZoneResetAdmit(bool resetInProgress, bool stopSeen, ZoneResetSite)
{
	if (stopSeen)
		return ZONE_RESET_DEFER_STOP;
	if (resetInProgress)
		return ZONE_RESET_DEFER_RESET;
	return ZONE_RESET_ADMIT;
}

bool ZoneResetContentKept(const void* before, const void* now)
{
	return before != 0 && now == before;
}


namespace zone_reset_fence_detail
{
	struct UnverifiedRetirement
	{
		unsigned generation;
		short    gx;
		short    gy;
	};

	UnverifiedRetirement g_records[ZONE_RESET_FENCE_MAX];
	int      g_count      = 0;
	long     g_overflow   = 0;
	unsigned g_generation = 1;

	bool InGrid(int gx, int gy)
	{
		return gx >= 0 && gx < 64 && gy >= 0 && gy < 64;
	}

	int Find(int gx, int gy)
	{
		for (int i = 0; i < g_count; ++i)
			if (g_records[i].gx == (short)gx && g_records[i].gy == (short)gy)
				return i;
		return -1;
	}
}
using namespace zone_reset_fence_detail;

unsigned ZoneResetFenceGeneration()
{
	return g_generation;
}

void ZoneResetFenceAdvanceGeneration()
{
	g_generation++;
}

bool ZoneResetFenceNoteUnverified(int gx, int gy, unsigned generation)
{
	if (!InGrid(gx, gy))
		return false;
	int i = Find(gx, gy);
	if (i >= 0)
	{
		g_records[i].generation = generation;
		return true;
	}
	if (g_count >= ZONE_RESET_FENCE_MAX)
	{
		g_overflow++;
		return false;
	}
	g_records[g_count].generation = generation;
	g_records[g_count].gx = (short)gx;
	g_records[g_count].gy = (short)gy;
	g_count++;
	return true;
}

bool ZoneResetFenceHolds(int gx, int gy, unsigned currentGeneration)
{
	int i = Find(gx, gy);
	return i >= 0 && g_records[i].generation < currentGeneration;
}

bool ZoneResetFenceClear(int gx, int gy)
{
	int i = Find(gx, gy);
	if (i < 0)
		return false;
	g_count--;
	if (i < g_count)
		g_records[i] = g_records[g_count];
	return true;
}

int ZoneResetFenceCount()
{
	return g_count;
}

long ZoneResetFenceOverflow()
{
	return g_overflow;
}

void ZoneResetFenceReset()
{
	g_count      = 0;
	g_overflow   = 0;
	g_generation = 1;
}
