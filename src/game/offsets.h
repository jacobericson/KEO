// offsets.h - Game structure field offsets and layout assertions.
// Included through game.h.

#ifndef KEO_OFFSETS_H
#define KEO_OFFSETS_H

#include "game/klib_members.h"
#include "base/core.h"

// =========================================================================
// Game structure offsets (from decompilation + KenshiLib headers)
// =========================================================================

// RootObjectBase / Character
const size_t OFF_CHAR_POS_X      = 0x48;
KLIB_ASSERT_OFFSET(RootObjectBase_pos_x, OFF_CHAR_POS_X);
const size_t OFF_CHAR_POS_Y      = 0x4C;
KLIB_ASSERT_OFFSET(RootObjectBase_pos_y, OFF_CHAR_POS_Y);
const size_t OFF_CHAR_POS_Z      = 0x50;
KLIB_ASSERT_OFFSET(RootObjectBase_pos_z, OFF_CHAR_POS_Z);
const size_t OFF_CHAR_MOVEMENT   = 0x640;
KLIB_ASSERT_OFFSET(Character_movement, OFF_CHAR_MOVEMENT);
const size_t OFF_CHAR_IN_SOMETHING = 0x2F8; // Character::inSomething (UseStuffState; IN_NOTHING = 0)
KLIB_ASSERT_OFFSET(Character_inSomething, OFF_CHAR_IN_SOMETHING);
const size_t OFF_CHAR_STATS      = 0x450;  // Character::stats (CharStats*)
KLIB_ASSERT_OFFSET(Character_stats, OFF_CHAR_STATS);
const size_t OFF_STATS_HOLD      = 0x12B;  // CharStats::_holdPositionMode (bool): what
                                            // Character::getStandingOrder(M_SET_ORDER_HOLD) 0x5C8C30 returns
KLIB_ASSERT_OFFSET(CharStats__holdPositionMode, OFF_STATS_HOLD);
const size_t OFF_CHAR_RACE       = 0x2E0;  // Character::myRace (RaceData*)
KLIB_ASSERT_OFFSET(Character_myRace, OFF_CHAR_RACE);
const size_t OFF_STATS_MOVE_SPEED = 0x17C; // CharStats::moveSpeed: the land speed with every body factor
KLIB_ASSERT_OFFSET(CharStats_moveSpeed, OFF_STATS_MOVE_SPEED);

// RaceData
const size_t OFF_RACE_WALK_SPEED = 0x64;   // RaceData::walkSpeed (float)
KLIB_ASSERT_OFFSET(RaceData_walkSpeed, OFF_RACE_WALK_SPEED);
const size_t OFF_RACE_SWIMS      = 0x79;   // RaceData::swims (bool)
KLIB_ASSERT_OFFSET(RaceData_swims, OFF_RACE_SWIMS);

