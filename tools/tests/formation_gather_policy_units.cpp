// The gathering group's rules: the skip, the re-send, the gather timeout and the group timeout.
#include <cstdio>
#include "movement/formation_gather_policy.h"

#include "check.h"

static void SkipRows()
{
	Check(FormationSkipWhileGathering(false, false), "gather skip: a gathering group's member is left to the group");
	Check(!FormationSkipWhileGathering(false, true), "gather skip: a member the merge left alone is tracked on its own");
	Check(!FormationSkipWhileGathering(true, false), "gather skip: a gathered group's member is never skipped");
	Check(!FormationSkipWhileGathering(true, true), "gather skip: an alone member of a gathered group is never skipped");
}

// A six-member group: gather radius about 29 units (841 squared), far bound 100 units, cooldown 2 s.
static void ResendRows()
{
	const float r2 = 841.0f, far2 = 10000.0f;
	Check(FormationGatherResendDue(true, 4000000.0f, r2, far2, true, 2.5, 2.0),
	      "gather resend: a deleted order far from the point is sent again past the cooldown");
	Check(!FormationGatherResendDue(true, 1600.0f, r2, far2, true, 30.0, 2.0),
	      "gather resend: a completed order standing just outside the radius, inside the far bound, is not sent again");
	Check(!FormationGatherResendDue(true, 400.0f, r2, far2, true, 30.0, 2.0),
	      "gather resend: a member inside the radius is not sent again");
	Check(!FormationGatherResendDue(true, 4000000.0f, r2, far2, false, 30.0, 2.0),
	      "gather resend: a live order is not sent again");
	Check(!FormationGatherResendDue(true, 4000000.0f, r2, far2, true, 1.0, 2.0),
	      "gather resend: inside the cooldown nothing is sent");
	Check(!FormationGatherResendDue(false, 4000000.0f, r2, far2, true, 30.0, 2.0),
	      "gather resend: before the first send nothing is re-sent");
}

static void TimeoutRows()
{
	Check(FormationGatherTimeout(300.0f, 130.0f, 15.0, 60.0) == 15.0, "gather timeout: a short gather reads the 15 s floor");
	double t = FormationGatherTimeout(2600.0f, 130.0f, 15.0, 60.0);
	Check(t > 29.9 && t < 30.1, "gather timeout: a long gather scales with its distance (2600 at 130/s, margin 1.5: 30 s)");
	Check(FormationGatherTimeout(40000.0f, 130.0f, 15.0, 60.0) == 60.0, "gather timeout: a gather past the cap reads the cap");
	Check(FormationGatherTimeout(2600.0f, 0.0f, 15.0, 60.0) == 15.0, "gather timeout: speed 0 reads the floor");
	Check(FormationGatherTimeout(0.0f, 130.0f, 15.0, 60.0) == 15.0, "gather timeout: no distance reads the floor");
}

static void GroupTimeoutRows()
{
	Check(!FormationTimeoutCancels(100.0, 120.0, 600.0, false, 0), "group timeout: a young group lives");
	Check(FormationTimeoutCancels(121.0, 120.0, 600.0, false, 3), "group timeout: an unmerged group ends at the limit");
	Check(!FormationTimeoutCancels(121.0, 120.0, 600.0, true, 2), "group timeout: a merged group still walking lives past the limit");
	Check(FormationTimeoutCancels(121.0, 120.0, 600.0, true, 0), "group timeout: a merged group nobody walks in ends at the limit");
	Check(FormationTimeoutCancels(601.0, 120.0, 600.0, true, 2), "group timeout: a merged walking group ends at the ceiling");
}

int main()
{
	SkipRows();
	ResendRows();
	TimeoutRows();
	GroupTimeoutRows();
	return CheckExit("formation_gather_policy_units");
}
