// formation_gather_policy.cpp - The gathering group's skip rule. Pure; the caller's thread.
#include "movement/formation_gather_policy.h"

bool FormationSkipWhileGathering(bool groupGathered, bool memberAlone)
{
	return !groupGathered && !memberAlone;
}
