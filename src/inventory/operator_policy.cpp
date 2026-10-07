// operator_policy.cpp - The operator hold's guards, in the order the detour gathers them, the weight
// tiers' thresholds, and the jobs heartbeat's names and haul buckets. Pure; any thread.
#include "inventory/operator_policy.h"

namespace keo_inventory {

OperatorReason OperatorReasonOf(const OperatorFacts& f)
{
	if (!f.vanillaTrue)  return OR_VANILLA_FALSE;
	if (!f.isPlayer)     return OR_NOT_PLAYER;
	if (!f.haveMachine)  return OR_NO_MACHINE;
	if (!f.ownsMachine)  return OR_NOT_OWNED;
	if (!f.isResource)   return OR_NOT_RESOURCE;
	if (!f.powered)      return OR_UNPOWERED;
	if (!f.inputsValid)  return OR_INPUTS_INVALID;
	if (f.hungry)        return OR_HUNGRY;
	if (f.tooHeavy)      return OR_TOO_HEAVY;
	if (!f.haveProduct)  return OR_NO_PRODUCT;
	if (!f.hasRoom)      return OR_NO_ROOM;
	return OR_HOLD;
}

bool OperatorAnswer(bool vanilla, OperatorReason r)
{
	return r == OR_HOLD ? false : vanilla;
}

bool HoldDelivery(bool vanillaTrue, bool isPlayer, bool ownsMachine, bool isResource, bool hasRoom,
                  bool powered, bool inputsValid, bool hungry, bool tooHeavy)
{
	return vanillaTrue && isPlayer && ownsMachine && isResource && hasRoom && powered && inputsValid
	    && !hungry && !tooHeavy;
}

float OperatorHoldThreshold(int tier)
{
	switch (tier)
	{
	case OPERATOR_HOLD_WEIGHTLESS:  return 0.95f;
	case OPERATOR_HOLD_LIGHTWEIGHT: return 0.75f;
	case OPERATOR_HOLD_MODERATE:    return 0.5f;
	case OPERATOR_HOLD_HEAVY:       return 0.1f;
	default:                        return 0.0f;
	}
}

bool OperatorTooHeavy(float encumbranceMult, int tier)
{
	if (tier < OPERATOR_HOLD_WEIGHTLESS || tier > OPERATOR_HOLD_HEAVY)
		return false;
	return encumbranceMult < OperatorHoldThreshold(tier);
}

const char* OperatorReasonName(OperatorReason r)
{
	switch (r)
	{
	case OR_HOLD:           return "hold";
	case OR_VANILLA_FALSE:  return "vanilla";
	case OR_NOT_PLAYER:     return "notPlayer";
	case OR_NO_MACHINE:     return "noMachine";
	case OR_NOT_OWNED:      return "notOwned";
	case OR_NOT_RESOURCE:   return "notResource";
	case OR_UNPOWERED:      return "unpowered";
	case OR_INPUTS_INVALID: return "inputs";
	case OR_HUNGRY:         return "hungry";
	case OR_TOO_HEAVY:      return "heavy";
	case OR_NO_PRODUCT:     return "noProduct";
	case OR_NO_ROOM:        return "noRoom";
	default:                return "?";
	}
}

int HaulBucket(long long amount)
{
	if (amount < 0)
		return 0;
	if (amount > HAUL_BUCKETS - 2)
		return HAUL_BUCKETS - 1;
	return (int)amount;
}

} // namespace keo_inventory
