// audit_detail.h - Shared frame audit types, constants and state.
// Included only by KenshiFrameAudit.cpp and audit_*.cpp.
// The reporter owns every file; the main thread owns every game read.
// No probe takes a lock, allocates, or logs.

#ifndef KENSHI_FRAME_AUDIT_DETAIL_H
#define KENSHI_FRAME_AUDIT_DETAIL_H

#include "game/klib_members.h"
#include <Windows.h>
#include <intrin.h>
#include <string>
#include <vector>
#include "CallSiteProbe.h"
#include "KenshiFrameAudit.h"
#include "KenshiFrameAudit_internal.h"
#include "game/klib_bindings.h"

namespace kenshiframeaudit_detail {

using namespace audit;

#define KLIB_ROOT_FRAME(B) KlibRootNextFrame((uintptr_t)(B))
#define KLIB_MESH_POINTER(B) KlibMeshPointer((uintptr_t)(B))
#define KLIB_EFFECT_COUNT(B) KlibEffectCount((uintptr_t)(B) + EM_ACTIVE_DATA)
#define KLIB_EFFECT_DATA(B) KlibEffectData((uintptr_t)(B) + EM_ACTIVE_DATA)
// =========================================================================
// Game addresses (Steam 1.0.65, verified in IDA 2026-09-13)
// =========================================================================

const size_t RVA_GAMEWORLD      = 0x21330B0;  // GameWorld object (IDB label `pauseState`)
const size_t RVA_OPTIONS        = 0x2132440;  // OptionsHolder
const size_t RVA_RENDERER       = 0x21322B8;  // renderer holder; +0x60 = main SceneManager
const size_t RVA_FULLRATE_COUNT = 0x2131E84;  // full-rate characters, counted by charsUpdate

const size_t GW_FACTIONMGR = 0x4A8;
KLIB_ASSERT_OFFSET(GameWorld_factionMgr, GW_FACTIONMGR);
const size_t GW_PLAYER     = 0x580;
KLIB_ASSERT_OFFSET(GameWorld_player, GW_PLAYER);
const size_t GW_SPEED      = 0x700;   // float frameSpeedMult
KLIB_ASSERT_OFFSET(GameWorld_frameSpeedMult, GW_SPEED);
const size_t GW_DEAD_COUNT = 0x728;   // deathParade size
static_assert(GW_DEAD_COUNT == KLIB_OFF_GameWorld_deathParade + KLIB_OFF_DeathMapTable_size_, "GW_DEAD_COUNT composed parity");
const size_t GW_CHAR_COUNT = 0x770;   // charUpdateListMain size
static_assert(GW_CHAR_COUNT == KLIB_OFF_GameWorld_charUpdateListMain + KLIB_OFF_CharacterSetTable_size_, "GW_CHAR_COUNT composed parity");
const size_t GW_AITHREAD   = 0x790;   // RenderTimeBackthread*
KLIB_ASSERT_OFFSET(GameWorld__AINonRenderThread, GW_AITHREAD);
const size_t GW_ZONEMGR    = 0x8B0;
KLIB_ASSERT_OFFSET(GameWorld_zoneMgr, GW_ZONEMGR);
const size_t GW_PAUSED     = 0x8B9;
KLIB_ASSERT_OFFSET(GameWorld_paused, GW_PAUSED);

const size_t ZM_SETB_COUNT = 0x168128;
static_assert(ZM_SETB_COUNT == KLIB_OFF_ZoneManager_activeZones + KLIB_OFF_ZoneSetTable_size_, "ZM_SETB_COUNT composed parity");
const size_t ZM_STATE      = 0x1681D8;
KLIB_ASSERT_OFFSET(ZoneManager_loadingPhase, ZM_STATE);
const size_t ZM_ZONE_BASE  = 200;
KLIB_ASSERT_OFFSET(ZoneManager_worldMap, ZM_ZONE_BASE);
const size_t ZONE_STRIDE   = 360;
static_assert(ZONE_STRIDE == KLIB_ZONE_STRIDE, "ZONE_STRIDE composed parity");
const size_t ZONE_LOADING  = 176;
KLIB_ASSERT_OFFSET(ZoneMap_stateT_mainThreadData__zoneBeingLoaded, ZONE_LOADING);
const size_t ZONE_ACCESS   = 177;
KLIB_ASSERT_OFFSET(ZoneMap_stateT_mainThreadData__zoneIsLoaded, ZONE_ACCESS);
const int    ZONE_GRID     = 64;
static_assert(ZONE_GRID == KLIB_ZONE_DIMENSION, "ZONE_GRID composed parity");

const size_t FM_COUNT      = 0x8;     // participants lektor: count, data
KLIB_ASSERT_OFFSET(FactionManager_participants_count, FM_COUNT);
const size_t FM_DATA       = 0x10;
KLIB_ASSERT_OFFSET(FactionManager_participants_stuff, FM_DATA);
const size_t FACTION_ACTIVE_PLATOONS = 0x210;   // activePlatoons.count (lektor at 0x208)
KLIB_ASSERT_OFFSET(Faction_activePlatoons_count, FACTION_ACTIVE_PLATOONS);

const size_t PLAYER_CAMERA   = 0x30;  // CameraClass*
KLIB_ASSERT_OFFSET(PlayerInterface_camera, PLAYER_CAMERA);
const size_t CAMERA_ALTITUDE = 0x60;  // float
KLIB_ASSERT_OFFSET(CameraClass_altitude, CAMERA_ALTITUDE);

const size_t RENDERER_SCENEMGR = 0x60;
KLIB_ASSERT_OFFSET(Renderer_scene, RENDERER_SCENEMGR);

const size_t AI_LIST_TU     = 0x1A8;  // RenderTimeBackthread list counts, read at body entry
const size_t AI_LIST_TU4    = 0x1C0;
const size_t AI_LIST_TUP    = 0x1D8;
const size_t THREAD_RUNNING = 0x14;   // ThreadClass::_running
KLIB_ASSERT_OFFSET(ThreadClass__running, THREAD_RUNNING);

// OptionsHolder members (layout from KenshiLib's OptionsHolder.h).
const size_t OPT_VIEW         = 0x18;   // float VIEW_DISTANCE
KLIB_ASSERT_OFFSET(OptionsHolder_VIEW_DISTANCE, OPT_VIEW);
const size_t OPT_TERRAIN      = 0x1C;   // float terrainDetail
KLIB_ASSERT_OFFSET(OptionsHolder_terrainDetail, OPT_TERRAIN);
const size_t OPT_GRASS_RANGE  = 0x2C;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_grassRange, OPT_GRASS_RANGE);
const size_t OPT_GRASS_DENS   = 0x30;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_grassDensity, OPT_GRASS_DENS);
const size_t OPT_FOLIAGE      = 0x34;   // float foliageRange
KLIB_ASSERT_OFFSET(OptionsHolder_foliageRange, OPT_FOLIAGE);
const size_t OPT_NPC_RANGE    = 0x38;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_NPCRange, OPT_NPC_RANGE);
const size_t OPT_OBJ_RANGE    = 0x3C;   // float smallBuildingRange
KLIB_ASSERT_OFFSET(OptionsHolder_smallBuildingRange, OPT_OBJ_RANGE);
const size_t OPT_FANCY        = 0x41;   // bool fancyShaders
KLIB_ASSERT_OFFSET(OptionsHolder_fancyShaders, OPT_FANCY);
const size_t OPT_POPULATION   = 0x48;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_populationMult, OPT_POPULATION);
const size_t OPT_SHADOWS      = 0x5C;   // int shadowMode
KLIB_ASSERT_OFFSET(OptionsHolder_shadowMode, OPT_SHADOWS);
const size_t OPT_SHADOW_Q     = 0x60;   // int shadowQuality
KLIB_ASSERT_OFFSET(OptionsHolder_shadowQuality, OPT_SHADOW_Q);
const size_t OPT_DECAL_RANGE  = 0x68;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_decalRange, OPT_DECAL_RANGE);
const size_t OPT_CHAR_MT      = 0x70;   // byte
KLIB_ASSERT_OFFSET(OptionsHolder_characterMultithreading, OPT_CHAR_MT);
const size_t OPT_NAMES        = 0x78;   // byte
KLIB_ASSERT_OFFSET(OptionsHolder_showNames, OPT_NAMES);
const size_t OPT_MANY_ZONES   = 0x88;   // bool manyActiveZones
KLIB_ASSERT_OFFSET(OptionsHolder_manyActiveZones, OPT_MANY_ZONES);
const size_t OPT_DIST_TOWN    = 0x8C;   // float distantTownRange
KLIB_ASSERT_OFFSET(OptionsHolder_distantTownRange, OPT_DIST_TOWN);
const size_t OPT_FEATURE      = 0x90;   // float featureRange
KLIB_ASSERT_OFFSET(OptionsHolder_featureRange, OPT_FEATURE);
const size_t OPT_SHADOW_RANGE = 0x98;   // float
KLIB_ASSERT_OFFSET(OptionsHolder_shadowRange, OPT_SHADOW_RANGE);
const size_t OPT_WATER        = 0xA0;   // int reflectionMode
KLIB_ASSERT_OFFSET(OptionsHolder_reflectionMode, OPT_WATER);
const size_t OPT_REFL_DIST    = 0xA4;   // float reflectionDistance
KLIB_ASSERT_OFFSET(OptionsHolder_reflectionDistance, OPT_REFL_DIST);
const size_t OPT_COMPOSITORS  = 0xA8;   // lektor<std::pair<std::string,bool> >
KLIB_ASSERT_OFFSET(OptionsHolder_compositors, OPT_COMPOSITORS);
const size_t LEKTOR_COUNT     = 0x8;
KLIB_ASSERT_OFFSET(HandLektor_count, LEKTOR_COUNT);
const size_t LEKTOR_DATA      = 0x10;
KLIB_ASSERT_OFFSET(HandLektor_stuff, LEKTOR_DATA);
// VS2010 x64 std::string is 40 bytes (buffer/pointer, size, capacity, then
// the allocator), so a pair<string,bool> is 48 with the flag at +40: the
// game's own reader of this list (0x812980) steps 48 and reads +40.
const size_t COMPOSITOR_ENTRY = 48;
static_assert(COMPOSITOR_ENTRY == KLIB_SIZE_Compositor, "COMPOSITOR_ENTRY parity");
const size_t COMPOSITOR_FLAG  = 40;
KLIB_ASSERT_OFFSET(Compositor_second, COMPOSITOR_FLAG);

