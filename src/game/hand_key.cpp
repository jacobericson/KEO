// hand_key.cpp - A game handle's identity fields.
#include "game/hand_key.h"
#include "game/klib_member_contract.h"
#include <stddef.h>

static_assert(KLIB_WIDTH_hand_type == 4 && KLIB_WIDTH_hand_container == 4 && KLIB_WIDTH_hand_containerSerial == 4
              && KLIB_WIDTH_hand_index == 4 && KLIB_WIDTH_hand_serial == 4, "hand key fields are 32-bit");

namespace game {

static unsigned ReadU32(const void* base, size_t off)
{
	return *(const unsigned*)((const char*)base + off);
}

bool HandKeyEqual(const HandKey& a, const HandKey& b)
{
	return a.type == b.type && a.container == b.container && a.containerSerial == b.containerSerial
	    && a.index == b.index && a.serial == b.serial;
}

bool HandKeyIsNull(const HandKey& k)
{
	return k.type == HAND_KEY_NULL_TYPE;
}

HandKey HandKeyFromHand(const void* hand)
{
	HandKey k = { HAND_KEY_NULL_TYPE, 0, 0, 0, 0 };
	if (!hand)
		return k;
	k.type            = ReadU32(hand, KLIB_OFF_hand_type);
	k.container       = ReadU32(hand, KLIB_OFF_hand_container);
	k.containerSerial = ReadU32(hand, KLIB_OFF_hand_containerSerial);
	k.index           = ReadU32(hand, KLIB_OFF_hand_index);
	k.serial          = ReadU32(hand, KLIB_OFF_hand_serial);
	return k;
}

HandKey HandKeyOfObject(const void* rootObjectBase)
{
	const char* hand = rootObjectBase ? (const char*)rootObjectBase + KLIB_OFF_RootObjectBase_handle : NULL;
	return HandKeyFromHand(hand);
}

} // namespace game
