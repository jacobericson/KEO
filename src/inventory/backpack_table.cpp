// backpack_table.cpp - The per-character backpack-first table: fixed slots, each guarded by its
// own sequence. Readers on any thread take no lock; the writers run on the main thread only, so
// two writes never overlap and only a reader can observe a write in progress.
#include <windows.h>
#include "inventory/backpack_table.h"
#include "inventory/backpack_policy.h"
#include "game/hand_key.h"

namespace backpack_table_detail {
// One slot. Written only between the two increments of seq; read through volatile accesses.
struct BackpackSlot
{
	volatile LONG seq;
	volatile LONG used;
	volatile LONG on;
	volatile unsigned key[5];   // type, container, containerSerial, index, serial
};
} // namespace backpack_table_detail
using namespace backpack_table_detail;

namespace keo_inventory {

static BackpackSlot s_slots[BACKPACK_TABLE_CAP];
static volatile LONG s_highWater = 0;
static volatile LONG s_full = 0;
static volatile LONG s_raced = 0;

// Any thread: at most two attempts per slot; a slot that raced both is skipped (counted). Plain
// volatile reads: on x64, MSVC's volatile loads are acquire (/volatile:ms, the x64 default), so
// the field copies stay between the two reads of seq.
int BackpackFirstGet(const game::HandKey& k)
{
	const LONG n = s_highWater;
	for (LONG i = 0; i < n; ++i)
	{
		const BackpackSlot& s = s_slots[i];
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			LONG before = s.seq;
			LONG used = s.used;
			LONG on = s.on;
			game::HandKey copy = { s.key[0], s.key[1], s.key[2], s.key[3], s.key[4] };
			LONG after = s.seq;
			if (!BackpackSlotReadable(before, after))
			{
				if (attempt == 1)
					InterlockedIncrement(&s_raced);
				continue;
			}
			if (used && game::HandKeyEqual(copy, k))
				return on ? 1 : 0;
			break;
		}
	}
	return -1;
}

// Main thread. The only store into a slot: the sequence is odd from the first increment to the
// second, and the interlocked increments are full barriers, so the three stores land between them.
static void SlotWrite(int i, LONG used, const game::HandKey& k, LONG on)
{
	BackpackSlot& s = s_slots[i];
	InterlockedIncrement(&s.seq);
	s.used = used;
	s.on = on;
	s.key[0] = k.type;
	s.key[1] = k.container;
	s.key[2] = k.containerSerial;
	s.key[3] = k.index;
	s.key[4] = k.serial;
	InterlockedIncrement(&s.seq);
}

// Main thread: the slot's own fields, read without the sequence (no other writer exists).
static bool SlotHolds(int i, const game::HandKey& k)
{
	const BackpackSlot& s = s_slots[i];
	if (!s.used)
		return false;
	game::HandKey copy = { s.key[0], s.key[1], s.key[2], s.key[3], s.key[4] };
	return game::HandKeyEqual(copy, k);
}

static game::HandKey FreeKey()
{
	game::HandKey k = { game::HAND_KEY_NULL_TYPE, 0, 0, 0, 0 };
	return k;
}

bool BackpackFirstSet(const game::HandKey& k, int on)
{
	const int n = (int)s_highWater;
	int freeSlot = -1;
	for (int i = 0; i < n; ++i)
	{
		if (SlotHolds(i, k))
		{
			SlotWrite(i, 1, k, on ? 1 : 0);
			return true;
		}
		if (freeSlot < 0 && !s_slots[i].used)
			freeSlot = i;
	}
	if (freeSlot < 0)
	{
		if (n >= BACKPACK_TABLE_CAP)
		{
			InterlockedIncrement(&s_full);
			return false;
		}
		freeSlot = n;
		// The scan bound rises before the slot is filled: a reader that reaches the slot early
		// sees it free or mid-write, never a stale entry.
		InterlockedExchange(&s_highWater, (LONG)(n + 1));
	}
	SlotWrite(freeSlot, 1, k, on ? 1 : 0);
	return true;
}

bool BackpackFirstEntry(int i, game::HandKey* k, int* on)
{
	if (i < 0 || i >= (int)s_highWater || !s_slots[i].used)
		return false;
	const BackpackSlot& s = s_slots[i];
	if (k)
	{
		game::HandKey copy = { s.key[0], s.key[1], s.key[2], s.key[3], s.key[4] };
		*k = copy;
	}
	if (on)
		*on = s.on ? 1 : 0;
	return true;
}

void BackpackFirstRekey(int i, const game::HandKey& k)
{
	const int n = (int)s_highWater;
	if (i < 0 || i >= n || !s_slots[i].used)
		return;
	for (int j = 0; j < n; ++j)
	{
		if (j != i && SlotHolds(j, k))
		{
			SlotWrite(i, 0, FreeKey(), 0);
			return;
		}
	}
	SlotWrite(i, 1, k, s_slots[i].on);
}

void BackpackFirstErase(int i)
{
	if (i < 0 || i >= (int)s_highWater)
		return;
	SlotWrite(i, 0, FreeKey(), 0);
}

// The scan bound stays where it is: a reader in flight scans freed slots and finds nothing.
void BackpackFirstClear()
{
	const int n = (int)s_highWater;
	for (int i = 0; i < n; ++i)
		SlotWrite(i, 0, FreeKey(), 0);
}

int BackpackFirstSlotCount()
{
	return (int)s_highWater;
}

int BackpackFirstEntryCount()
{
	const int n = (int)s_highWater;
	int used = 0;
	for (int i = 0; i < n; ++i)
		if (s_slots[i].used)
			++used;
	return used;
}

long BackpackFirstFullCount()
{
	return s_full;
}

long BackpackFirstRaceCount()
{
	return s_raced;
}

} // namespace keo_inventory