// Entry detours in kenshi_x64.exe.
const size_t RVA_FRAME_STARTED  = 0x82A780;
const size_t RVA_FRAME_QUEUED   = 0x82A0C0;
const size_t RVA_FRAME_ENDED    = 0x82AAF0;
const size_t RVA_AI_BODY        = 0x786E00;  // RenderTimeBackthread vtable slot 2
const size_t RVA_PHYS_BODY      = 0x7DC4C0;  // PhysicsActual vtable slot 2
const size_t RVA_BIRDS_BODY     = 0x2627F0;  // BirdManager vtable slot 2
const size_t RVA_PHYS_UT        = 0x4CD040;  // PhysicsActual::updateUT (indirect call)
const size_t RVA_IS_INDOORS     = 0x9B1CD0;  // UtilityT::isIndoors (1-2 closest-shape rays)
const size_t RVA_PHYS_MAKE_HULL = 0x4CAEA0;  // PhysicsHullT slot +0x30
const size_t RVA_PHYS_MAKE_FILE = 0x4CCA60;  // SimplePhysXEntity / DoorPhysXEntity slot +0x30
const size_t RVA_PHYS_MAKE_SCYTHE = 0x4CAF90;// ScythePhysicsT / ScytheRagdollPhysicsT slot +0x30
const size_t RVA_PHYS_FINISH_SCYTHE = 0x7E0B50;// Scythe slot +0x38
const size_t RVA_PHYS_APPLY_HULL = 0x4CB0E0; // PhysicsHullT slot +0x28
const size_t RVA_PHYS_APPLY_DOOR = 0x4CB6F0; // DoorPhysXEntity slot +0x28
const size_t RVA_ZONE_LIFECYCLE = 0xA0A960;  // returns 0 when it unloaded the zone
const size_t RVA_CHARBODY_UPD   = 0x5C6290;  // CharBody vtable slot 1: runs the current task (tail call)
const size_t RVA_CHARMOVE_UPD   = 0x65F510;  // CharMovement vtable slot 11 (+0x58), (this, float dt)
const size_t CHARBODY_TASK      = 0x68;      // CharBody: current Task_* object
KLIB_ASSERT_OFFSET(CharBody_currentAction, CHARBODY_TASK);
const size_t CHARBODY_CHAR      = 0x18;      // CharBody: owning Character*
KLIB_ASSERT_OFFSET(CharBody_character, CHARBODY_CHAR);
const size_t CHAR_ACTIVE_PLATOON = 0x658;    // Character: ActivePlatoon* (list 1 requires it)
KLIB_ASSERT_OFFSET(Character_platoon, CHAR_ACTIVE_PLATOON);

// Save / autosave pipeline. SaveManager::execute is already timed by the
// `save` call-site probe; these split the blocking saveGame it calls.
const size_t RVA_SM_SAVEGAME    = 0x375220;  // SaveManager::saveGame (the whole stall)
const size_t RVA_SM_UPDATEAUTO  = 0x47B600;  // SaveManager::updateAutoSave (per frame, the timer)
const size_t RVA_ZM_SAVESTATES  = 0x36E310;  // ZoneManager::saveActiveZoneStates (Set B loop)
const size_t RVA_ZMC_SAVELEVEL  = 0x36DCA0;  // ZoneMapContent::saveLevelData (per zone)
const size_t RVA_ROC_SERIALISE  = 0x36C8A0;  // RootObjectContainer::serialiseThings(lektor)
const size_t RVA_ZMC_SAVEITEMS  = 0x36B710;  // ZoneMapContent::saveItems
const size_t RVA_ZMC_SAVEDISK   = 0x36BF30;  // ZoneMapContent::saveToDisk
const size_t RVA_GDC_SAVE       = 0x6BD220;  // GameDataContainer::save (the ofstream write)
const size_t RVA_TL_SAVEUNIQUE  = 0x9297F0;  // TownList::saveUniqueTownsAndNests
const size_t RVA_TL_SAVESTATE   = 0x36B5D0;  // TownList::saveState
const size_t RVA_FM_SAVEPLAYER  = 0x374FC0;  // FactionManager::savePlayerGameState
const size_t RVA_FM_SAVESTATE   = 0x3750D0;  // FactionManager::saveGameState
const size_t RVA_PM_SAVETEX     = 0x4148E0;  // PortraitManager::saveTexture (render-to-texture)
const size_t RVA_SFS_SAVEGAME   = 0x473380;  // SaveFileSystem::saveGame (queues the async copy)
const size_t RVA_SFS_SYNC       = 0x473F10;  // SaveFileSystem::sync (main thread, polls the copy)

