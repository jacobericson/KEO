// preload_bindings.h - Preload readiness query binding and activation flag.
// Included through game.h.

#ifndef KEO_PRELOAD_BINDINGS_H
#define KEO_PRELOAD_BINDINGS_H

#include "game/klib_members.h"
#include "base/core.h"

// ---- Preload pipeline: zone-ready query ----
// Called, not hooked: bool __fastcall(void* zoneEntry), confirmed in IDA
// (ZoneMapContent__isZoneReady, size 0x2C): `return zoneEntry && *(zoneEntry+184)
// && globalReadyCheck(...)`. No allocation, never blocks.
const size_t RVA_ZONE_IS_READY  = 0xA08E10;
// ZoneMapContent+264, BYTE: the flag ZoneMapContent::update (0x9FF730) tests
// before finalizing. The polarity is the opposite of the name's suggestion:
// the finalize runs only while the flag is NON-zero (and isZoneReady(zoneEntry)
// passes), and clearing the flag to 0 is its own first act. A clear flag means
// the finalize has already run; it never means "ready for it".
// ZoneMap::_activate sets it to 1 on every load, the mod's own included.
const size_t OFF_ZMC_READY_FLAG = 264;
KLIB_ASSERT_OFFSET(ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG);

typedef bool (__fastcall *isZoneReady_t)(void* zoneEntry);
extern isZoneReady_t fn_isZoneReady;
// ---- end preload pipeline: zone-ready query ----

#endif // KEO_PRELOAD_BINDINGS_H
