// backpack_sidecar.h - The backpack-first table's persistence (a sidecar file written into each
// save's temp folder and read back at load), its main-thread re-key, and the module heartbeat.
#ifndef KEO_INVENTORY_BACKPACK_SIDECAR_H
#define KEO_INVENTORY_BACKPACK_SIDECAR_H

#include "game/hand_key.h"

namespace keo_inventory {

// Main thread, a startup install step after InstallBackpackFirst: checks the three callees'
// heads, then installs the three SaveFileSystem hooks. Arms only when all three are in; an armed
// failure leaves each installed hook a pass-through.
void InstallBackpackSidecar(int* installed, int* total);

// Main thread. Follows each entry's handle through HandleManager's redirects and moves a moved
// entry to its character's live key. dropUnresolved erases an entry whose character is not
// found (save time only). Returns the entries moved; every move, from any caller, counts in
// rekeyed=.
int  BackpackRekeyNow(bool dropUnresolved);

// Main thread. *live is k's character's live hand key, through the same redirect lookup the
// re-key makes. False, with *live = k, when the lookup is unbound or the character is not found.
bool BackpackResolveKey(const game::HandKey& k, game::HandKey* live);

// Main thread, every frame from the camera tick: the re-key once a second outside a save load,
// and the "Backpack:" heartbeat once a minute whether or not anything is armed.
void BackpackRekeyTick(double now, bool saveLoading);

} // namespace keo_inventory

#endif
