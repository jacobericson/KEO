#ifndef KEO_FIXES_THROWOUT_HOLD_H
#define KEO_FIXES_THROWOUT_HOLD_H

#include "game/hand_key.h"

// Bodies the throw-out finder must skip: a 256-entry ring of {handle, the handle's 32 bytes,
// expiry in in-game hours}. Lock-free and allocation-free on every thread. Each entry has a
// sequence word, odd while written: a writer claims an even entry by compare-exchange, writes
// the key, the bytes and the expiry, then publishes the next even value; a reader copies an entry
// between two equal even reads of its sequence, at most twice, and otherwise treats it as not
// held. A writer that draws an entry being written skips it and counts the add as lost.

namespace fixes {

const int THROWOUT_HOLD_SLOTS = 256;
const int THROWOUT_HOLD_CLEAR_TRIES = 64;   // compare-exchange attempts per entry in a clear

// A game hand's 32 bytes (its vftable and fields), 8-aligned: it is passed to the game as `this`.
struct __declspec(align(8)) ThrowoutHand { unsigned char bytes[32]; };

// Any thread. False when the drawn entry was being written (counted by the caller as lost).
bool ThrowoutHoldAdd(const game::HandKey& key, const ThrowoutHand& hand, double expiryHours);
// Any thread. True while a live entry with this key is HELD at nowHours under capHours (a stale
// entry is not held and is left for the main thread to release; ThrowoutHoldDecide with
// unconscious = true: the finder asks only about unconscious candidates).
bool ThrowoutHoldIsHeld(const game::HandKey& key, double nowHours, double capHours);
// Main thread. Entry i's contents, when it is live and was read whole.
bool ThrowoutHoldRead(int i, game::HandKey* key, ThrowoutHand* hand, double* expiryHours);
// Main thread. Ends entry i only if it still holds this key with this expiry (the values a
// ThrowoutHoldRead returned); false when the entry changed or was being written. A body thrown
// out again in between has a new expiry, so its fresh hold stays.
bool ThrowoutHoldRelease(int i, const game::HandKey& key, double expiryHours);
// Main thread, on a save load's rising edge: every entry ends. An entry another writer holds is
// retried up to THROWOUT_HOLD_CLEAR_TRIES times; returns the number still not cleared.
long ThrowoutHoldClear();
long ThrowoutHoldLive();   // live entries, a main-thread walk

} // namespace fixes

#endif // KEO_FIXES_THROWOUT_HOLD_H