const size_t SM_SIGNAL         = 0xA0;   // SaveManager: int signal (1 = save)
const size_t SM_FLAGS          = 0xA4;   // SaveManager: int flags (1 = autosave)
const size_t SM_AUTOSAVE_TIMER = 0x118;  // SaveManager: float autoSaveTimer (-1 = armed)
const size_t SFS_MSGS_COUNT    = 0x1D0;  // SaveFileSystem: messages.count (lektor)
const size_t SFS_STATE         = 0x1E0;  // SaveFileSystem: State (0 idle, 1 saving, 2 complete)
const size_t ZMC_ZONEMAP       = 0xD0;   // ZoneMapContent: ZoneMap*
const size_t OPT_AUTOSAVE_TIME = 0xDC;   // OptionsHolder: float autosavetime (minutes)
const size_t OPT_AUTOSAVE_ON   = 0xE2;   // OptionsHolder: bool autosave
const size_t ROC_THINGS_COUNT  = 0x58;   // RootObjectContainer: things.count
KLIB_ASSERT_OFFSET(RootObjectContainer_things_count, ROC_THINGS_COUNT);
const size_t ZONE_COORD_X      = 0x18;   // ZoneMap: coordinates.x
KLIB_ASSERT_OFFSET(ZoneMap_coordinates_x, ZONE_COORD_X);
const size_t ZONE_COORD_Y      = 0x1C;   // ZoneMap: coordinates.y
KLIB_ASSERT_OFFSET(ZoneMap_coordinates_y, ZONE_COORD_Y);

// First 16 bytes of every detour target (IDA / OgreMain_x64.dll, 2026-09-13).
const unsigned char PRO_FRAME_STARTED[16]  = { 0x40,0x57,0x48,0x83,0xEC,0x60,0x48,0xC7,0x44,0x24,0x28,0xFE,0xFF,0xFF,0xFF,0x48 };
const unsigned char PRO_FRAME_QUEUED[16]   = { 0x40,0x53,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24,0x28,0xFE,0xFF,0xFF,0xFF,0x0F };
const unsigned char PRO_FRAME_ENDED[16]    = { 0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x38 };
const unsigned char PRO_AI_BODY[16]        = { 0x40,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,0x48 };
const unsigned char PRO_PHYS_BODY[16]      = { 0x48,0x8B,0xC4,0x57,0x48,0x83,0xEC,0x70,0x48,0xC7,0x44,0x24,0x28,0xFE,0xFF,0xFF };
const unsigned char PRO_BIRDS_BODY[16]     = { 0x48,0x8B,0xC4,0x55,0x41,0x54,0x48,0x8D,0xA8,0x38,0xFD,0xFF,0xFF,0x48,0x81,0xEC };
const unsigned char PRO_PHYS_UT[16]        = { 0x40,0x56,0x41,0x54,0x48,0x83,0xEC,0x28,0x48,0x8B,0xF1,0x48,0x81,0xC1,0xA0,0x01 };
const unsigned char PRO_IS_INDOORS[16]     = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x57,0x48,0x81,0xEC,0x20,0x01,0x00,0x00,0x0F };
const unsigned char PRO_PHYS_MAKE_HULL[16] = { 0x40,0x56,0x48,0x83,0xEC,0x20,0x48,0x83,0x79,0x50,0x00,0x48,0x8B,0xF1,0x75,0x34 };
const unsigned char PRO_PHYS_MAKE_FILE[16] = { 0x4C,0x8B,0xDC,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x20 };
const unsigned char PRO_PHYS_MAKE_SCYTHE[16] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x0D,0x28,0x81,0xC6,0x01 };
const unsigned char PRO_PHYS_FINISH_SCYTHE[16] = { 0x88,0x54,0x24,0x10,0x53,0x48,0x83,0xEC,0x30,0x48,0x8D,0x99,0xC0,0x00,0x00,0x00 };
const unsigned char PRO_PHYS_APPLY_HULL[16] = { 0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xD9,0x48,0x8B,0x49,0x50,0x48,0x85,0xC9 };
const unsigned char PRO_PHYS_APPLY_DOOR[16] = { 0x40,0x53,0x48,0x83,0xEC,0x30,0x83,0x79,0x58,0x00,0x48,0x8B,0xD9,0x0F,0x84,0xDF };
const unsigned char PRO_ZONE_LIFECYCLE[16] = { 0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x81,0x88,0x00,0x00 };
const unsigned char PRO_CHARBODY_UPD[16]   = { 0x40,0x53,0x48,0x83,0xEC,0x50,0x48,0x8B,0x01,0x0F,0x29,0x74,0x24,0x40,0x48,0x8B };
const unsigned char PRO_CHARMOVE_UPD[16]   = { 0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D };

const unsigned char PRO_SM_SAVEGAME[16]   = { 0x48,0x8B,0xC4,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81 };
const unsigned char PRO_SM_UPDATEAUTO[16] = { 0x48,0x8B,0xC4,0x55,0x48,0x8D,0x6C,0x24,0x80,0x48,0x81,0xEC,0x80,0x01,0x00,0x00 };
const unsigned char PRO_ZM_SAVESTATES[16] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x83,0xB9,0x28,0x81,0x16,0x00,0x00,0x48,0x8B };
const unsigned char PRO_ZMC_SAVELEVEL[16] = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24 };
const unsigned char PRO_ROC_SERIALISE[16] = { 0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xEC };
const unsigned char PRO_ZMC_SAVEITEMS[16] = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24 };
const unsigned char PRO_ZMC_SAVEDISK[16]  = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC };
const unsigned char PRO_GDC_SAVE[16]      = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 };
const unsigned char PRO_TL_SAVEUNIQUE[16] = { 0x48,0x8B,0xC4,0x41,0x54,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x48,0xC7,0x44,0x24 };
const unsigned char PRO_TL_SAVESTATE[16]  = { 0x48,0x8B,0xC4,0x57,0x41,0x54,0x41,0x55,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48 };
const unsigned char PRO_FM_SAVEPLAYER[16] = { 0x48,0x8B,0xC4,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x20 };
const unsigned char PRO_FM_SAVESTATE[16]  = { 0x48,0x8B,0xC4,0x41,0x54,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0xC7,0x44,0x24 };
const unsigned char PRO_PM_SAVETEX[16]    = { 0x40,0x57,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x30,0xFE,0xFF };
const unsigned char PRO_SFS_SAVEGAME[16]  = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 };
const unsigned char PRO_SFS_SYNC[16]      = { 0x48,0x8B,0xC4,0x55,0x48,0x8D,0x68,0xE8,0x48,0x81,0xEC,0x10,0x01,0x00,0x00,0x48 };

const unsigned char PRO_RENDER_ONE_FRAME[16] = { 0x40,0x56,0x48,0x83,0xEC,0x40,0x48,0x8B,0xF1,0xE8,0x22,0xFB,0xFF,0xFF,0x84,0xC0 };
const unsigned char PRO_UPDATE_SCENE[16]     = { 0x41,0x54,0x48,0x83,0xEC,0x40,0x48,0x89,0x5C,0x24,0x60,0x4C,0x8B,0xE1,0x48,0x8B };
const unsigned char PRO_CM2_UPDATE[16]       = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48 };
const unsigned char PRO_CM2_SWAP[16]         = { 0x4C,0x8B,0xDC,0x57,0x41,0x54,0x41,0x55,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24 };
const unsigned char PRO_RS_RENDER[16]        = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x80,0x7A,0x0C,0x00,0x4C,0x8B,0xCA,0x48,0x8B,0xD9 };
const unsigned char PRO_RENDER_PHASE02[16]   = { 0x4C,0x8B,0xDC,0x4D,0x89,0x4B,0x20,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
const unsigned char PRO_CULL_PHASE01[16]     = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8B,0xEC,0x48 };
const unsigned char PRO_RENDER_VISIBLE[16]   = { 0x48,0x8B,0x81,0xE0,0x05,0x00,0x00,0x48,0x8B,0x50,0x70,0x48,0x85,0xD2,0x74,0x13 };
const unsigned char PRO_RSO[16]              = { 0x4C,0x8B,0xDC,0x45,0x88,0x4B,0x20,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56 };
const unsigned char PRO_SET_PASS[16]         = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 };

