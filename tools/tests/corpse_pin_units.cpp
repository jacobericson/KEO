#include <cstdio>
#include "fixes/world/corpse_pin_policy.h"

#include "check.h"

int main()
{
	// An empty squad has no live member, and nothing to correct either.
	{
		CorpsePinDecision d = CorpsePinDecide(NULL, 0);
		Check(d.action == CORPSEPIN_KEEP_ORIGINAL, "an empty squad keeps the original");
	}

	// One live member: the original already used it (leader position or the
	// one-member average), so nothing is overridden even if it happens to be
	// marked as carried (a living prisoner being carried is not this bug).
	{
		CorpsePinMember m[1];
		m[0].isCharacter = true; m[0].isDead = false; m[0].isBeingCarried = true;
		CorpsePinDecision d = CorpsePinDecide(m, 1);
		Check(d.action == CORPSEPIN_KEEP_ORIGINAL, "a live member is never overridden");
	}

	// The classic case: the squad's only member is a dead, carried corpse.
	{
		CorpsePinMember m[1];
		m[0].isCharacter = true; m[0].isDead = true; m[0].isBeingCarried = true;
		CorpsePinDecision d = CorpsePinDecide(m, 1);
		Check(d.action == CORPSEPIN_USE_CARRIER, "a lone carried corpse uses the carrier");
		Check(d.carriedIndex == 0, "the carried member is index 0");
	}

	// A dead member that is not being carried (a plain unrecovered corpse):
	// the frozen position stands, nothing to redirect to.
	{
		CorpsePinMember m[1];
		m[0].isCharacter = true; m[0].isDead = true; m[0].isBeingCarried = false;
		CorpsePinDecision d = CorpsePinDecide(m, 1);
		Check(d.action == CORPSEPIN_KEEP_ORIGINAL, "an uncarried corpse keeps the frozen position");
	}

	// Multiple dead members, only the second carried: found regardless of
	// position in the list.
	{
		CorpsePinMember m[3];
		m[0].isCharacter = true; m[0].isDead = true; m[0].isBeingCarried = false;
		m[1].isCharacter = true; m[1].isDead = true; m[1].isBeingCarried = true;
		m[2].isCharacter = true; m[2].isDead = true; m[2].isBeingCarried = false;
		CorpsePinDecision d = CorpsePinDecide(m, 3);
		Check(d.action == CORPSEPIN_USE_CARRIER, "the carried member is found among several dead ones");
		Check(d.carriedIndex == 1, "the correct index is reported");
	}

	// A non-Character member (should not occur in an ActivePlatoon's things,
	// but the flag exists so the detour need not filter before building the
	// array) is never treated as a live member or a carried corpse.
	{
		CorpsePinMember m[2];
		m[0].isCharacter = false; m[0].isDead = false; m[0].isBeingCarried = false;
		m[1].isCharacter = true;  m[1].isDead = true;  m[1].isBeingCarried = true;
		CorpsePinDecision d = CorpsePinDecide(m, 2);
		Check(d.action == CORPSEPIN_USE_CARRIER, "a non-Character member does not block the real corpse");
		Check(d.carriedIndex == 1, "the carried member's own index is reported");
	}

	// Any live Character member anywhere in the list wins, even alongside a
	// carried corpse -- the original already averaged the live ones in.
	{
		CorpsePinMember m[2];
		m[0].isCharacter = true; m[0].isDead = true;  m[0].isBeingCarried = true;
		m[1].isCharacter = true; m[1].isDead = false; m[1].isBeingCarried = false;
		CorpsePinDecision d = CorpsePinDecide(m, 2);
		Check(d.action == CORPSEPIN_KEEP_ORIGINAL, "a live squadmate takes precedence over a carried corpse");
	}

	return CheckExit("corpse_pin_units");
}
