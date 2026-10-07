// hand_key.h - A game handle's identity fields, copied out so they can be compared and stored
// without the hand object. Pure reads at the KenshiLib member offsets; any thread.
#ifndef KEO_HAND_KEY_H
#define KEO_HAND_KEY_H

namespace game {

// The type a null handle carries (NULL_ITEM).
const unsigned HAND_KEY_NULL_TYPE = 11;

// All five identity fields: index and serial alone are reused across containers and squads.
struct HandKey { unsigned type, container, containerSerial, index, serial; };

bool    HandKeyEqual(const HandKey& a, const HandKey& b);
bool    HandKeyIsNull(const HandKey& k);
// The five fields of the hand at `hand`; the null key for a NULL pointer.
HandKey HandKeyFromHand(const void* hand);
// A RootObjectBase's own handle; the null key for a NULL pointer.
HandKey HandKeyOfObject(const void* rootObjectBase);

} // namespace game

#endif
