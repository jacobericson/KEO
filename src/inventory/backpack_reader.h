// backpack_reader.h - The character's worn backpack and the inventory facts backpack-first
// needs, read straight from the game's layouts. Any thread after BackpackReaderInit: read-only,
// no lock, no allocation, no logging (the game's own helper for the backpack allocates).
#ifndef KEO_INVENTORY_BACKPACK_READER_H
#define KEO_INVENTORY_BACKPACK_READER_H

namespace keo_inventory {

// Main thread, at install: binds getSectionOfType and isLimitedSlotCompatible and checks each
// callee's head and the four layout tripwires against the bytes this build carries. False, with
// *why naming the first mismatch, when any differs.
bool  BackpackReaderInit(const char** why);

void* CharacterInventory(void* character);         // Character's main Inventory*, or NULL
void* ItemInventory(void* item);                   // the item's own Inventory* (a container), or NULL
bool  InventoryIsEmpty(void* inventory);           // no items
bool  CharacterIsAnimal(void* character);
// The first item of the main inventory's ATTACH_BACKPACK section when it is a container, else NULL.
void* WornBackpack(void* character);
// That backpack's Inventory*, or NULL (no worn backpack, or one without an inventory).
void* WornBackpackInventory(void* character);
// True when one of the inventory's equipment sections is empty and accepts the item: vanilla's
// tryAddItem would equip it there. True also when the section list cannot be read, so every
// doubt keeps vanilla's order.
bool  InventoryWouldAutoEquip(void* inventory, void* item);

} // namespace keo_inventory

#endif