// CharMovement
const size_t OFF_CMOV_SPEED_MODE = 0x20;   // MoveSpeed enum: 0=WALK,1=JOG,2=RUN,3=GROUPED,4=NO_CHANGE
KLIB_ASSERT_OFFSET(AbstractMovementBase_speedOrders, OFF_CMOV_SPEED_MODE);
const size_t OFF_CMOV_HAVOK_CHAR = 0x320;
KLIB_ASSERT_OFFSET(CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR);
const int    MOVESPEED_GROUPED   = 3;      // "Running Together" mode
// Island routing reads (from setDestination 0x6607E0 / computeProjectedDest 0x3A39C0)
const size_t OFF_CMOV_POS            = 0xC4;   // Vector3 position used for zone lookup
KLIB_ASSERT_OFFSET(AbstractMovementBase_pos, OFF_CMOV_POS);
const size_t OFF_CMOV_LAST_DEST      = 0xDC;   // last requested destination (edge-mode compare, 2 units)
KLIB_ASSERT_OFFSET(AbstractMovementBase_destination, OFF_CMOV_LAST_DEST);
const size_t OFF_CMOV_PATH_DEST      = 0xE8;   // pathDestination handed to HavokCharacter::requestPath
KLIB_ASSERT_OFFSET(AbstractMovementBase_pathDestination, OFF_CMOV_PATH_DEST);
const size_t OFF_CMOV_WALK_SPEED     = 0xC0;   // float: the WALK order's desired speed
KLIB_ASSERT_OFFSET(AbstractMovementBase_walkSpeed, OFF_CMOV_WALK_SPEED);
const size_t OFF_HC_WATER_MODIFIER   = 0x74;   // HavokCharacter::waterModifier: the engine's water cost multiplier
KLIB_ASSERT_OFFSET(HavokCharacter_waterModifier, OFF_HC_WATER_MODIFIER);
const size_t OFF_CMOV_EDGE_COUNTER   = 0x368;  // edge-target retry counter (0..17)
KLIB_ASSERT_OFFSET(CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER);
const size_t OFF_CMOV_MOVING_TO_EDGE = 0x370;  // BYTE: 1 while routing to an island edge
KLIB_ASSERT_OFFSET(CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE);
const size_t OFF_CMOV_MOVEMENT_MODE = 0x378;  // int MovementMode: 0 path following, 1 combat, 2 direct
KLIB_ASSERT_OFFSET(CharMovement_movementMode, OFF_CMOV_MOVEMENT_MODE);
const size_t OFF_CMOV_ANIMATION_OVERRIDE = 0x37C;  // bool: an animation drives the character, not its path
KLIB_ASSERT_OFFSET(CharMovement_animationOverride, OFF_CMOV_ANIMATION_OVERRIDE);
const size_t OFF_CMOV_ROAD_FOLLOWER = 0xF8;  // RoadFollower*: set while the character follows a road
KLIB_ASSERT_OFFSET(AbstractMovementBase_roadFollower, OFF_CMOV_ROAD_FOLLOWER);
const size_t OFF_ROAD_FOLLOWER_STATE = 0x18;  // int: RoadPathBuilder::isValid 0x45A240 reads it valid unless 0, 3 or 4
const size_t OFF_HC_ACCELERATION  = 0x78;  // float: the Havok character's acceleration, tenths of a unit
KLIB_ASSERT_OFFSET(HavokCharacter_acceleration, OFF_HC_ACCELERATION);
const size_t OFF_HC_DESIRED_SPEED = 0x7C;  // float: its desired speed, tenths of a game unit per second
KLIB_ASSERT_OFFSET(HavokCharacter_desiredSpeed, OFF_HC_DESIRED_SPEED);

// ZoneMap entry (360 bytes each in ZoneManager zone array)
const int    ZONE_ENTRY_SIZE     = 360;
static_assert(ZONE_ENTRY_SIZE == KLIB_ZONE_STRIDE, "ZONE_ENTRY_SIZE composed parity");
static_assert(ZONE_ENTRY_SIZE == 360, "ZoneMap legacy stride");
const size_t OFF_ZONE_CONTENT    = 0;
KLIB_ASSERT_OFFSET(ZoneMap_mapContent, OFF_ZONE_CONTENT);
const size_t OFF_ZONE_COORDS_X   = 24;
KLIB_ASSERT_OFFSET(ZoneMap_coordinates_x, OFF_ZONE_COORDS_X);
const size_t OFF_ZONE_COORDS_Y   = 28;
KLIB_ASSERT_OFFSET(ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y);
const size_t OFF_ZONE_IS_LOADING = 176;
KLIB_ASSERT_OFFSET(ZoneMap_stateT_mainThreadData__zoneBeingLoaded, OFF_ZONE_IS_LOADING);
const size_t OFF_ZONE_IS_ACCESS  = 177;
KLIB_ASSERT_OFFSET(ZoneMap_stateT_mainThreadData__zoneIsLoaded, OFF_ZONE_IS_ACCESS);
// float[3], indexed by activation type. ZoneMap::_activate writes the slot
// for its type before it discovers the cell is already held, and only a cell
// in the active set ever decrements one.
const size_t OFF_ZONE_COUNTDOWNS = 192;
KLIB_ASSERT_OFFSET(ZoneMap_activatedCountdown, OFF_ZONE_COUNTDOWNS);
const size_t OFF_ZONE_CENTER_X   = 248;
KLIB_ASSERT_OFFSET(ZoneMap_center_x, OFF_ZONE_CENTER_X);
const size_t OFF_ZONE_CENTER_Z   = 256;
KLIB_ASSERT_OFFSET(ZoneMap_center_z, OFF_ZONE_CENTER_Z);
const size_t OFF_ZMC_THINGS_COUNT = 88;
KLIB_ASSERT_OFFSET(RootObjectContainer_things_count, OFF_ZMC_THINGS_COUNT);
// Island routing (ZoneMap)
const size_t OFF_ZONE_ISLAND      = 0x20;   // int label written by _calculateIslands (0 = unlabelled)
KLIB_ASSERT_OFFSET(ZoneMap_island, OFF_ZONE_ISLAND);
const size_t OFF_ZONE_AABB_CENTER = 0xD0;   // Ogre::Aabb mCenter (3 floats)
KLIB_ASSERT_OFFSET(ZoneMap_bounds_mCenter, OFF_ZONE_AABB_CENTER);
const size_t OFF_ZONE_AABB_HALF   = 0xDC;   // Ogre::Aabb mHalfSize (3 floats); min = center - half
KLIB_ASSERT_OFFSET(ZoneMap_bounds_mHalfSize, OFF_ZONE_AABB_HALF);
const size_t OFF_ZONE_NEIGHBORS   = 0x128;  // ZoneMap* neighbors[4] (static grid adjacency, NULL at edges)
KLIB_ASSERT_OFFSET(ZoneMap_neighbors, OFF_ZONE_NEIGHBORS);
const int    ZONE_NEIGHBOR_COUNT  = 4;