const int   OGRE_STAGE_TEXTURE_SHADOWS = 1;   // SceneManager::IRS_RENDER_TO_TEXTURE

// Scene rendering: Kenshi's CSM loop (0x863CB0, inside the DeferredLightingPass
// compositor pass) and the compositor scene passes both go through these.

// Accessors used to name what is drawn (one-instruction getters in OgreMain).

// RenderOperation::numberOfInstances (release layout, no srcRenderable;
// RenderSystem::_render reads [op+0x18]).
const size_t OP_NUM_INSTANCES = 0x18;
KLIB_ASSERT_OFFSET(Ogre__RenderOperation_numberOfInstances, OP_NUM_INSTANCES);

// Per-pass fixed costs: the worker barrier (every fork/join wakes 15 threads
// through semaphores) and Kenshi's per-cull animation pre-pass.
const unsigned char PRO_BARRIER_SYNC[16] = { 0x48,0x83,0xEC,0x28,0x48,0x8B,0x41,0x08,0x4C,0x8B,0xC9,0x48,0x89,0x44,0x24,0x30 };
const unsigned char PRO_OLD_ANIMS[16]    = { 0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41 };

// D3D11 render system (not exported; D3D11RenderSystem vtable 0x7B618 slots
// +0x310 and +0x338, verified in the DLL): the per-draw submission and the
// per-draw constant-buffer path (getConstantBuffer's string-keyed lookups).
const size_t RVA_D3D_RENDER      = 0x34170;   // D3D11RenderSystem::_render(const RenderOperation&)
const size_t RVA_D3D_BIND_PARAMS = 0x36F50;   // ::bindGpuProgramParameters(type, SharedPtr params, uint16 mask)
const unsigned char PRO_D3D_RENDER[16] = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 };
const unsigned char PRO_D3D_BIND[16]   = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x8D,0xAC,0x24,0x08,0xF7,0xFF };

// Visibility flags Kenshi gives character attachments (4) and attached
// objects (0x1000): the far-cascade caster candidates.
const unsigned VIS_ATTACHMENTS = 0x1004;

// Particle effects (verified in IDA / Plugin_ParticleUniverse_x64.dll 2026-09-13).
// EffectsManager::update (0x40A4E0, the `particles` mainLoop section) runs
// ParticleUniverse::ParticleSystem::_update(scaled dt) once for every active
// effect, spread round-robin over the Ogre worker pool (EffectsManagerUpdateJob
// 0x40ECD0). Zone effects (0x40BA60, inside updateCameraZone) call the same
// _update serially on the main thread. Kenshi pools effects and their particle
// system handlers per effect type, so churn is entries into / exits from the
// active list, not ParticleUniverse creations.
const size_t RVA_EFFECTS_MGR = 0x2127190;  // EffectsManager* (lazy singleton, 296 bytes)
const size_t EM_ACTIVE_DATA  = 200;        // Ogre::FastArray<Effect*> of active effects: data
const size_t EM_ACTIVE_SIZE  = 208;        //   element count
// Private mSize offset is verified with the VS2010 compiler's class layout report.
static_assert(EM_ACTIVE_SIZE == EM_ACTIVE_DATA + 8, "active FastArray count composed legacy parity");
const size_t FX_HANDLER      = 24;         // Effect: ParticleSystemHandler* (NULL: no particles)
const size_t FX_AGE          = 48;         // Effect: float, += scaled dt each update (0xF9570)
const size_t FX_STATE        = 92;         // Effect: int, 1 = stopping (removed once PU stops), 2 = dead
const size_t PSH_SYSTEM      = 168;        // ParticleSystemHandler: ParticleUniverse::ParticleSystem*
const size_t PS_LAST_VISIBLE = 0x274;      // Root frame number stamped by _updateRenderQueue
const size_t PS_NONVIS_SET   = 0x27C;      // bool: nonvisible_update_timeout set
const size_t PS_TEMPLATE     = 0x380;      // std::string, ParticleSystem::getTemplateName
const size_t ROOT_NEXT_FRAME = 0x190;      // Ogre::Root::getNextFrameNumber
static_assert(ROOT_NEXT_FRAME == KLIB_OFF_Root_nextFrame, "ROOT_NEXT_FRAME parity");

const unsigned char PRO_PU_UPDATE[16] = { 0x40,0x56,0x48,0x83,0xEC,0x40,0x48,0x83,0x79,0x28,0x00,0x0F,0x29,0x74,0x24,0x30 };
// The field layout above, checked against the code that uses it before any
// field is read: _update's visibility test (+0x3A: mov ecx,[rax+190h];
// sub ecx,[rsi+274h]), getTemplateName (lea rax,[rcx+380h]) and
// Root::getNextFrameNumber (mov eax,[rcx+190h]).
const size_t        PU_UPDATE_VISTEST = 0x3A;
const unsigned char PU_VISTEST_BYTES[12]  = { 0x8B,0x88,0x90,0x01,0x00,0x00,0x2B,0x8E,0x74,0x02,0x00,0x00 };
const unsigned char PU_TEMPLATE_BYTES[8]  = { 0x48,0x8D,0x81,0x80,0x03,0x00,0x00,0xC3 };
const unsigned char ROOT_FRAME_BYTES[7]   = { 0x8B,0x81,0x90,0x01,0x00,0x00,0xC3 };

// Default of the INI key CursorCharGroups: collision groups 23-26, the character
// shapes. Unconfirmed: ragdoll actors get group 3
// (outdoors) or 4 (indoors) (AnimationClass::ragdollModeUT 0x5B90CD..0x5B9226 ->
// PhysFileParams+0x70 -> ScytheRagdollPhysicsT+0x18 -> setActorCollisionGroup),
// and mouseScan finds characters in groups 0/1/3/4 (mask 0x1B, 0x7FF8E3) while it
// treats 23-26 as a building flag (0x7FFCDD). The per-group hit histogram decides.
const unsigned CHAR_GROUPS       = 0x07800000;
// traceAll's code from the scene load to the qsort call: every byte the shadow
// rays and the hit-list reader rely on (nWorld +0xE8, the vt+0x370 call with
// shapes 3 / hint 0x11 / NULL groupsMask, the FLT_MAX load, the lektor count
// +8 / data +0x10 and the 0x28 element size passed to qsort).
const unsigned char TRACE_CALLSEQ_BYTES[88] = {
	0x48,0x8B,0x88,0xE8,0x00,0x00,0x00,            // mov rcx,[rax+0E8h]   (physics->nWorld)
	0x48,0x8B,0x01,                                // mov rax,[rcx]
	0x48,0xC7,0x44,0x24,0x38,0x00,0x00,0x00,0x00,  // groupsMask = NULL
	0xC7,0x44,0x24,0x30,0x11,0x00,0x00,0x00,       // hintFlags = 0x11
	0xF3,0x0F,0x10,0x05,0x64,0x82,0xCE,0x00,       // movss xmm0,[X]       (FLT_MAX)
	0xF3,0x0F,0x11,0x44,0x24,0x28,                 // maxDist
	0x89,0x7C,0x24,0x20,                           // groups
	0x41,0xB9,0x03,0x00,0x00,0x00,                 // shapesType = 3
	0x4C,0x8D,0x44,0x24,0x48,                      // report
	0x48,0x8D,0x54,0x24,0x58,                      // ray
	0xFF,0x90,0x70,0x03,0x00,0x00,                 // call [rax+370h]
	0x90,                                          // nop
	0x8B,0x53,0x08,                                // count = result+8
	0x4C,0x8D,0x0D,0xE2,0x18,0x65,0xFF,            // comparator (thunk to 0x9B0950)
	0x41,0xB8,0x28,0x00,0x00,0x00,                 // element size 0x28
	0x48,0x8B,0x4B,0x10                            // data = result+0x10
};
// InputHandler `key` (KenshiLib InputHandler, 288 bytes) and the previous-frame
// button copies PlayerInterface::update (0x800A80) keeps.
const size_t RVA_INPUT_KEY    = 0x2132320;
const size_t IH_CTRL          = 0xD8;   // ctrl, shift, alt: 3 bools
const size_t IH_MLEFT         = 0xF3;   // mLeft, mRight, lastMLeft, lastMRight, mLDown, mRDown, mLUp, mRUp
const size_t IH_MPOS          = 0xFC;   // Ogre::Vector2 mPos
const size_t RVA_PREV_MRIGHT  = 0x2132281;  // byte: last frame's mRight
const size_t RVA_PREV_MLEFT   = 0x2132282;  // byte: last frame's mLeft

