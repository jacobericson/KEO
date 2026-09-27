#pragma once
#include "game/klib_member_contract.h"
#include <stdint.h>
#define KLIB_FIELD(N,T,M,O,W) uintptr_t KlibField_##N(uintptr_t base);
#include "game/klib_member_fields.inc"
#include "game/klib_member_combat.inc"
#undef KLIB_FIELD
// Keep reads/stores at their original sites, including volatile, Interlocked and SEH.
// G names the member group; the macro no longer branches on it, so it is a
// vestigial argument kept for call-site compatibility.
#define KLIB_MEMBER(G,B,N,O) ((void)sizeof(KlibOffsetCheck<(O), KLIB_OFF_##N>), KlibField_##N((uintptr_t)(B)))
uintptr_t KlibZoneEntry(uintptr_t base, size_t index);
uintptr_t KlibField_ZoneMapContent_items_size(uintptr_t base);
uintptr_t KlibZoneNeighbor(uintptr_t base, size_t index);
#define KLIB_ZONE_NEIGHBOR(G,B,I) KlibZoneNeighbor((uintptr_t)(B), (I))
uintptr_t KlibField_PlayerInterface_selected_count(uintptr_t base);
uintptr_t KlibField_PlayerInterface_selected_buckets(uintptr_t base);
uintptr_t KlibField_PlayerInterface_selected_bucketCount(uintptr_t base);
uintptr_t KlibField_HandSetNode_handle_type(uintptr_t base);
uintptr_t KlibField_Blackboard_requests_size(uintptr_t base);
uintptr_t KlibField_SectorNode_mapped(uintptr_t base);
uintptr_t KlibField_GameWorld_deathParade_size(uintptr_t base);
uintptr_t KlibField_GameWorld_charUpdateListMain_size(uintptr_t base);
uintptr_t KlibField_ZoneManager_activeZones_size(uintptr_t base);
unsigned long KlibRootNextFrame(uintptr_t base);
uintptr_t KlibMeshPointer(uintptr_t base);
size_t KlibEffectCount(uintptr_t base);
uintptr_t KlibEffectData(uintptr_t base);
uintptr_t KlibCompositorEntry(uintptr_t base, size_t index);

uintptr_t KlibField_hkVector4f_x(uintptr_t base);
uintptr_t KlibField_hkVector4f_y(uintptr_t base);
uintptr_t KlibField_hkVector4f_z(uintptr_t base);
uintptr_t KlibField_hkVector4f_w(uintptr_t base);