// ZoneManager
const size_t OFF_ZM_ZONE_BASE    = 200;
KLIB_ASSERT_OFFSET(ZoneManager_worldMap, OFF_ZM_ZONE_BASE);
const size_t OFF_ZM_LOADING      = 8;        // BYTE: SaveManager::loadGame sets 1; cleared with state 5
KLIB_ASSERT_OFFSET(ZoneManager_justLoadedAGame, OFF_ZM_LOADING);
const size_t OFF_ZM_CURRENT_ZONE = 1475024;
KLIB_ASSERT_OFFSET(ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE);
const size_t OFF_ZM_STATE        = 1475032;
KLIB_ASSERT_OFFSET(ZoneManager_loadingPhase, OFF_ZM_STATE);
const int    ZONE_GRID_MAX       = 63;
static_assert(ZONE_GRID_MAX == KLIB_ZONE_DIMENSION - 1, "ZONE_GRID_MAX composed parity");
const int    ZONE_GRID_COUNT     = 64 * 64;
static_assert(ZONE_GRID_COUNT == KLIB_ZONE_DIMENSION * KLIB_ZONE_DIMENSION, "ZONE_GRID_COUNT composed parity");

// Set B (boost::unordered_set<ZoneMap*>) at ZoneManager+1474824.
// List head = *(buckets + 8*bucketCount); node: next at +0, value at +16.
const size_t OFF_ZM_SET_B          = 1474824;
KLIB_ASSERT_OFFSET(ZoneManager_activeZones, OFF_ZM_SET_B);
// Set A, same boost::unordered_set<ZoneMap*> layout, at ZoneManager+1474760
// (KenshiLib ZoneManager::processingNewActiveZones).
const size_t OFF_ZM_SET_A          = 1474760;
KLIB_ASSERT_OFFSET(ZoneManager_processingNewActiveZones, OFF_ZM_SET_A);
const size_t OFF_SET_BUCKET_COUNT  = 24;
KLIB_ASSERT_OFFSET(ZoneSetTable_bucket_count_, OFF_SET_BUCKET_COUNT);
const size_t OFF_SET_SIZE          = 32;
KLIB_ASSERT_OFFSET(ZoneSetTable_size_, OFF_SET_SIZE);
const size_t OFF_SET_BUCKETS       = 56;
KLIB_ASSERT_OFFSET(ZoneSetTable_buckets_, OFF_SET_BUCKETS);
const size_t OFF_SET_NODE_VALUE    = 16;
KLIB_ASSERT_OFFSET(ZoneSetNode_value_base_, OFF_SET_NODE_VALUE);

