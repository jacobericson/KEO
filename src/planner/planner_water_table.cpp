// planner_water_table.cpp - The per-player-character water table and the path request's water
// write. The main thread publishes the table under a sequence word, odd while it writes; the
// requesting thread (main or AI back thread) reads it without a lock, at most twice, and writes the
// fresh request's water field before the request is queued, while no other thread holds the request.
// No allocation, no lock, no log.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include "planner/planner_water_table.h"
#include "planner/plan_policy.h"

namespace planner_water_table_detail {

struct WaterTable
{
	volatile LONG           seq;
	int                     count;
	planner::PlanWaterEntry entries[planner::PLAN_WATER_TABLE_MAX];
};

} // namespace planner_water_table_detail
using namespace planner_water_table_detail;

namespace planner {

static WaterTable    s_table    = { 0, 0, { { 0, 0.0f } } };
static volatile LONG s_live     = 0;
static volatile LONG s_mode     = PWC_FLOOR;
static volatile LONG s_writes   = 0;
static volatile LONG s_leaves   = 0;
static volatile LONG s_lastBits = 0;
static __declspec(thread) uintptr_t t_requester = 0;
static void (*s_pauseInPublish)(void* ctx) = NULL;
static void* s_pauseCtx = NULL;

void PlannerWaterTableArm(int live, int mode)
{
	InterlockedExchange(&s_mode, mode);
	InterlockedExchange(&s_live, live ? 1 : 0);
}

int PlannerWaterTableLive()
{
	return InterlockedCompareExchange(&s_live, 0, 0) != 0 ? 1 : 0;
}

void PlannerWaterTablePublish(const PlanWaterEntry* e, int n)
{
	if (!e || n < 0)
		n = 0;
	if (n > PLAN_WATER_TABLE_MAX)
		n = PLAN_WATER_TABLE_MAX;
	InterlockedIncrement(&s_table.seq);
	_ReadWriteBarrier();
	s_table.count = n;
	for (int i = 0; i < n; ++i)
		s_table.entries[i] = e[i];
	if (s_pauseInPublish)
		s_pauseInPublish(s_pauseCtx);
	_ReadWriteBarrier();
	InterlockedIncrement(&s_table.seq);
}

void PlannerWaterTableClear()
{
	PlannerWaterTablePublish(NULL, 0);
}

float PlannerWaterTableFind(uintptr_t havokChar)
{
	if (!havokChar)
		return 0.0f;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG seq1 = s_table.seq;
		if (seq1 & 1)
			continue;
		_ReadWriteBarrier();
		int count = s_table.count;
		float mult = 0.0f;
		for (int i = 0; i < count && i < PLAN_WATER_TABLE_MAX; ++i)
		{
			if (s_table.entries[i].havokChar == havokChar)
			{
				mult = s_table.entries[i].mult;
				break;
			}
		}
		_ReadWriteBarrier();
		if (s_table.seq == seq1)
			return mult;
	}
	return 0.0f;
}

void PlannerWaterNoteRequester(void* havokChar)
{
	if (!s_live)
		return;
	t_requester = (uintptr_t)havokChar;
}

void PlannerWaterOnSubmit(float* waterField)
{
	if (!s_live)
		return;
	uintptr_t requester = t_requester;
	float v = (waterField && requester)
	        ? PlanWaterRequestValue((int)s_mode, PlannerWaterTableFind(requester), *waterField)
	        : 0.0f;
	if (v == 0.0f)
	{
		InterlockedIncrement(&s_leaves);
		return;
	}
	*waterField = v;
	InterlockedIncrement(&s_writes);
	LONG bits;
	memcpy(&bits, &v, sizeof(bits));
	InterlockedExchange(&s_lastBits, bits);
}

void PlannerWaterTableStatsGet(PlanWaterTableStats* out)
{
	out->writes = InterlockedCompareExchange(&s_writes, 0, 0);
	out->leaves = InterlockedCompareExchange(&s_leaves, 0, 0);
	out->entries = s_table.count;
	LONG bits = InterlockedCompareExchange(&s_lastBits, 0, 0);
	memcpy(&out->last, &bits, sizeof(out->last));
}

void PlannerWaterTableTestPauseInPublish(void (*fn)(void* ctx), void* ctx)
{
	s_pauseInPublish = fn;
	s_pauseCtx = ctx;
}

} // namespace planner
