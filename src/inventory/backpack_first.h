// backpack_first.h - The detour on Character::giveItem that tries a character's worn backpack
// before its main inventory when the character's backpack-first setting is on.
#ifndef KEO_INVENTORY_BACKPACK_FIRST_H
#define KEO_INVENTORY_BACKPACK_FIRST_H

namespace keo_inventory {

// Main thread, a startup install step: checks the reader's callees and layouts, then installs
// the detour. A refusal leaves giveItem as vanilla.
void InstallBackpackFirst(int* installed, int* total);

// Interlocked reads, for the heartbeat.
bool BackpackFirstInstalled();
long BackpackFirstCalls();      // detour entries
long BackpackFirstRouted();     // calls that tried the backpack first
long BackpackFirstPlaced();     // of those, placed in the backpack
long BackpackFirstFellBack();   // of those, the backpack refused and the original ran

} // namespace keo_inventory

#endif