// SectionManager (pauseState.navmesh): zone extent used by computeProjectedDest
const size_t OFF_NAVMESH_ZONE_SIZE = 468;
KLIB_ASSERT_OFFSET(NavMesh_cellSize, OFF_NAVMESH_ZONE_SIZE);

// lektor<T*> (game dynamic array): +8 count, +12 capacity, +16 data
const size_t OFF_LEKTOR_COUNT    = 8;
KLIB_ASSERT_OFFSET(HandLektor_count, OFF_LEKTOR_COUNT);
const size_t OFF_LEKTOR_CAPACITY = 12;
KLIB_ASSERT_OFFSET(HandLektor_maxSize, OFF_LEKTOR_CAPACITY);
const size_t OFF_LEKTOR_DATA     = 16;
KLIB_ASSERT_OFFSET(HandLektor_stuff, OFF_LEKTOR_DATA);

// PlayerInterface (playerCharacters lektor at +0x2B0)
const size_t OFF_PI_CHAR_COUNT   = 0x2B8;
KLIB_ASSERT_OFFSET(PlayerInterface_playerCharacters_count, OFF_PI_CHAR_COUNT);
const size_t OFF_PI_CHAR_STUFF   = 0x2C0;
KLIB_ASSERT_OFFSET(PlayerInterface_playerCharacters_stuff, OFF_PI_CHAR_STUFF);

// Selected characters linked list
const size_t OFF_PI_SEL_INDEX    = 544;
static_assert(OFF_PI_SEL_INDEX == KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_bucket_count_, "OFF_PI_SEL_INDEX composed legacy offset drift");
const size_t OFF_PI_SEL_COUNT    = 552;
static_assert(OFF_PI_SEL_COUNT == KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_size_, "OFF_PI_SEL_COUNT composed legacy offset drift");
const size_t OFF_PI_SEL_ARRAY    = 576;
static_assert(OFF_PI_SEL_ARRAY == KLIB_OFF_PlayerInterface_selectedCharacters + KLIB_OFF_HandSetTable_buckets_, "OFF_PI_SEL_ARRAY composed legacy offset drift");
const size_t OFF_SEL_NODE_HANDLE = 16;
static_assert(OFF_SEL_NODE_HANDLE == KLIB_OFF_HandSetNode_value_base_, "OFF_SEL_NODE_HANDLE composed legacy offset drift");
const size_t OFF_SEL_NODE_TYPE   = 24;
static_assert(OFF_SEL_NODE_TYPE == KLIB_OFF_HandSetNode_value_base_ + KLIB_OFF_hand_type, "OFF_SEL_NODE_TYPE composed legacy offset drift");

// NavMeshGenerator (via section manager)
const size_t OFF_MGR_NAVMESH_GEN = 656;
KLIB_ASSERT_OFFSET(NavMesh_generator, OFF_MGR_NAVMESH_GEN);
const size_t OFF_NAVMESH_THREAD  = 8;
KLIB_ASSERT_OFFSET(ThreadClass_threadHandle, OFF_NAVMESH_THREAD);

// hkaiStreamingCollection
const size_t OFF_SC_INSTANCES_COUNT = 40;

// PathRequest layout: dispatcher passes &requestObj[+128] as resultBuf to csFindPath chain.
// Verified from SectionManager::contentStream (0x3AE350) decompilation + PathRequest__ctor.
// The game passes req+128 as resultBuf to hook_csFindPath, so playerByReq
// is recovered from it.
const size_t OFF_REQ_RESULTBUF_SLOT = 128;

// The request's water cost multiplier: requestPath copies the character's value here before it
// submits the request, and the serve passes it to the search.
const size_t OFF_REQ_WATER_COST_MULT = 0x34;

// A biome record (the engine's AreaBiomeGroup; KenshiLib declares no layout): its acidic-water rate, and
// its weather region, without which the engine applies no acid.
const size_t OFF_BIOME_ACID_WATER     = 144;
const size_t OFF_BIOME_WEATHER_REGION = 152;


// Extraction race mitigation: SEH wrap around Havok::contentStreamCallee_0x8869
// (the path-result-extraction loop). 526 bytes at impl. Called from findPath +
// findPathFallback after A* success; walks m_visitedEdges and fills Kenshi
// result buffer. Derefs m_instances[sec].m_instancePtr without NULL check,
// races with addInstance -> AV when section slot is mid-mutation.
const size_t RVA_CONTENT_STREAM_CALLEE_0X8869 = 0x3A9F20;

