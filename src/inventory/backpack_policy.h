// backpack_policy.h - The backpack-first decisions, pure: whether a pickup tries the worn
// backpack first, the table's slot read rule, and the text form of a hand key. No Windows,
// KenshiLib or game header beyond the hand key's POD; any thread.
#ifndef KEO_INVENTORY_BACKPACK_POLICY_H
#define KEO_INVENTORY_BACKPACK_POLICY_H

#include "game/hand_key.h"

namespace keo_inventory {

// setting: the character's table entry, 1 or 0, or -1 for none (then defaultOn decides).
// True only when the setting resolves on, the character is not an animal, wears a backpack,
// the item is not that backpack, the item is not a container holding anything, and the item
// would not go into one of the main inventory's empty equipment slots (vanilla equips it there).
bool RouteToBackpackFirst(int setting, bool defaultOn, bool isAnimal, bool wearsBackpack,
                          bool itemIsTheBackpack, bool itemIsNonEmptyContainer, bool wouldAutoEquip);

// A table slot copied between two reads of its sequence is consistent when the sequence was
// even (no write in progress) and did not move.
bool BackpackSlotReadable(long seqBefore, long seqAfter);

// Up to max unsigned decimal fields separated by spaces or tabs, each at most 0xFFFFFFFF.
// Returns the field count, or -1 for any other character, an overlong field, or more than max
// fields.
int  ParseUnsignedFields(const char* text, int len, unsigned* out, int max);

// "type container containerSerial index serial". Returns the characters written (no
// terminator counted), or 0 when n is too small.
int  HandKeyFormat(const game::HandKey& k, char* out, int n);
// Exactly five fields and not the null handle.
bool HandKeyParse(const char* text, int len, game::HandKey* out);

} // namespace keo_inventory

#endif
