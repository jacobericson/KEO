// backpack_policy.cpp - The backpack-first decisions, pure. Any thread; no lock, no allocation.
#include "inventory/backpack_policy.h"
#include <string.h>

namespace keo_inventory {

// RootObjectBase::getDataType values.
static const int DATA_TYPE_CHARACTER = 1;
static const int DATA_TYPE_CONTAINER = 46;

bool RouteToBackpackFirst(int setting, bool defaultOn, bool isAnimal, bool wearsBackpack,
                          bool itemIsTheBackpack, bool itemIsNonEmptyContainer, bool wouldAutoEquip)
{
	if (!(setting < 0 ? defaultOn : setting != 0))
		return false;
	if (isAnimal)
		return false;
	if (!wearsBackpack)
		return false;
	if (itemIsTheBackpack)
		return false;
	// Vanilla's own backpack try refuses a container holding anything.
	if (itemIsNonEmptyContainer)
		return false;
	// The main inventory's first pass would equip it: the backpack never sees it there.
	if (wouldAutoEquip)
		return false;
	return true;
}

bool BackpackSlotReadable(long seqBefore, long seqAfter)
{
	return (seqBefore & 1) == 0 && seqBefore == seqAfter;
}

int ParseUnsignedFields(const char* text, int len, unsigned* out, int max)
{
	if (!text || len < 0)
		return -1;
	int count = 0;
	int i = 0;
	while (i < len)
	{
		char c = text[i];
		if (c == ' ' || c == '\t')
		{
			++i;
			continue;
		}
		if (c < '0' || c > '9')
			return -1;
		if (count >= max)
			return -1;
		// The accumulator is checked after every digit, so a field of any length either fits in
		// 32 bits or fails here.
		unsigned __int64 v = 0;
		while (i < len && text[i] >= '0' && text[i] <= '9')
		{
			v = v * 10 + (unsigned)(text[i] - '0');
			if (v > 0xFFFFFFFFull)
				return -1;
			++i;
		}
		if (i < len && text[i] != ' ' && text[i] != '\t')
			return -1;
		if (out)
			out[count] = (unsigned)v;
		++count;
	}
	return count;
}

// Writes v in decimal at p, returns the digit count (at most 10).
static int FormatUnsigned(unsigned v, char* p)
{
	char rev[10];
	int n = 0;
	do
	{
		rev[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	for (int i = 0; i < n; ++i)
		p[i] = rev[n - 1 - i];
	return n;
}

int HandKeyFormat(const game::HandKey& k, char* out, int n)
{
	// Five 10-digit fields and four spaces at most.
	char buf[5 * 10 + 4];
	const unsigned fields[5] = { k.type, k.container, k.containerSerial, k.index, k.serial };
	int len = 0;
	for (int f = 0; f < 5; ++f)
	{
		if (f > 0)
			buf[len++] = ' ';
		len += FormatUnsigned(fields[f], buf + len);
	}
	if (!out || n < len + 1)
		return 0;
	memcpy(out, buf, (size_t)len);
	out[len] = '\0';
	return len;
}

bool HandKeyParse(const char* text, int len, game::HandKey* out)
{
	unsigned f[5];
	if (ParseUnsignedFields(text, len, f, 5) != 5)
		return false;
	game::HandKey k = { f[0], f[1], f[2], f[3], f[4] };
	if (game::HandKeyIsNull(k))
		return false;
	if (out)
		*out = k;
	return true;
}

bool FoodScoreZero(bool vanillaWants, bool backpackHasFood)
{
	return vanillaWants && backpackHasFood;
}

bool DialogUseBackpack(bool mainHas, bool wearsBackpack)
{
	return !mainHas && wearsBackpack;
}

bool BackpackBoxWanted(int callbackDataType, int ownerDataType, bool ownerIsAnimal,
                       bool ownerIsPlayer, bool ownerWearsThis)
{
	// A character's own window, or any other window, is not a backpack's.
	if (callbackDataType != DATA_TYPE_CONTAINER)
		return false;
	// A backpack on the ground, in a box or in a shop has no character owner.
	if (ownerDataType != DATA_TYPE_CHARACTER)
		return false;
	if (ownerIsAnimal)
		return false;
	// A looted NPC's backpack opens beside the loot window.
	if (!ownerIsPlayer)
		return false;
	// A backpack carried in the inventory, not on the back, takes no pickups.
	if (!ownerWearsThis)
		return false;
	return true;
}

} // namespace keo_inventory