// hkaiStreamingCollection::addInstance: every streaming-collection insertion,
// counted (sectionStamp) and fed to the navmesh lifecycle and stitch-source
// rows (hook_addInstance). 821 bytes at impl.
const size_t RVA_ADD_INSTANCE = 0xD0D8F0;

// Render levers (src/render/)
const size_t RVA_WATER_OCCLUSION_LISTENER = 0x9D9060;  // WaterOcclusionListener::frameRenderingQueued
const size_t RVA_EFFECTS_MGR              = 0x2127190; // EffectsManager* (lazy singleton)
const size_t RVA_CSM_SETUP_CASCADE        = 0x862F60;  // CsmShadowMap::setupCascade (one cascade's box and shared params)
const size_t RVA_SKY_INSTANCE             = 0x212F3C0; // sky/weather object*; +0xC is the interpolated direction toward the sun
const size_t RVA_RENDERER                 = 0x21322B8; // Renderer* holder; +0x60 is the main Ogre::SceneManager*
const size_t RVA_FOG_CONTROLLER           = 0x2127348; // FogController* (fog volumes and their fades)
const size_t RVA_FOG_CONTROLLER_UPDATE    = 0x1096E0;  // FogController update(controller, camera): fades advance one frame dt per call
const size_t RVA_QUEUE_CUTTER_FOG_CALL    = 0x82C643;  // QueueListenerCutter::preRenderQueues: its one FogController update call
const size_t RVA_FOG_UPDATE_THUNK         = 0x10C67;   // jmp FogController update, the call's target
const size_t RVA_PAGED_GEOMETRY_UPDATE    = 0xA29E40;  // Forests::PagedGeometry::update (only caller FoliageSystem::update)

// Settings panel (src/gui/), both main thread
const size_t RVA_OPTIONS_CREATE       = 0x3F0120;  // OptionsWindow::create (builds the tabs; only caller show())
const size_t RVA_OPTIONS_SAVE_OPTIONS = 0x3EC950;  // OptionsWindow::saveOptions (only caller hide(), once per close)
const size_t RVA_DP_SET_LINE_TEXT_BUTTON = 0x6FE390; // DatapanelGUI::setLineTextButton
const size_t RVA_DP_BUTTON_PRESS      = 0x6F60A0;  // DataPanelLine_Button::pressCallback (invokes the line's callback)

