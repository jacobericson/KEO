// The gathering group's skip rule: which members the island tracker and K7 leave to the group.
#include <cstdio>
#include "movement/formation_gather_policy.h"

#include "check.h"

int main()
{
	Check(FormationSkipWhileGathering(false, false), "gather skip: a gathering group's member is left to the group");
	Check(!FormationSkipWhileGathering(false, true), "gather skip: a member the merge left alone is tracked on its own");
	Check(!FormationSkipWhileGathering(true, false), "gather skip: a gathered group's member is never skipped");
	Check(!FormationSkipWhileGathering(true, true), "gather skip: an alone member of a gathered group is never skipped");
	return CheckExit("formation_gather_policy_units");
}