// =========================================================================
// Frame record
// =========================================================================

// "nChars" (character count) is distinct from the "chars" mainLoop section.
#define AUDIT_COUNTS(X) \
	X(CHARS, "nChars") X(DEAD, "dead") X(FULLRATE, "full") X(AIL1, "aiL1") X(AIL4, "aiL4") \
	X(AILP, "aiLP") X(VISCALLS, "visCalls") X(DRAWS, "draws") X(SHADOWDRAWS, "shadowDraws") \
	X(INSTDRAWS, "instDraws") X(INSTANCES, "instances") X(SETPASSES, "setPass") X(RSOS, "rso") \
	X(DRAWS_OUT, "drawsNoScene") X(DRAWS_NORSO, "drawsNoRso") X(SCENECALLS, "sceneCalls") \
	X(SYNCS, "syncs") X(OLDANIMS, "oldAnims") X(BINDS, "binds") \
	X(SETB, "setB") X(ZONESTATE, "zoneState") X(ZLOADED, "zLoaded") X(ZOUTB, "zOutB") \
	X(UNLOADS, "unloads") X(SQUADS, "squads") X(PLATOONS, "platoons") \
	X(FX, "fx") X(FXSYS, "fxSys") X(FXVIS, "fxVis") X(FXSTOP, "fxStop") X(FXPARTS, "fxParts") \
	X(FXCALLS, "fxCalls") X(FXNEW, "fxNew") X(FXDEL, "fxDel") \
	X(CURCALLS, "curCalls") X(CURSTILL, "curStill") X(CURSMALL, "curSmall") X(CURLARGE, "curLarge") \
	X(CURMOVED, "curMoved") X(CURBTN, "curBtn") X(CUREDGE, "curEdge") X(CURMOD, "curMod") \
	X(CURPHYS, "curPhys") X(CURHITS, "curHits") X(CURCHAR, "curChar") X(RAY2CALLS, "ray2Calls") \
	X(CURINPUT, "curInput")

#define AUDIT_ENUM_C(id, name) C_##id,
#define AUDIT_NAME(id, name) name,

enum Count  { AUDIT_COUNTS(AUDIT_ENUM_C) NUM_COUNTS };


// Counts that are events (summed per second) rather than state (averaged).
inline bool CountIsEvent(int c)
{
	return c == C_UNLOADS || c == C_SQUADS || c == C_FXNEW || c == C_FXDEL ||
	       (c >= C_CURCALLS && c <= C_RAY2CALLS);
}

// Bit masks (OR-ed per second, left out of the means).
inline bool CountIsBits(int c) { return c == C_CURINPUT; }

// curInput bits: the input state at each cursor ray, OR-ed over the frame.
enum CursorInputBit
{
	CI_MLEFT      = 1 << 0,    // InputHandler mLeft held
	CI_MRIGHT     = 1 << 1,    // mRight held
	CI_LASTMLEFT  = 1 << 2,    // InputHandler lastMLeft
	CI_LASTMRIGHT = 1 << 3,    // lastMRight
	CI_MLDOWN     = 1 << 4,    // mLDown (press edge)
	CI_MRDOWN     = 1 << 5,    // mRDown
	CI_MLUP       = 1 << 6,    // mLUp (release edge)
	CI_MRUP       = 1 << 7,    // mRUp
	CI_CTRL       = 1 << 8,
	CI_SHIFT      = 1 << 9,
	CI_ALT        = 1 << 10,
	CI_PREVMLEFT  = 1 << 11,   // 0x2132282: mLeft as PlayerInterface::update saw it last frame
	CI_PREVMRIGHT = 1 << 12,   // 0x2132281: mRight, last frame
	CI_MOVED      = 1 << 13,   // InputHandler mPos changed since the previous cursor ray
	CI_PHYS       = 1 << 14    // the physics thread was running when the ray started
};


// PhysicsActual::threadJunkPreBT fields, Steam 1.0.65. Each queue count is
// sampled at the outer call's entry, before the physics thread drains it.
const size_t PHYS_Q_MAKE          = 0x1C8;
const size_t PHYS_HULLS           = 0x1E0;
const size_t PHYS_Q_HULL_DESTROY  = 0x218;
const size_t PHYS_Q_GROUP         = 0x250;
const size_t PHYS_Q_IMPULSE       = 0x288;
const size_t PHYS_Q_ACTOR_DESTROY = 0x2C0;
const size_t PHYS_Q_TERRAIN       = 0x310;

enum FrameFlag
{
	F_INGAME     = 1 << 0,   // mainLoop ran
	F_PAUSED     = 1 << 1,
	F_TRANS      = 1 << 2,   // a showLoadingMessage bracket was open during the frame
	F_ZONEBUSY   = 1 << 3,   // zone state machine not idle
	F_PHYSRAN    = 1 << 4,   // mainLoop's physics block ran (physics thread was idle)
	F_AIJOIN     = 1 << 5,   // main thread had to wait for the AI thread
	F_BIRDSJOIN  = 1 << 6,
	F_AISYNC     = 1 << 7,   // AI body ran on the main thread (characterMultithreading off)
	F_AIMISS     = 1 << 8,   // AI data not collected for this frame
	F_SGNESTED   = 1 << 9,   // updateSceneGraph ran inside CompositorManager2::_update
	F_FG         = 1 << 10,  // game window in the foreground
	F_STALL      = 1 << 11,  // frame longer than StallMs
	F_BROKEN     = 1 << 12,  // missing stamps or unbalanced probes
	F_BOUNDARY_T = 1 << 13,  // framed on frameStarted, not renderOneFrame
	F_PROFLOG    = 1 << 14,  // the profiler wrote its own log lines this frame
	F_RCALLSFULL = 1 << 15,  // more scene calls than MAX_RCALLS: render sums are partial
	F_CURSORSPLIT = 1 << 16  // the audit cast its own shadow rays after the cursor ray
};

enum FrameClass { CLS_STEADY, CLS_STREAM, CLS_PAUSED, CLS_MENU, NUM_CLASSES };

enum RCallFlag
{
	RC_SHADOW   = 1 << 0,   // render stage was IRS_RENDER_TO_TEXTURE (shadow casters)
	RC_CULLONLY = 1 << 1,   // a _cullPhase01 no _renderPhase02 claimed this frame
	RC_NESTED   = 1 << 2    // ran inside another scene call
};

struct FrameRec
{
	unsigned seq;
	unsigned flags;
	double   t;               // seconds since plugin start, at frame start
	float    m[NUM_METRICS];  // NaN = not measured this frame
	int      c[NUM_COUNTS];
	int      ncalls;
	RCall    calls[MAX_RCALLS];
	unsigned short curGroups[CUR_GROUPS];   // cursor hits by collision group
	CursorSplit    split;                   // valid when flags & F_CURSORSPLIT
	PhysRunSample  phys;
	int            nphysq;
	PhysQuerySample physq[MAX_PHYS_QUERIES];
};

inline bool IsNan(float f) { return f != f; }


// =========================================================================
// Configuration ([Audit] in KEOProfiler.ini)
// =========================================================================

