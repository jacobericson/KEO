// destroy_list_defer.h - Destroy-list probe and deferral interface.
// Main thread for setup, probe, stats, flush and drop; any thread for inserts.
// Queue operations take leaf deferCS; original inserts run with it released.

#ifndef KEO_DESTROY_LIST_DEFER_H
#define KEO_DESTROY_LIST_DEFER_H

#include "base/core.h"

// =========================================================================
// destroyListOE invariant probe
// =========================================================================
//
// The game's per-frame destroy-list drain (sub_14079CD00+0x1C1) dereferences
// GameWorld::destroyListOE's sentinel head without a null check whenever the
// element count is non-zero over an empty node list. That state is produced
// by a lost update on insert, i.e. two unsynchronised inserts.
//
// Two read-only instruments, both cheap enough for PROD:
//   * DestroyListProbeTick() — once per frame on the main thread, comparing
//     element count against the sentinel head for two hash containers in the
//     GameWorld singleton. It samples just AFTER the frame's own drain: the
//     drain is called at GameWorld__mainLoop_GPUSensitiveStuff+0x1B0 and
//     updateCameraZone at +0x385, in that order. A normal drain empties the
//     container, so a hit means a drain exited with a stale count over an empty
//     list — and the NEXT frame's drain is the one that faults on it.
//   * hook_destroyListInsert — pass-through on the sole inserter, recording
//     which thread calls it. A non-main thread there proves the race outright,
//     and proves the defect is vanilla rather than mod-authored.
//
// Neither writes game memory. The layering matches SetHavokTlsParams: game.cpp
// hands core the resolved address, so core keeps no game knowledge.

// Called by InitGameBindings with gameBase + RVA_GLOBAL_GAMEWORLD.
void SetDestroyListBase(uintptr_t gameWorld);
void DestroyListDeferInit();

// Main thread, top of hook_updateCameraZone. One frame of warning ahead of the
// crash; see above for why it is after the drain and not before.
void DestroyListProbeTick();

// `, dlIns=<main>/<other>` for the transition stats line, plus the recorded
// off-main-thread return addresses the first time any exist.
std::string DestroyListStatsSuffix();

// -------------------------------------------------------------------------
// Mitigation: defer off-main-thread inserts to the main thread
// -------------------------------------------------------------------------
//
// The game inserts into destroyListOE from `PhysicalEntity::~PhysicalEntity`
// (RVA 0x4CAFF0; the IDB mislabels it as a basic_filebuf dtor) while sectors
// unload on the contentStream thread — observed as `dlIns=2721/129
// dlFrom=0x4cb02a`. The main thread drains the same container
// every frame with no synchronisation at all, so an insert that overlaps
// another insert or the drain loses a link while both count, leaving the
// element count above the node list. The next frame's drain enters on the
// count and dereferences a NULL sentinel head.
//
// The mitigation takes the off-main insert out of the race: the hook queues the
// object instead of touching the container, and the main thread performs the
// real insert on the next frame. Every write to destroyListOE is then on one
// thread. The object is not destroyed until a drain reaches it, and deferring
// by one frame only postpones that; nothing else in the binary frees it.
//
// This mitigates a vanilla defect that the mod's zone churn exposes, not
// mod-authored corruption — no mod write lands in the GameWorld object.

// Set at the install site, after LoadConfig (INI `destroyListDefer`).
void SetDestroyListDefer(bool enabled);

// Main thread, in hook_updateCameraZone: empties the deferred queue and
// performs each insert through the original, at most 8 batches of 32 per frame
// (leftovers next frame). Never holds the queue lock across the original call.
void DestroyListFlushDeferred();

// Drops the queue without inserting anything, counted as `dlDropClear`. Called
// when the world is cleared: sub_1407A82C0 (reached from SaveManager__execute
// and the save-load / new-game paths) drains destroyListOE with a2=1 and tears
// the scene down, so every queued pointer is about to be freed and replaying it
// would insert a dangling pointer into a container the clear has just emptied.
// The mod's save-load edge (`PreloadCheckSaveLoad` -> `ClearPreloadStateForLoad`
// in preload_saveload.cpp) is the signal.
void DestroyListDropDeferred();

// Hook on the sole destroyListOE inserter, sub_140799BE0 (RVA 0x799BE0,
// `__fastcall(GameWorld*, Ogre::MovableObject*)`). On the main thread, and
// whenever deferral is off, it is a pass-through with counters only — no CRT,
// no logging, callable on any thread, except that a main-thread insert first
// removes any queued entry for the same object (`dlDedup`), because the set's
// own "already present" no-op is the thing a queue in front of it would lose.
// Off the main thread with deferral on it queues the object under `deferCS`
// (replacing any entry it already holds for that object) and returns without
// calling the original.
//
// What the original does, and therefore what deferral defers: it calls
// `getMovableType` (vtable+32), and when the type is "Entity" it also calls
// sub_140449240 via sub_140023317 — which unlinks the object from the loader
// singleton's queued (+320/+328) and preloaded (+288/+296) lists and deletes
// the list node's payload — then `setVisible(false)` and the set insert. The
// de-registration is on the Entity branch, i.e. the branch a PhysicalEntity's
// Ogre object normally takes, so it is normally deferred with the insert. That
// is a one-frame delay, not a lost call: nothing frees the object in between,
// and the flush performs the whole original. `dlDeferEnt=<entity>/<other>`
// counts which branch each deferred insert would have taken.
//
// Discarding the original's return value is safe: it tail-returns the address
// of its own 32-byte stack temporary (the `pair<iterator,bool>` that the set
// insert at sub_140018930 writes), which is dead the moment it returns, and
// none of the 22 call sites reads it.
typedef __int64 (__fastcall *destroyListInsert_t)(void* gameWorld, void* movable);
extern destroyListInsert_t orig_destroyListInsert;
__int64 __fastcall hook_destroyListInsert(void* gameWorld, void* movable);


#endif // KEO_DESTROY_LIST_DEFER_H
