// The per-player-character water table and the request write: publish and find, the sequence word,
// the cap, the clear, the lever, the requester hand-off.
#include <cstdio>
#include <cstring>
#include "planner/planner_water_table.h"
#include "planner/plan_policy.h"

#include "check.h"

using namespace planner;

static const uintptr_t kHcA   = 0x1000;
static const uintptr_t kHcB   = 0x2000;
static const uintptr_t kHcNpc = 0x3000;

static float s_pausedRead = -1.0f;

static void ReadInsidePublish(void* ctx)
{
	s_pausedRead = PlannerWaterTableFind(*(const uintptr_t*)ctx);
}

static void CheckTable()
{
	PlanWaterEntry e[2] = { { kHcA, 1.6f }, { kHcB, 9.5f } };
	PlannerWaterTablePublish(e, 2);
	CHECK(PlannerWaterTableFind(kHcA) == 1.6f && PlannerWaterTableFind(kHcB) == 9.5f,
	      "water table: a published character reads its multiplier");
	CHECK(PlannerWaterTableFind(kHcNpc) == 0.0f && PlannerWaterTableFind(0) == 0.0f,
	      "water table: a character never published reads no value");

	uintptr_t probe = kHcA;
	PlannerWaterTableTestPauseInPublish(ReadInsidePublish, &probe);
	PlannerWaterTablePublish(e, 2);
	PlannerWaterTableTestPauseInPublish(NULL, NULL);
	CHECK(s_pausedRead == 0.0f, "water table: a read inside a publish reads no value");

	static PlanWaterEntry many[PLAN_WATER_TABLE_MAX + 5];
	for (int i = 0; i < PLAN_WATER_TABLE_MAX + 5; ++i)
	{
		many[i].havokChar = 0x10000 + (uintptr_t)i * 16;
		many[i].mult = 2.0f;
	}
	PlannerWaterTablePublish(many, PLAN_WATER_TABLE_MAX + 5);
	PlanWaterTableStats st;
	PlannerWaterTableStatsGet(&st);
	CHECK(st.entries == PLAN_WATER_TABLE_MAX && PlannerWaterTableFind(many[PLAN_WATER_TABLE_MAX - 1].havokChar) == 2.0f
	      && PlannerWaterTableFind(many[PLAN_WATER_TABLE_MAX].havokChar) == 0.0f,
	      "water table: a publish past the cap keeps the first 200");

	PlannerWaterTableClear();
	PlannerWaterTableStatsGet(&st);
	CHECK(st.entries == 0 && PlannerWaterTableFind(many[0].havokChar) == 0.0f, "water table: a clear empties the table");
}

static void CheckRequest()
{
	PlanWaterEntry e[1] = { { kHcA, 1.6f } };
	PlannerWaterTablePublish(e, 1);
	PlanWaterTableStats before, after;

	PlannerWaterTableArm(1, PWC_DYNAMIC);
	PlannerWaterNoteRequester((void*)kHcA);
	float field = 5.0f;
	PlannerWaterTableStatsGet(&before);
	PlannerWaterOnSubmit(&field);
	PlannerWaterTableStatsGet(&after);
	CHECK(field == 1.6f && after.writes == before.writes + 1 && after.last == 1.6f,
	      "water request: a published requester's request takes its multiplier");

	PlannerWaterNoteRequester((void*)kHcNpc);
	field = 10.0f;
	PlannerWaterOnSubmit(&field);
	CHECK(field == 10.0f, "water request: an unpublished requester leaves the request");

	PlannerWaterNoteRequester(NULL);
	field = 5.0f;
	PlannerWaterTableStatsGet(&before);
	PlannerWaterOnSubmit(&field);
	PlannerWaterTableStatsGet(&after);
	CHECK(field == 5.0f && after.leaves == before.leaves + 1, "water request: no requester leaves the request");

	PlannerWaterNoteRequester((void*)kHcA);
	PlannerWaterTableArm(0, PWC_DYNAMIC);
	field = 5.0f;
	PlannerWaterTableStatsGet(&before);
	PlannerWaterOnSubmit(&field);
	PlannerWaterTableStatsGet(&after);
	CHECK(field == 5.0f && after.writes == before.writes && after.leaves == before.leaves,
	      "water request: with the lever off the request keeps the engine's value");

	PlannerWaterTableArm(1, PWC_OFF);
	field = 5.0f;
	PlannerWaterOnSubmit(&field);
	CHECK(field == 5.0f, "water request: water off leaves the request");

	PlannerWaterTableArm(0, PWC_FLOOR);
	PlannerWaterTableClear();
}

int main()
{
	CheckTable();
	CheckRequest();
	return CheckExit("planner_water_table_units");
}
