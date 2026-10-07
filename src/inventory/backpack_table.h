// backpack_table.h - The per-character backpack-first table: up to BACKPACK_TABLE_CAP entries
// {hand key, on}. Read lock-free from any thread; written only on the main thread. Each slot
// carries a sequence: odd while a write is in progress, bumped by an interlocked increment before
// and after the write, so a reader that copies a slot between two even, equal reads holds one
// write's key and value together.
#ifndef KEO_INVENTORY_BACKPACK_TABLE_H
#define KEO_INVENTORY_BACKPACK_TABLE_H

#include "game/hand_key.h"

namespace keo_inventory {

const int BACKPACK_TABLE_CAP = 512;

// Any thread. 1 or 0 for the character's entry; -1 when it has none, or when both reads of its
// slot raced a write (counted; the caller then uses the default, as for no entry).
int  BackpackFirstGet(const game::HandKey& k);

// Main thread only. Updates k's entry, else takes the lowest free slot. False when the table
// is full (counted).
bool BackpackFirstSet(const game::HandKey& k, int on);
// Main thread only. Slot i's key and value while it is in use; false for a free slot or an
// index out of range.
bool BackpackFirstEntry(int i, game::HandKey* k, int* on);
// Main thread only. Moves slot i to key k. When another slot already holds k, the slot already
// holding k wins and slot i is erased.
void BackpackFirstRekey(int i, const game::HandKey& k);
void BackpackFirstErase(int i);   // main thread only
void BackpackFirstClear();        // main thread only

int  BackpackFirstSlotCount();    // one past the highest slot ever used: the scan bound
int  BackpackFirstEntryCount();   // slots in use
long BackpackFirstFullCount();    // Set calls refused for a full table
long BackpackFirstRaceCount();    // Get reads that raced a write twice

} // namespace keo_inventory

#endif
