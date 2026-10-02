// save_load_bindings.h - Save-load reset bindings and zone handle registry layout.
// Included through game.h.

#ifndef KEO_SAVE_LOAD_BINDINGS_H
#define KEO_SAVE_LOAD_BINDINGS_H

#include "game/klib_members.h"
#include "base/core.h"

// ---- Save-load reset ----
//
// The game's "Reset game" step, sub_14036CA40 (0x36CA40), runs at the start of
// every save load (SaveManager::loadGame 0x373DC0, via thunk 0x41C31 at
// 0x373E82) and from SaveManager::execute / importGame. At 0x36CE38 it calls
// sub_14036C1E0 below; then, at 0x36CF2B, the world clear sub_1407A82C0; then,
// at 0x36CF60, the ZoneMap handle registry's vt+24 (sub_1404FDC30, a memset of
// every slot). All three run on the main thread (SaveManager::execute's callers
// are GameWorld__mainLoop_GPUSensitiveStuff +0x241 and MainListener's vtable
// slot 3); the hook still checks at runtime. So while sub_14036C1E0 runs, the
// registry and the world's object set are still intact. Not everything is:
// the reset has already run GameWorld__populateMapArea_nonPermanent (0x36CB6A),
// the sub_140786790 purge (0x36CB96), FactionManager__clearAndDestroy
// (0x36CBBA) and the biome/weather reset (0x36CBD7) -- the same
// state the game's own Set A/B unloads run in.
//
// sub_14036C1E0 (0x36C1E0, 0x2C8 bytes), `void __fastcall(ZoneManager*)`, its
// only caller being the reset through thunk 0x513FC (call at 0x36CE38): it copies every zone in
// Set B (ZM+1474824, count +1474856) and Set A (ZM+1474760, count +1474792)
// into an array, calls sub_140A09BB0(zm, zone, 0) on each (0x36C400-0x36C409),
// empties Set A, and zeroes ZM+1475024 (OFF_ZM_CURRENT_ZONE). Zones in neither
// set, i.e. the mod's, are left loaded; the registry memset that follows then
// leaves them with a live content and a dead registration. Prologue
// `mov r11,rsp; mov [r11+8],rcx` is position-independent.
const size_t RVA_RESET_UNLOAD_ZONES = 0x36C1E0;
typedef void (__fastcall *resetUnloadZones_t)(void* zoneMgr);
extern resetUnloadZones_t orig_resetUnloadZones;

// sub_140A09BB0 (0xA09BB0, 0x4B bytes), called through its thunk 0x365ED exactly
// as the original does: `__int64 __fastcall(ZoneManager*, ZoneMap*, u8 param)`.
// It calls ZoneManager::unloadSingleZone(zone, param) (0xA09620 via thunk
// 0x440DF: a no-op unless ZoneMap+0 is set; otherwise SectionManager
// onZoneUnload, Town::notifyUnloading, ZoneContent::prepareUnload (which puts
// the registry sentinel in the zone's slot), content delete, +0 = NULL,
// +176 = +177 = 0), erases the zone from Set B (sub_1409F0100: a no-op for a
// zone not in it), then sub_1409D59C0(qword_142134BE8, zone) (a per-zone map
// keyed x + 64y, releasing its +16 resource). The reset passes param = 0.
// param is ZoneContent::prepareUnload's save-zone-state flag (its a2, passed
// through unloadSingleZone): non-zero calls sub_14036DCA0 (thunk 0x48A63, at
// 0x9FDB20), the zone-state serializer the save paths 0x36E310 / 0x36EAD0 also
// use; 0 discards the zone state, which is what a load needs -- the old world's
// zones must not be written into the save data being loaded.
const size_t RVA_UNLOAD_ZONE_FROM_RESET = 0x365ED;   // thunk -> 0xA09BB0
typedef __int64 (__fastcall *unloadZoneFromReset_t)(void* zoneMgr, void* zoneEntry,
                                                      unsigned char param);
extern unloadZoneFromReset_t fn_unloadZoneFromReset;

// ZoneMap handle registry: the ZoneMapHandleContainerList object at 0x2132F50
// (vtable 0x171F828; a member of the HandleManager at 0x2132F30).
//   +8  (0x2132F58) void** slots  -- ZoneContent::prepareUnload writes
//                                    `*(qword_142132F58 + 8*idx) = sentinel` (0x9FDE30)
//   +16 (0x2132F60) u32    count  -- registerHandle 0x380930 compares the index
//                                    against it (0x38098C); the clear
//                                    sub_1404FDC30 memsets 8*count bytes
// ZoneMapContent's ctor (0xA00720, called only from loadSingleZone) builds the
// HandleDummy, stores the dummy pointer at content+0xA0 (0xA008C1), and
// registers it with hand {type 12, container = ZoneMap+24 + (ZoneMap+28 << 6) + 1,
// serial 11111} (0xA008C8-0xA008F0). registerHandle stores the dummy pointer
// itself in slots[container] when the slot is 0 or the sentinel (0x3809C8-
// 0x3809DF); the list's vt+80 (sub_1408794D0) returns hand+12, the container.
// An empty slot is 0 (after the reset's memset) or *(u64*)RVA_HANDLE_SENTINEL
// (after prepareUnload). The lookup (vt+32, sub_140336C20) refuses both, then
// also compares the dummy's hand serial -- a constant 11111 for zones.
const size_t RVA_ZONEMAP_HANDLE_LIST  = 0x2132F50;
const size_t RVA_ZONEMAP_HANDLE_SLOTS = RVA_ZONEMAP_HANDLE_LIST + 8;    // 0x2132F58
const size_t RVA_ZONEMAP_HANDLE_COUNT = RVA_ZONEMAP_HANDLE_LIST + 16;   // 0x2132F60
const size_t OFF_ZMC_HANDLE_DUMMY     = 0xA0;   // ZoneMapContent::HandleDummy* (dummy+120 = content)
KLIB_ASSERT_OFFSET(ZoneMapContent_handleDummy, OFF_ZMC_HANDLE_DUMMY);
// ---- end save-load reset ----

#endif // KEO_SAVE_LOAD_BINDINGS_H
