#include "fixes/world/corpse_pin_policy.h"

CorpsePinDecision CorpsePinDecide(const CorpsePinMember* members, int count)
{
	CorpsePinDecision d;
	d.action = CORPSEPIN_KEEP_ORIGINAL;
	d.carriedIndex = -1;

	for (int i = 0; i < count; ++i)
	{
		if (members[i].isCharacter && !members[i].isDead)
			return d; // a live member already gave the original a real position
	}

	for (int i = 0; i < count; ++i)
	{
		if (members[i].isCharacter && members[i].isDead && members[i].isBeingCarried)
		{
			d.action = CORPSEPIN_USE_CARRIER;
			d.carriedIndex = i;
			return d;
		}
	}

	return d; // frozen case, but nothing carried -- nothing to correct
}
