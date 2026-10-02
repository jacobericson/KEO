#ifndef KEO_FIXES_CORPSE_PIN_POLICY_H
#define KEO_FIXES_CORPSE_PIN_POLICY_H

// The decision behind the corpse-pin detour on ActivePlatoon::calculateCurrentPos
// (corpse_pin.h): is this squad the frozen-position case, and if so, which
// member is a carried corpse whose carrier's position should be used instead.
// Pure arithmetic over a plain description of the squad's members, so it is
// host-testable and cannot drift from what the detour actually checks.

struct CorpsePinMember
{
	bool isCharacter;   // getDataType() == CHARACTER (1); a non-Character member never applies
	bool isDead;
	bool isBeingCarried;
};

enum CorpsePinAction
{
	CORPSEPIN_KEEP_ORIGINAL, // the original's result already reflects a live member
	CORPSEPIN_USE_CARRIER    // use the carrier's position for the member at carriedIndex
};

struct CorpsePinDecision
{
	CorpsePinAction action;
	int             carriedIndex; // valid only when action == CORPSEPIN_USE_CARRIER
};

// members/count describe one squad's things list, in order. The original
// function returns a live position (the leader's, or an average) whenever any
// member is a living Character; only when none are does it fall back to the
// frozen workingPos, which is the case this decides for. Among carried corpses
// the first one found is used -- calculateCurrentPos itself has no ordering
// preference among dead members, so any deterministic pick is equally valid.
CorpsePinDecision CorpsePinDecide(const CorpsePinMember* members, int count);

#endif // KEO_FIXES_CORPSE_PIN_POLICY_H