// Benchmark facade (src/bench/), called or read on the main thread only.
// KenshiLib-covered functions: bench_game.cpp compares each with GetRealAddress.
const size_t RVA_CAMERA_TELEPORT        = 0x6B00F0;  // CameraClass::teleport
const size_t RVA_CAMERA_ORIENT_ZOOM     = 0x6AF0C0;  // CameraClass::manuallySetOrientationAndZoom
const size_t RVA_CAMERA_GET_CENTER      = 0x100880;  // CameraClass::getCenter
// PlayerInterface::camera, asserted via klib_layout.cpp's
// P(PlayerInterface,camera,0x30).
const size_t OFF_PI_CAMERA              = 0x30;
// CameraClass fields read for camera_focus.cpp, each asserted against
// KenshiLib's real offsets in klib_layout.cpp. `center` is the orbit/follow
// anchor node: CameraClass::update (0x6B1540) sets its position to the
// followed character's position every frame, but only past its
// !isLoading/!isPaused gate and only while objectCurrentlyFollowing is set
// and freeCameraMode is false -- otherwise it goes stale (IsCameraDetached).
const size_t OFF_CAM_CENTER_NODE        = 0x58;   // CameraClass::center (Ogre::SceneNode*)
const size_t OFF_CAM_CENTER_BUILDING    = 0xB0;   // CameraClass::centerBuilding (non-NULL indoors)
const size_t OFF_CAM_FREE_MODE          = 0xBF;   // CameraClass::freeCameraMode (bool)
// CameraClass::objectCurrentlyFollowing (a `hand`) is asserted at +0x28;
// its `.type` field is IDA-recovered at hand+0x8, giving +0x30 -- distinct
// from, and not composed with, OFF_PI_CAMERA above.
const size_t OFF_CAM_FOLLOWING_TYPE     = 0x30;
// NULL_ITEM, confirmed against two `cmp dword ptr [.], 0Bh` / jz-to-skip
// sites in CameraClass::update: 0x6B17E3 (objectCurrentlyFollowing.type
// itself) and 0x6B188F (the resolved hand's type after redirect lookup).
const int    ITEM_TYPE_NULL             = 0x0B;
const size_t RVA_GW_SET_GAME_SPEED      = 0x787300;  // GameWorld::setGameSpeed
const size_t RVA_GW_USER_PAUSE          = 0x787470;  // GameWorld::userPause
const size_t RVA_GW_TIMESTAMP_HOURS     = 0x66C180;  // GameWorld::getTimeStamp_inGameHours
const size_t RVA_GUI_IS_LOADING         = 0x6E1DB0;  // ForgottenGUI::isLoading
const size_t RVA_GUI_IS_PAUSED          = 0x6E1E60;  // ForgottenGUI::isPaused (escape menu visible)
const size_t RVA_PROCESS_LOADING        = 0xA0E950;  // ZoneManager::processLoading (main thread, every frame)
const size_t RVA_GW_TOGGLE_PAUSE        = 0x787200;  // GameWorld::togglePause(bool)
const size_t RVA_ESC_MENU_HIDE          = 0x916190;  // escape menu hide: unpauses iff menu+0xE8 == 0
const size_t OFF_ESC_MENU_WAS_PAUSED    = 0xE8;      // byte: pause state when the menu opened
const size_t OFF_GUI_ISPAUSED_GUARD_OP  = 0x0D;      // isPaused: mov eax,[rip+d] (8B 05) -> menu guard dword
const size_t OFF_GUI_ISPAUSED_MENU_OP   = 0x46;      // isPaused: mov rax,[rip+d] (48 8B 05) -> menu singleton
const size_t RVA_OPTIONS_IS_VISIBLE     = 0x3E7100;  // OptionsWindow::isVisible
const size_t RVA_OPTIONS_GET_SINGLETON  = 0x406B90;  // OptionsWindow::getSingleton (constructs on first call)
const size_t RVA_WEATHER_GET_INSTANCE   = 0xF89A0;   // WeatherSystem::getInstance (constructs on first call)
const size_t RVA_PI_START_TRACK_CHARACTER = 0x7F4860; // PlayerInterface::startTrackCharacter (NULL: first selected)
const size_t RVA_HAND_GET_ROOT_OBJECT   = 0x79C870;  // hand::getRootObject
const size_t RVA_HAND_GET_CHARACTER     = 0x7974F0;  // hand::getCharacter (platoons, then death parade)
const size_t RVA_CHARACTER_IS_DEAD      = 0x620B20;  // Character::isDead
// ActivePlatoon::calculateCurrentPos (src/fixes/world/corpse_pin.cpp). Private, so
// GetRealAddress cannot take its address from outside the class; the prologue
// compare against g_hookPrologues is the only check for this one.
const size_t RVA_ACTIVEPLATOON_CALC_POS = 0x4FE2C0;
const size_t RVA_GLOBAL_GUI             = 0x2132750; // the ForgottenGUI object (KenshiLib `gui`)
const size_t RVA_GLOBAL_INPUT           = 0x2132320; // the InputHandler object (KenshiLib `key`)
// Not in KenshiLib. The statics are checked against the RIP-relative operands
// of the covered function named beside them; the vtable on every read.
const size_t RVA_SKYX_CONTROLLER_VTABLE = 0x16FC420; // *(sky+0x20); float hour at +0x1C
const size_t RVA_USER_PAUSE_SPEED       = 0x2131E88; // float, userPause's saved speed
const size_t RVA_USER_PAUSE_GUARD       = 0x2131E8C; // bit 0: the saved speed is set
const size_t RVA_OPTIONS_INSTANCE       = 0x212E080; // OptionsWindow* (getSingleton's static)
const size_t RVA_WEATHER_INSTANCE       = 0x2127180; // WeatherSystem* (getInstance's static)


#endif // KEO_OFFSETS_H
