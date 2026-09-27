#pragma once
#include <stddef.h>
#define KLIB_FIELD(N,T,M,O,W) enum { KLIB_OFF_##N = O, KLIB_WIDTH_##N = W };
#include "game/klib_member_fields.inc"
#include "game/klib_member_combat.inc"
#undef KLIB_FIELD
#define KLIB_ASSERT_OFFSET(N,O) static_assert((O)==KLIB_OFF_##N, #N " legacy offset drift")

// Real node offset is independently proved in dependent SectorNodeProof.
enum { KLIB_OFF_SectorMapNode_value = 24 };
enum { KLIB_ZONE_STRIDE = 360, KLIB_ZONE_DIMENSION = 64 };
enum { KLIB_SIZE_NavMeshGenerator = 352 };
enum { KLIB_OFF_ZoneMapContent_items_size = KLIB_OFF_ZoneMapContent_items + KLIB_OFF_HandSetTable_size_, KLIB_WIDTH_ZoneMapContent_items_size = KLIB_WIDTH_HandSetTable_size_ };
enum { KLIB_OFF_PlayerInterface_selected_count = KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_size_, KLIB_WIDTH_PlayerInterface_selected_count = KLIB_WIDTH_HandSetTable_size_ };
enum { KLIB_OFF_PlayerInterface_selected_buckets = KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_buckets_, KLIB_WIDTH_PlayerInterface_selected_buckets = KLIB_WIDTH_HandSetTable_buckets_ };
enum { KLIB_OFF_PlayerInterface_selected_bucketCount = KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_bucket_count_, KLIB_WIDTH_PlayerInterface_selected_bucketCount = KLIB_WIDTH_HandSetTable_bucket_count_ };
enum { KLIB_OFF_HandSetNode_handle_type = KLIB_OFF_HandSetNode_value_base_ + KLIB_OFF_hand_type, KLIB_WIDTH_HandSetNode_handle_type = KLIB_WIDTH_hand_type };
enum { KLIB_OFF_Blackboard_requests_size = KLIB_OFF_Blackboard_requests + KLIB_OFF_RequestMapTable_size_, KLIB_WIDTH_Blackboard_requests_size = KLIB_WIDTH_RequestMapTable_size_ };
enum { KLIB_OFF_GameWorld_deathParade_size = KLIB_OFF_GameWorld_deathParade + KLIB_OFF_DeathMapTable_size_, KLIB_WIDTH_GameWorld_deathParade_size = KLIB_WIDTH_DeathMapTable_size_ };
enum { KLIB_OFF_GameWorld_charUpdateListMain_size = KLIB_OFF_GameWorld_charUpdateListMain + KLIB_OFF_CharacterSetTable_size_, KLIB_WIDTH_GameWorld_charUpdateListMain_size = KLIB_WIDTH_CharacterSetTable_size_ };
enum { KLIB_OFF_ZoneManager_activeZones_size = KLIB_OFF_ZoneManager_activeZones + KLIB_OFF_ZoneSetTable_size_, KLIB_WIDTH_ZoneManager_activeZones_size = KLIB_WIDTH_ZoneSetTable_size_ };
enum { KLIB_OFF_SectorNode_mapped = KLIB_OFF_SectorMapNode_value + KLIB_OFF_SectorValue_second, KLIB_WIDTH_SectorNode_mapped = 8, KLIB_SIZE_Compositor = 48, KLIB_OFF_Root_nextFrame = 0x190 };
// Ogre::Root frame-listener sets: Ogre::set<FrameListener*>::type, 40 bytes each.
enum { KLIB_OFF_Root_frameListeners = 0x288, KLIB_OFF_Root_removedFrameListeners = 0x2B0, KLIB_SIZE_Root_listenerSet = 40 };
// Ogre::Entity::updateAnimation's early-out reads, past the Entity itself.
enum { KLIB_OFF_MovableObject_parentNode = 0x28, KLIB_OFF_AnimationStateSet_dirtyFrameNumber = 0x18, KLIB_OFF_Node_transform = 0x38, KLIB_OFF_Transform_index = 0, KLIB_OFF_Transform_derivedTransform = 0x48 };
// Ogre::GpuProgramParameters::mNamedConstants, a SharedPtr<GpuNamedConstants> (raw pointer first).
enum { KLIB_OFF_GpuProgramParameters_namedConstants = 0x128 };
template<size_t Actual, size_t Expected> struct KlibOffsetCheck
{
 static_assert(Actual == Expected, "legacy offset differs from named member");
};

// Components of the public SIMD union are proved separately for VS2010 offsetof.
enum { KLIB_OFF_hkVector4f_x = 0, KLIB_WIDTH_hkVector4f_x = 4 };
enum { KLIB_OFF_hkVector4f_y = 4, KLIB_WIDTH_hkVector4f_y = 4 };
enum { KLIB_OFF_hkVector4f_z = 8, KLIB_WIDTH_hkVector4f_z = 4 };
enum { KLIB_OFF_hkVector4f_w = 12, KLIB_WIDTH_hkVector4f_w = 4 };
