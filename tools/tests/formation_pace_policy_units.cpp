// The gather pacing's decisions: the factor and its floor, the farthest distance over the paced
// members, the arrival spread, and the factor table's publish, read, clear, cap and torn read.
#include <cstdio>
#include "movement/formation_pace_policy.h"

#include "check.h"

static PaceTable s_table;   // zero: an even sequence, no entry

static void CheckFactor()
{
	const float r2 = 40.0f * 40.0f;
	const float far2 = 400.0f * 400.0f;
	CHECK(FormationPaceFactor(far2, far2, r2) == 1.0f, "pace: the farthest member runs at full speed");
	CHECK(FormationPaceFactor(200.0f * 200.0f, far2, r2) == 0.5f,
	      "pace: a member at half the farthest distance runs at half speed");
	CHECK(FormationPaceFactor(100.0f * 100.0f, far2, r2) == PACE_MIN_FACTOR,
	      "pace: a member at a quarter of the farthest distance runs at half speed");
	float f = FormationPaceFactor(300.0f * 300.0f, far2, r2);
	CHECK(f > 0.7499f && f < 0.7501f, "pace: a member at three quarters runs at three quarters");
	CHECK(FormationPaceFactor(30.0f * 30.0f, far2, r2) == 1.0f, "pace: a member inside the gather radius runs at full speed");
	CHECK(FormationPaceFactor(50.0f * 50.0f, 0.0f, r2) == 1.0f, "pace: no farthest distance reads full speed");
	CHECK(FormationPaceFactor(50.0f * 50.0f, -1.0f, r2) == 1.0f, "pace: no paced member reads full speed");
}

static void CheckWalksGather()
{
	CHECK(FormationPaceWalksGather(129.0f, 200.0f, 100.0f, 200.0f), "pace: a member walking to its gather point walks the gather");
	CHECK(!FormationPaceWalksGather(131.0f, 200.0f, 100.0f, 200.0f), "pace: a member walking another order is not the gather's");
	CHECK(FormationPaceWalksGather(100.0f, 230.0f, 100.0f, 200.0f), "pace: a member at the bound walks the gather");
}

static void CheckFarthestAndSpread()
{
	const float d[4] = { -1.0f, 900.0f, 2500.0f, -1.0f };
	const float none[2] = { -1.0f, -1.0f };
	CHECK(FormationPaceMaxDistSq(d, 4) == 2500.0f, "pace: the farthest distance is over the paced members");
	CHECK(FormationPaceMaxDistSq(none, 2) == -1.0f, "pace: no paced member gives no farthest distance");
	const double arrive[4] = { 0.0, 10.0, 12.5, 11.0 };
	CHECK(FormationGatherSpread(arrive, 4) == 2.5, "spread: a member that never arrived is not counted");
	const double one[2] = { 0.0, 10.0 };
	CHECK(FormationGatherSpread(one, 2) == 0.0, "spread: one arrival reads 0");
}

static void CheckTable()
{
	PaceEntry e[2] = { { 0x1000, 0.5f }, { 0x2000, 0.75f } };
	PaceTablePublish(&s_table, e, 2);
	CHECK(PaceTableRead(&s_table, 0x1000) == 0.5f && PaceTableRead(&s_table, 0x2000) == 0.75f,
	      "pace table: a published member reads its factor");
	CHECK(PaceTableRead(&s_table, 0x3000) == 1.0f, "pace table: an absent member reads 1");
	PaceEntry z = { 0, 0.5f };
	PaceTablePublish(&s_table, &z, 1);
	CHECK(PaceTableRead(&s_table, 0) == 1.0f, "pace table: no character reads 1");
	PaceTablePublish(&s_table, e, 2);
	s_table.seq += 1;   // a publish in progress
	CHECK(PaceTableRead(&s_table, 0x1000) == 1.0f, "pace table: a read inside a publish reads 1");
	s_table.seq += 1;
	CHECK(PaceTableRead(&s_table, 0x1000) == 0.5f && (s_table.seq & 1) == 0,
	      "pace table: a whole publish reads again once the sequence is even");
	PaceTablePublish(&s_table, NULL, 0);
	CHECK(PaceTableRead(&s_table, 0x1000) == 1.0f && s_table.count == 0, "pace table: a clear leaves every member at 1");
	PaceEntry many[PACE_TABLE_MAX + 1];
	for (int i = 0; i <= PACE_TABLE_MAX; ++i)
	{
		many[i].character = 0x100 + (size_t)i;
		many[i].factor = 0.6f;
	}
	PaceTablePublish(&s_table, many, PACE_TABLE_MAX + 1);
	CHECK(s_table.count == PACE_TABLE_MAX && PaceTableRead(&s_table, 0x100 + (size_t)PACE_TABLE_MAX) == 1.0f,
	      "pace table: a publish past the cap keeps the cap and paces nobody past it");
}

int main()
{
	CheckFactor();
	CheckWalksGather();
	CheckFarthestAndSpread();
	CheckTable();
	return CheckExit("formation_pace_policy_units");
}
