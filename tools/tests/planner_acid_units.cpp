// The acidic-water cells and the immunity read: the fill through a callback, the unknown retry and its
// window, the world start, the binding and its refusal.
#include <cstdio>
#include <cstring>
#include "planner/planner_acid.h"
#include "planner/plan_policy.h"

#include "check.h"

using namespace planner;

static int s_calls      = 0;
static int s_knownAfter = 0;    // cell (30, 30) answers unknown until this many lookups were made
static int s_lastAsked  = -1;   // the weather kind the fake isImmune was last asked about

// Cell (10, 20) has acidic water (0.5); every other known cell is dry.
static int FakeLookup(void* ctx, float x, float z, float* acidWater)
{
	(void)ctx;
	int call = s_calls++;
	int cx = -1, cy = -1;
	PlanCellOf(x, z, &cx, &cy);
	if (cx == 30 && cy == 30 && call < s_knownAfter)
		return 0;
	*acidWater = (cx == 10 && cy == 20) ? 0.5f : 0.0f;
	return 1;
}

static bool FakeIsImmune(void* race, int weatherAffecting)
{
	s_lastAsked = weatherAffecting;
	return race == (void*)0x2000;
}

static void CheckFill()
{
	PlanAcidStats st;
	PlannerAcidNewWorld();
	s_calls = 0;
	s_knownAfter = PLAN_ACID_CELLS;
	int first = PlannerAcidFillStep(100.0, FakeLookup, NULL);
	PlannerAcidStatsGet(&st);
	CHECK(first == PLAN_ACID_CELLS && st.acidCells == 1 && st.unknown == 1,
	      "acid fill: the first step looks every cell up and keeps an unknown one unknown");
	CHECK(PlannerAcidCellIs(20 * 64 + 10) == 1 && PlannerAcidCellIs(20 * 64 + 11) == 0
	      && PlannerAcidCellIs(30 * 64 + 30) == 0,
	      "acid fill: an acid biome tags its cell and an unknown cell reads dry");
	CHECK(PlannerAcidCellIs(PLAN_ACID_CELLS) == 0 && PlannerAcidCellIs(-1) == 0,
	      "acid cells: an interior or an index out of range reads dry");
	CHECK(PlannerAcidFillStep(100.5, FakeLookup, NULL) == 0, "acid fill: no retry within the interval");
	CHECK(PlannerAcidFillStep(101.0, FakeLookup, NULL) == 1, "acid fill: an unknown cell is retried after the interval");
	PlannerAcidStatsGet(&st);
	CHECK(st.unknown == 0 && PlannerAcidFillStep(105.0, FakeLookup, NULL) == 0, "acid fill: nothing unknown, no pass");

	PlannerAcidNewWorld();
	s_calls = 0;
	s_knownAfter = 1 << 30;
	CHECK(PlannerAcidFillStep(200.0, FakeLookup, NULL) == PLAN_ACID_CELLS,
	      "acid fill: after a world start the next step looks every cell up again");
	CHECK(PlannerAcidCellIs(20 * 64 + 10) == 1, "acid fill: the refill tags the acid cell again");
	CHECK(PlannerAcidFillStep(259.0, FakeLookup, NULL) == 1 && PlannerAcidFillStep(261.0, FakeLookup, NULL) == 0,
	      "acid fill: the retries stop 60 s after the first pass");
	PlannerAcidStatsGet(&st);
	CHECK(st.unknown == 1, "acid fill: a cell never known stays counted unknown");

	PlannerAcidNewWorld();
	PlannerAcidStatsGet(&st);
	CHECK(st.unknown == PLAN_ACID_CELLS && st.acidCells == 0 && PlannerAcidCellIs(20 * 64 + 10) == 0,
	      "acid fill: a world start forgets every cell");
	CHECK(PlannerAcidFillStep(300.0, NULL, NULL) == 0, "acid fill: no lookup, no pass");
}

static void CheckBind()
{
	static const unsigned char good[16] =
		{ 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D, 0x44, 0x24, 0x38, 0x48, 0x8D, 0x54 };
	unsigned char bad[16];
	memcpy(bad, good, sizeof(bad));
	bad[15] ^= 0xFF;
	CHECK(!PlannerAcidBind(bad, FakeIsImmune) && PlannerAcidImmune((void*)0x1000) == 1
	      && PlannerAcidImmune((void*)0x2000) == 1,
	      "acid bind: a refused binding reads every race immune");
	CHECK(!PlannerAcidBind(NULL, FakeIsImmune), "acid bind: no code refuses");
	CHECK(PlannerAcidBind(good, FakeIsImmune), "acid bind: isImmune's prologue binds");
	CHECK(PlannerAcidImmune((void*)0x1000) == 0 && s_lastAsked == PLAN_WA_ACID,
	      "acid bind: a race the engine does not count immune reads 0, asked about acid");
	CHECK(PlannerAcidImmune((void*)0x2000) == 1, "acid bind: an immune race reads 1");
	CHECK(PlannerAcidImmune(NULL) == 1, "acid bind: no race reads immune");
	PlannerAcidBind(NULL, NULL);
}

int main()
{
	CheckFill();
	CheckBind();
	return CheckExit("planner_acid_units");
}
