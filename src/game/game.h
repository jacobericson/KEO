#include "game/klib_members.h"
// game.h - Umbrella over the game binding headers (rva.h, offsets.h, bindings.h and the module binding headers).
// Module binding headers keep bare pointers beside their RVAs; only bindings.h's own fn_ and orig_ pointers are tables.

#ifndef KENSHI_ZONE_OPT_GAME_H
#define KENSHI_ZONE_OPT_GAME_H

#include "game/klib_bindings.h"

#include "base/core.h"
#include "zone/preload/camera_focus.h"


#include "game/rva.h"
#include "game/offsets.h"
#include "game/bindings.h"
#include "zone/zone_helpers.h"
// =========================================================================
// Game bindings initialization (resolves all function pointers)
// =========================================================================

void InitGameBindings(uintptr_t base);


#include "pathfind/path_pool_bindings.h"

// hkaiKeycode::validate: zeroes the byte at RCX, checks the embedded key, and
// writes 1 on success. Each of its five callers (realGenerate and four
// hkai feature guards) tests its own flag and calls this only on 0.
const size_t RVA_HKAI_KEYCODE_VALIDATE = 0xE76C30;

#include "movement/char_movement_fields.h"

#include "zone/readiness/readiness_bindings.h"

#include "zone/preload_bindings.h"

// ZoneMap+0xB8: TerrainSector* terrainCollision (KenshiLib ZoneManager.h).
// ZoneManager__unloadSingleZone (0xA09620) frees and NULLs it after it NULLs
// mapContent (+0). processJobAlt reads *(zone+0xB8)+8 unconditionally for
// type 0/1 jobs (0x3C1580 via 0xA07B50, called from 0x3CBE60+0xB3C).
const size_t OFF_ZONE_TERRAIN_COLLISION = 0xB8;
KLIB_ASSERT_OFFSET(ZoneMap_terrainCollision, OFF_ZONE_TERRAIN_COLLISION);

#include "zone/reset/save_load_bindings.h"

#include "zone/handoff/first_time_bindings.h"

#include "navmesh/generation/nbr_seed_bindings.h"

#endif // KENSHI_ZONE_OPT_GAME_H
