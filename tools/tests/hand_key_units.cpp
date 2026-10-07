// The hand key: equality over all five fields, the null type, and the reads at the hand's member
// offsets and at an object's handle. The offsets here are literals, so a contract drift fails.
#include <cstdio>
#include <cstring>
#include "game/hand_key.h"

#include "check.h"

using game::HandKey;

static HandKey Key(unsigned type, unsigned container, unsigned containerSerial, unsigned index, unsigned serial)
{
	HandKey k = { type, container, containerSerial, index, serial };
	return k;
}

static void PutU32(unsigned char* base, size_t off, unsigned v)
{
	std::memcpy(base + off, &v, sizeof v);
}

static void WriteHand(unsigned char* hand, const HandKey& k)
{
	PutU32(hand, 0x08, k.type);
	PutU32(hand, 0x0C, k.container);
	PutU32(hand, 0x10, k.containerSerial);
	PutU32(hand, 0x14, k.index);
	PutU32(hand, 0x18, k.serial);
}

static void CheckEquality()
{
	const HandKey ref = Key(2, 7, 31, 405, 9001);
	CHECK(game::HandKeyEqual(ref, Key(2, 7, 31, 405, 9001)), "key: equal keys match");
	const HandKey other[5] =
	{
		Key(3, 7, 31, 405, 9001), Key(2, 8, 31, 405, 9001), Key(2, 7, 32, 405, 9001),
		Key(2, 7, 31, 406, 9001), Key(2, 7, 31, 405, 9002)
	};
	bool apart = true;
	for (int i = 0; i < 5; ++i)
		apart = apart && !game::HandKeyEqual(ref, other[i]) && !game::HandKeyEqual(other[i], ref);
	CHECK(apart, "key: each field alone tells two keys apart");
	CHECK(game::HandKeyIsNull(Key(11, 0, 0, 0, 0)) && !game::HandKeyIsNull(ref), "key: the NULL_ITEM type is null");
}

static void CheckReads()
{
	const HandKey ref = Key(2, 7, 31, 405, 9001);
	unsigned char hand[0x20];
	std::memset(hand, 0xCC, sizeof hand);
	WriteHand(hand, ref);
	CHECK(game::HandKeyEqual(game::HandKeyFromHand(hand), ref), "key: read from a hand at its member offsets");

	unsigned char object[0x80];
	std::memset(object, 0xCC, sizeof object);
	WriteHand(object + 0x58, ref);
	CHECK(game::HandKeyEqual(game::HandKeyOfObject(object), ref), "key: read from an object's handle at +0x58");

	CHECK(game::HandKeyIsNull(game::HandKeyFromHand(NULL)) && game::HandKeyIsNull(game::HandKeyOfObject(NULL)),
	      "key: a NULL pointer reads as the null key");
}

int main()
{
	CheckEquality();
	CheckReads();
	return CheckExit("hand_key_units");
}