inline bool IsMain()
{
	return GetCurrentThreadId() == g_mainThreadId;
}
enum LogTarget { LOG_PROFILER, LOG_AUDIT };
struct QueuedLine
{
	int         target;
	std::string text;
};
const LONG64    RING_SIZE = 4096;              // power of two
const int NAME_LEN    = 64;
const int MAX_NAMED   = 64;     // cameras, viewports
const int MAX_CLASSES = 256;    // Renderable and AI task classes (by vtable)
const int MAX_MESHES  = 4096;
enum ClassKind { KIND_OTHER, KIND_SUBENTITY, KIND_BATCH };
struct NameEntry
{
	const void* key;
	int         kind;        // classes: ClassKind
	int         objOffset;   // classes: renderable pointer - objOffset = complete object
	char        name[NAME_LEN];
};
const int MAX_FX_TPL = 256;
struct TimerInfo
{
	bool   resOk;
	double resCoarseMs, resFineMs, resCurMs;   // NtQueryTimerResolution (system-wide)
	double sleep1AvgMs, sleep1MaxMs;           // what this process actually gets
	double qpcNs;
};
// Scene-call buckets (camera, viewport, render-queue range, shadow) over the
// window's running frames, steady and stream together.
struct Bucket
{
	unsigned char cam, vp, firstRq, lastRq, flags;
	int    calls;
	int    timedCalls;   // calls with rso/setPass times (TSC calibrated)
	int    bindN, d3dN, syncN;
	double ms, cullMs, submitMs, rsoMs, setPassMs, bindMs, d3dMs, syncMs;
	double draws, instDraws, instances, setPasses, rsos, noRso, timedRsos, attachRso;
};
// One CSV row per wall-clock second of frame starts.
struct SecondAcc
{
	long long          second;       // floor(t), -1 = empty
	int                frames;
	int                cls[NUM_CLASSES];
	int                excluded;
	std::vector<float> frameMs;      // running, non-excluded frames
	double             sum[NUM_METRICS];
	float              mx[NUM_METRICS];
	int                n[NUM_METRICS];
	double             csum[NUM_COUNTS];
	unsigned           cor[NUM_COUNTS];   // bit-mask counts, OR-ed
	int                cn;
};

// =========================================================================
// Current frame (main thread only)
// =========================================================================

// Call-site probe tags (g_sites below).
enum SiteTag
{
	// Main thread: the mainLoop sections in call order, plus the AI kick.
	ST_AIKICK, ST_AIJOIN, ST_PATH, ST_KILL, ST_CHARSUT, ST_SAVE, ST_RAGDOLL, ST_PHYSKICK,
	ST_ZONECAM, ST_PLAYER, ST_MISCA, ST_PARTICLES, ST_MISCB, ST_ZONEMT, ST_FACTORY,
	ST_BIRDSJOIN, ST_CHARS, ST_CHARSP, ST_FACTIONS, ST_BIRDSKICK, ST_GUI,
	// Main thread: nested inside the player and zoneCam sections, and the
	// DeferredLightingPass light renderer (inside the render passes).
	ST_MOUSESCAN, ST_MOUSERAY, ST_CAMRAY, ST_MOUSERAY2, ST_CURSORT,
	ST_ZC_CONTENT, ST_ZC_MISC, ST_ZC_ACT, ST_ZC_DEACT, ST_ZC_SECT, ST_ZC_MAINT,
	ST_LIGHTS,
	// AI thread (RenderTimeBackthread body, and threadedUpdate inside list 1).
	ST_AI_FIRST,
	ST_AIZONE = ST_AI_FIRST, ST_AICONTENT, ST_AIFACTIONS, ST_AIVIS1, ST_AIVIS2,
	ST_AITU, ST_AITU4, ST_AITUP, ST_AIENV, ST_AIFORCED, ST_AIFLUSH, ST_AIANIM,
	// Physics thread (PhysicsActual body).
	ST_PHYS_FIRST,
	ST_PHYSLOCK = ST_PHYS_FIRST, ST_PHYSPRE, ST_PHYSPOST,
	ST_PHYS_DETAIL_FIRST,
	ST_PHYS_GROUP = ST_PHYS_DETAIL_FIRST, ST_PHYS_ACTOR_DESTROY, ST_PHYS_TERRAIN,
	ST_PHYS_FLUSH, ST_PHYS_FETCH,
	ST_COUNT
};

typedef int    (*GetRenderStage_t)(void*);
typedef size_t (*GetWorkerThreads_t)(void*);
typedef const void* (*OgreGetter_t)(const void*);
typedef int         (*OgreIntGetter_t)(const void*);
typedef unsigned (*VisFlags_t)(const void*);
// Scene calls in flight (main thread). Kenshi runs its CSM cascades from a
// compositor pass, not inside another scene call, but nesting is handled.
struct CallCtx
{
	int                idx;        // g_cur.calls index, -1 when the frame's table is full
	LONGLONG           t0;         // _renderPhase02 entry
	LONGLONG           child;      // nested scene calls (render + cull), inclusive
	LONGLONG           rvoT0;      // _renderVisibleObjects entry, 0 when outside it
	LONGLONG           rvoTicks;   // _renderVisibleObjects, inclusive
	LONGLONG           rvoChild;   // nested scene time that fell inside it
	LONGLONG           syncTicks;  // Barrier::sync on the main thread
	unsigned long long rsoTsc, passTsc, bindTsc, d3dTsc;
};
const int MAX_DEPTH = 4;
// A _cullPhase01 waits here until the _renderPhase02 for the same camera and
// viewport claims it.
struct PendingCull
{
	const void*   cam;
	const void*   vp;
	float         ms;
	unsigned char camId, vpId;   // named at cull time, while both objects are known alive
	unsigned char firstRq, lastRq, shadow;
};
inline void Inc16(unsigned short& v)
{
	if (v != 0xFFFF)
		++v;
}
const int   MAX_PENDING = 8;
inline unsigned HashPtr(const void* p, unsigned mask)
{
	unsigned long long v = (unsigned long long)(uintptr_t)p;
	v ^= v >> 29;
	v *= 0x9E3779B97F4A7C15ULL;
	return (unsigned)(v >> 32) & mask;
}
const int MAX_PROBES_PER_LOOKUP = 64;
extern NameEntry     g_camE[MAX_NAMED], g_vpE[MAX_NAMED], g_clsE[MAX_CLASSES], g_meshE[MAX_MESHES];
extern volatile LONG g_camN, g_vpN, g_clsN, g_meshN;
int NamedId(NameEntry* table, volatile LONG* count, const void* key, bool camera);
inline int CameraId(const void* cam)  { return NamedId(g_camE, &g_camN, cam, true); }
inline int ViewportId(const void* vp) { return NamedId(g_vpE, &g_vpN, vp, false); }
typedef void*  (*RootSingleton_t)();
typedef size_t (*PuParticles_t)(void*);
typedef void (*PuUpdate_t)(void*, float);
typedef bool (*RenderOneFrame_t)(void*);
typedef bool (*FrameListener_t)(void*, const void*);
typedef void (*VoidThis_t)(void*);
typedef void (*RsRender_t)(void*, const void*);
typedef void (*ThreadBody_t)(void*, float, bool);
typedef char (*ZoneLifecycle_t)(void*);
typedef void (*RenderPhase02_t)(void*, void*, const void*, void*, unsigned char, unsigned char, bool);
typedef void (*CullPhase01_t)(void*, void*, const void*, void*, unsigned char, unsigned char);
typedef void (*Rso_t)(void*, void*, const void*, bool, bool);
typedef const void* (*SetPass_t)(void*, const void*, bool, bool);
typedef void (*BarrierSync_t)(void*);
typedef void (*D3DBind_t)(void*, int, void*, unsigned __int64);
typedef unsigned char (*PhysMake_t)(void*);
typedef __int64       (*PhysFinish_t)(void*, unsigned char);
typedef __int64       (*PhysApplyHull_t)(void*);
typedef void          (*PhysApplyDoor_t)(void*);
typedef unsigned __int64 (*BodyUpdate_t)(void*, float);
typedef void             (*MoveUpdate_t)(void*, float);
typedef int  (*SmSaveGame_t)(void*, const void*, const void*);
typedef void (*SmUpdateAuto_t)(void*);
typedef void (*ZmSaveStates_t)(void*);
typedef void (*ZmcSaveLevel_t)(void*, bool, const void*, const void*);
typedef void (*RocSerialise_t)(void*, const void*, void*, void*, void*, const void*);
typedef void (*ZmcSaveItems_t)(void*, void*, bool);
typedef void (*ZmcSaveDisk_t)(void*, const void*);
typedef bool (*GdcSave_t)(void*, const void*, void*);
typedef void (*TlSave_t)(void*, void*);
typedef void (*FmSave_t)(void*, void*);
typedef void (*PmSaveTex_t)(void*);
typedef char (*SfsSaveGame_t)(void*, const void*);
typedef void (*SfsSync_t)(void*);
typedef void* (*IsIndoors_t)(const void*);
extern const char* OGRE_DLL;
extern const char* SYM_RENDER_ONE_FRAME;
extern const char* SYM_UPDATE_SCENE_GRAPH;
extern const char* SYM_CM2_UPDATE;
extern const char* SYM_CM2_SWAP;
extern const char* SYM_RS_RENDER;
extern const char* SYM_GET_RENDER_STAGE;
extern const char* SYM_GET_WORKER_THREADS;
extern const char* SYM_RENDER_PHASE02;
extern const char* SYM_CULL_PHASE01;
extern const char* SYM_RENDER_VISIBLE;
extern const char* SYM_RSO;
extern const char* SYM_SET_PASS;
extern const char* SYM_MO_GET_NAME;
extern const char* SYM_SUBENT_PARENT;
extern const char* SYM_ENT_GET_MESH;
extern const char* SYM_BATCH_MESH_REF;
extern const char* SYM_RES_GET_NAME;
extern const char* SYM_VP_WIDTH;
extern const char* SYM_VP_HEIGHT;
extern const char* SYM_VP_TARGET;
extern const char* SYM_RT_GET_NAME;
extern const char* SYM_BARRIER_SYNC;
extern const char* SYM_OLD_ANIMS;
extern const char* SYM_GET_VIS_FLAGS;
extern const char*  D3D11_DLL;
extern const char* PU_DLL;
extern const char* SYM_PU_UPDATE;
extern const char* SYM_PU_PARTICLES;
extern const char* SYM_PU_TEMPLATE;
extern const char* SYM_ROOT_SINGLETON;
extern const char* SYM_ROOT_NEXT_FRAME;
extern const char* METRIC_NAMES[NUM_METRICS];
extern const char* COUNT_NAMES[NUM_COUNTS];
extern const char* CURCLASS_NAMES[NUM_CURCLASSES];
extern const char* SPLITRAY_NAMES[NUM_SPLITRAYS];
extern const char* CLASS_NAMES[NUM_CLASSES];
int ClassOf(const FrameRec& r);
bool Excluded(const FrameRec& r);
extern uintptr_t   g_base;
extern uintptr_t   g_exeEnd;
extern std::string g_dllDir;
extern std::string g_runName;
void LoadConfig();
bool NameDisabled(const char* name);
extern CRITICAL_SECTION        g_lineCS;
extern bool                    g_lineCSReady;
extern std::vector<QueuedLine> g_lines;
extern HANDLE                  g_reporter;
extern volatile LONG           g_reporterRunning;
extern FrameRec*       g_ring;
extern volatile LONG   g_ringLost;
void QueueLine(int target, const std::string& text);
void RingPush(const FrameRec& r);
extern volatile LONG   g_clsDraws[2][MAX_CLASSES];
extern volatile LONG   g_clsInst[2][MAX_CLASSES];
extern volatile LONG64 g_clsTsc[2][MAX_CLASSES];
extern volatile LONG   g_meshDraws[2][MAX_MESHES];
extern volatile LONG   g_meshBatch[2][MAX_MESHES];
extern volatile LONG   g_meshInst[2][MAX_MESHES];
extern volatile LONG   g_drawFrames;
extern volatile double g_tscPerMs;
extern volatile LONG64 g_taskTicks[MAX_CLASSES];
extern volatile LONG   g_taskCalls[MAX_CLASSES];
extern volatile LONG64 g_taskNoneTicks;
extern volatile LONG   g_taskNoneCalls;
extern volatile LONG   g_aiFrames;
extern NameEntry       g_fxE[MAX_FX_TPL];
extern volatile LONG   g_fxN;
extern volatile LONG64 g_fxTsc[2][MAX_FX_TPL];
extern volatile LONG   g_fxCalls[2][MAX_FX_TPL];
extern volatile LONG64 g_fxAlive[MAX_FX_TPL];
extern volatile LONG64 g_fxVisSum[MAX_FX_TPL];
extern volatile LONG64 g_fxStopSum[MAX_FX_TPL];
extern volatile LONG64 g_fxPartSum[MAX_FX_TPL];
extern volatile LONG   g_fxNew[MAX_FX_TPL];
extern volatile LONG   g_fxDel[MAX_FX_TPL];
extern volatile LONG   g_fxFrames;
extern volatile LONG64 g_fxEffSum;
extern volatile LONG64 g_fxNvtoSum;
extern volatile LONG64 g_fxMainTsc;
extern volatile LONG   g_fxMainCalls;
extern volatile double g_fxRunMs;
extern volatile LONG   g_fxOverflow;
void InitNameTables();
extern FILE* g_profLog;
extern FILE* g_auditLog;
extern FILE* g_secCsv;
extern FILE* g_frameCsv;
extern FILE* g_physCsv;
extern FILE* g_physqCsv;
extern TimerInfo g_timer;
TimerInfo MeasureTimer();
void WriteRaw(FILE* f, const std::string& s);
std::string V(float v, int decimals);
void AuditOut(const std::string& text);
void WriteTimerLine();
float Percentile(std::vector<float>& v, double p);
extern std::vector<FrameRec> g_window;
extern double g_windowStart;
extern int    g_slowThisWindow;
extern int    g_slowSuppressed;
extern std::vector<Bucket> g_buckets;
void AccumulateBuckets(const FrameRec& r);
void WriteRenderBuckets();
void WriteDrawTables();
void WriteTaskTable();
void WriteFxTable();
void WriteSummary();
extern SecondAcc g_sec;
void ResetSecond(long long second);
void OnRecord(const FrameRec& r);
unsigned __stdcall ReporterProc(void*);
extern unsigned      g_nextSeq;
extern bool          g_installed;
extern bool          g_boundaryR;
extern int           g_rofDepth;
extern volatile LONG g_transOpen;
extern volatile LONG g_transSeen;
extern volatile LONG g_squadsPending;
extern bool          g_foreground;
extern LONGLONG      g_last1Hz;
extern int           g_zoneRow;
extern int           g_rowLoaded[ZONE_GRID];
extern int           g_zLoaded;
extern int           g_platoons;
extern float         g_camAlt;
extern bool          g_headerDone;
extern std::string   g_lastSettings;
extern void*         g_sceneMgr;
extern GetRenderStage_t   g_getStage;
extern GetWorkerThreads_t g_getWorkers;
extern OgreGetter_t    g_moGetName;
extern OgreGetter_t    g_subParent;
extern OgreGetter_t    g_entMesh;
extern OgreGetter_t    g_batchMesh;
extern OgreGetter_t    g_resGetName;
extern OgreGetter_t    g_vpTarget;
extern OgreGetter_t    g_rtGetName;
extern OgreIntGetter_t g_vpWidth;
extern OgreIntGetter_t g_vpHeight;
extern bool g_renderOn;
extern bool g_cmHooked;
extern bool g_haveTag[ST_COUNT];
extern bool g_bodyHooked;
extern bool g_moveHooked;
extern bool g_physBodyHooked;
extern bool g_physMakeHooks;
extern bool g_physApplyHooks;
extern const char* g_hullStatus;
extern bool g_syncHooked;
extern bool g_oldAnimHooked;
extern bool g_bindHooked;
extern bool g_d3dHooked;
extern VisFlags_t g_getVisFlags;
extern CallCtx   g_stack[MAX_DEPTH];
extern int       g_depth;
extern PendingCull g_pending[MAX_PENDING];
extern int         g_npending;
extern int  g_rsoDepth;
extern int  g_rsoCls;
extern int  g_rsoMesh;
extern bool g_rsoBatch;
void CopyOgreString(const void* s, char* out, size_t cap);
int ClassIdOf(const void* vtable);
int MeshIdOf(const void* mesh);
void* MainSceneManager();
extern RootSingleton_t     g_rootSingleton;
extern PuParticles_t       g_puParticles;
extern const char* volatile g_root;
extern bool g_fxHooked;
extern bool g_fxLayoutOk;
extern bool g_fxCensusOn;
void FxCensus();
extern PuUpdate_t oPuUpdate;
void hk_PuUpdate(void* sys, float dt);
void FxFrameTotals(FrameRec& r);
void FrameBoundary(LONGLONG t, bool viaRenderOneFrame);
void CollectAi();
void CollectBirds();
void CollectPhys();
void SampleWorld();
extern RenderPhase02_t oRenderPhase02;
extern CullPhase01_t   oCullPhase01;
extern VoidThis_t      oRenderVisible;
extern Rso_t           oRso;
extern SetPass_t       oSetPass;
extern RenderOneFrame_t oRenderOneFrame;
extern FrameListener_t  oFrameStarted;
extern FrameListener_t  oFrameQueued;
extern FrameListener_t  oFrameEnded;
extern VoidThis_t       oUpdateScene;
extern VoidThis_t       oCm2Update;
extern VoidThis_t       oCm2Swap;
extern RsRender_t       oRsRender;
extern ThreadBody_t     oAiBody;
extern ThreadBody_t     oPhysBody;
extern ThreadBody_t     oBirdsBody;
extern VoidThis_t       oPhysUT;
extern ZoneLifecycle_t  oZoneLifecycle;
bool hk_RenderOneFrame(void* root);
bool hk_FrameStarted(void* self, const void* evt);
bool hk_FrameQueued(void* self, const void* evt);
bool hk_FrameEnded(void* self, const void* evt);
void hk_UpdateScene(void* sm);
void hk_Cm2Update(void* cm);
void hk_Cm2Swap(void* cm);
void hk_RsRender(void* rs, const void* op);
void hk_RenderPhase02(void* sm, void* cam, const void* lod, void* vp, unsigned char firstRq, unsigned char lastRq, bool overlays);
void hk_CullPhase01(void* sm, void* cam, const void* lod, void* vp, unsigned char firstRq, unsigned char lastRq);
void hk_RenderVisible(void* sm);
void hk_Rso(void* sm, void* rend, const void* pass, bool scissor, bool lights);
extern BarrierSync_t oBarrierSync;
extern VoidThis_t    oOldAnims;
extern D3DBind_t     oD3DBind;
extern RsRender_t    oD3DRender;
void hk_BarrierSync(void* barrier);
void hk_OldAnims(void* sm);
void hk_D3DBind(void* rs, int type, void* params, unsigned __int64 mask);
void hk_D3DRender(void* rs, const void* op);
const void* hk_SetPass(void* sm, const void* pass, bool evenIfSuppressed, bool shadowDerivation);
void PhysDetailBegin(LONG seq, LONGLONG at);
void PhysDetailEnd(LONGLONG at);
void PhysProbeEnter(int tag, uintptr_t self);
void PhysProbeExit(int tag, LONGLONG at);
extern PhysMake_t      oPhysMakeHull;
extern PhysMake_t      oPhysMakeFile;
extern PhysMake_t      oPhysMakeScythe;
extern PhysFinish_t    oPhysFinishScythe;
extern PhysApplyHull_t oPhysApplyHull;
extern PhysApplyDoor_t oPhysApplyDoor;
unsigned char hk_PhysMakeHull(void* self);
unsigned char hk_PhysMakeFile(void* self);
unsigned char hk_PhysMakeScythe(void* self);
__int64 hk_PhysFinishScythe(void* self, unsigned char inserted);
__int64 hk_PhysApplyHull(void* self);
void hk_PhysApplyDoor(void* self);
void hk_AiBody(void* self, float ft, bool inf);
void hk_PhysBody(void* self, float ft, bool inf);
void hk_BirdsBody(void* self, float ft, bool inf);
extern BodyUpdate_t oBodyUpdate;
extern MoveUpdate_t oMoveUpdate;
unsigned __int64 hk_BodyUpdate(void* body, float dt);
void hk_MoveUpdate(void* movement, float dt);
void hk_PhysUT(void* self);
char hk_ZoneLifecycle(void* zone);
extern bool      g_saveHooked;
extern SmSaveGame_t   oSmSaveGame;
extern SmUpdateAuto_t oSmUpdateAuto;
extern ZmSaveStates_t oZmSaveStates;
extern ZmcSaveLevel_t oZmcSaveLevel;
extern RocSerialise_t oRocSerialise;
extern ZmcSaveItems_t oZmcSaveItems;
extern ZmcSaveDisk_t  oZmcSaveDisk;
extern GdcSave_t      oGdcSave;
extern TlSave_t       oTlSaveUnique;
extern TlSave_t       oTlSaveState;
extern FmSave_t       oFmSavePlayer;
extern FmSave_t       oFmSaveState;
extern PmSaveTex_t    oPmSaveTex;
extern SfsSaveGame_t  oSfsSaveGame;
extern SfsSync_t      oSfsSync;
int hk_SmSaveGame(void* sm, const void* location, const void* name);
void hk_SmUpdateAuto(void* sm);
void hk_ZmSaveStates(void* zm);
void hk_ZmcSaveLevel(void* zmc, bool all, const void* modName, const void* pathOverride);
void hk_RocSerialise(void* roc, const void* things, void* out, void* source, void* offset, const void* mod);
void hk_ZmcSaveItems(void* zmc, void* datas, bool all);
void hk_ZmcSaveDisk(void* zmc, const void* path);
bool hk_GdcSave(void* gdc, const void* filename, void* moreData);
void hk_TlSaveUnique(void* tl, void* datas);
void hk_TlSaveState(void* tl, void* datas);
void hk_FmSavePlayer(void* fm, void* datas);
void hk_FmSaveState(void* fm, void* datas);
void hk_PmSaveTex(void* pm);
char hk_SfsSaveGame(void* sfs, const void* savePath);
void hk_SfsSync(void* sfs);
extern IsIndoors_t oIsIndoors;
void* hk_IsIndoors(const void* point);
extern CallSiteProbe::Site g_sites[];
extern const int NUM_SITES;
void OnProbeEnter(int id, CallSiteProbe::U64 a, CallSiteProbe::U64 b, CallSiteProbe::U64 c, CallSiteProbe::U64 d);
void OnProbeExit(int id, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1);
extern int g_hooksOk, g_hooksTotal;
void InstallOgreHooks(HMODULE ogre, bool* haveRenderOneFrame);
void InstallParticleHooks(HMODULE ogre);
bool InstallExeHooks();
int InstallSites(int* total);
void InstallCursor(bool steam);
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace audit {
extern ThreadSlot g_ai, g_birds, g_phys;
extern LONGLONG g_physPhaseStart;
} // audit

#endif
