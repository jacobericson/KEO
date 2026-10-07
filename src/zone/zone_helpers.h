// zone_helpers.h - Inline zone, character and camera accessors.
// Included through game.h.
// Accessors take no mod locks; callers provide object lifetime and thread safety.
// Camera and tracking-set queries run on the main thread.

#ifndef KEO_ZONE_HELPERS_H
#define KEO_ZONE_HELPERS_H

#include "game/klib_members.h"
#include "base/core.h"
#include "zone/preload/camera_focus.h"
#include "game/rva.h"
#include "game/offsets.h"
#include "game/bindings.h"

// =========================================================================
// Zone helpers (inline)
// =========================================================================

inline void* GetZoneEntry(void* zoneMgr, int x, int y)
{
	if (x < 0 || x > ZONE_GRID_MAX || y < 0 || y > ZONE_GRID_MAX)
		return NULL;
	return (void*)KlibZoneEntry((uintptr_t)zoneMgr, y + x * 64);
}

inline int GetZoneState(void* zoneMgr)
{
	return *(int*)(KLIB_MEMBER(1, (uintptr_t)zoneMgr, ZoneManager_loadingPhase, OFF_ZM_STATE));
}

inline bool IsZoneLoading(void* zoneEntry)
{
	return *(unsigned char*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_stateT_mainThreadData__zoneBeingLoaded, OFF_ZONE_IS_LOADING)) != 0;
}

inline bool IsZoneAccessible(void* zoneEntry)
{
	return *(unsigned char*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_stateT_mainThreadData__zoneIsLoaded, OFF_ZONE_IS_ACCESS)) != 0;
}

inline float GetZoneCenterX(void* zoneEntry)
{
	return *(float*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_center_x, OFF_ZONE_CENTER_X));
}

inline float GetZoneCenterZ(void* zoneEntry)
{
	return *(float*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_center_z, OFF_ZONE_CENTER_Z));
}

inline int GetZoneGridX(void* zoneEntry)
{
	return *(int*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
}

inline int GetZoneGridY(void* zoneEntry)
{
	return *(int*)(KLIB_MEMBER(1, (uintptr_t)zoneEntry, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
}

inline float GetCharPosX(uintptr_t character)
{
	return *(float*)(KLIB_MEMBER(1, character, RootObjectBase_pos_x, OFF_CHAR_POS_X));
}

inline float GetCharPosZ(uintptr_t character)
{
	return *(float*)(KLIB_MEMBER(1, character, RootObjectBase_pos_z, OFF_CHAR_POS_Z));
}

inline unsigned int GetPlayerCharCount(uintptr_t playerIntf)
{
	return *(unsigned int*)(KLIB_MEMBER(1, playerIntf, PlayerInterface_playerCharacters_count, OFF_PI_CHAR_COUNT));
}

inline uintptr_t* GetPlayerCharStuff(uintptr_t playerIntf)
{
	return *(uintptr_t**)(KLIB_MEMBER(1, playerIntf, PlayerInterface_playerCharacters_stuff, OFF_PI_CHAR_STUFF));
}

// CameraClass::getCenter, called directly (not hooked): RCX=camera,
// RDX=hidden return pointer to 3 floats (x,y,z), matching MSVC's struct-return
// ABI for a small-by-value Ogre::Vector3. Main thread only.
typedef void* (__fastcall *cameraGetCenter_t)(void* camera, float* outXYZ);

// Reads the orbit/follow anchor's world X/Z through PlayerInterface::camera ->
// CameraClass::getCenter(). False when the player, camera or center node
// pointer chain is unreadable (main menu, very early frames).
inline bool GetCameraFocusXZ(uintptr_t playerIntf, float* outX, float* outZ)
{
	if (!playerIntf) return false;
	uintptr_t camera = *(uintptr_t*)(playerIntf + OFF_PI_CAMERA);
	if (!camera) return false;
	uintptr_t centerNode = *(uintptr_t*)(camera + OFF_CAM_CENTER_NODE);
	if (!centerNode) return false;

	cameraGetCenter_t getCenter = (cameraGetCenter_t)GameAddr(RVA_CAMERA_GET_CENTER);
	float xyz[3];
	getCenter((void*)camera, xyz);
	*outX = xyz[0];
	*outZ = xyz[2];
	return true;
}

// The game's Fast zone hopping option (OptionsHolder::manyActiveZones): with
// it on, the game keeps a 3x3 around each player character instead of the
// cell it stands in. False before the bindings are ready. Main thread, where
// the Options window writes it.
inline bool GameFastZoneHopping()
{
	uintptr_t o = (uintptr_t)GameAddr(RVA_GLOBAL_OPTIONS);
	return o && *(const unsigned char*)(KLIB_MEMBER(5, o, OptionsHolder_manyActiveZones, 0x88)) != 0;
}

// ForgottenGUI::isLoading / ::isPaused, called directly (not hooked):
// RCX=the ForgottenGUI object (a fixed global, not a pointer to dereference).
// This is exactly the pair CameraClass::update (0x6B1540) gates its whole
// body on, so it tells us precisely when `center` stopped being updated.
typedef bool (__fastcall *guiBoolQuery_t)(void* gui);

inline bool IsGameLoadingOrPaused()
{
	void* guiObj = GameAddr(RVA_GLOBAL_GUI);
	guiBoolQuery_t isLoading = (guiBoolQuery_t)GameAddr(RVA_GUI_IS_LOADING);
	guiBoolQuery_t isPaused  = (guiBoolQuery_t)GameAddr(RVA_GUI_IS_PAUSED);
	return isLoading(guiObj) || isPaused(guiObj);
}

// True when `center` is not being kept current this frame: free-camera mode,
// an interior (centerBuilding set), nothing followed (objectCurrentlyFollowing
// is NULL_ITEM), or CameraClass::update's own !isLoading/!isPaused gate is
// closed. `center` then holds a stale position from whenever it last updated.
inline bool IsCameraDetached(uintptr_t playerIntf)
{
	if (!playerIntf) return true;
	uintptr_t camera = *(uintptr_t*)(playerIntf + OFF_PI_CAMERA);
	if (!camera) return true;

	bool freeMode      = *(bool*)(camera + OFF_CAM_FREE_MODE);
	bool hasBuilding   = *(uintptr_t*)(camera + OFF_CAM_CENTER_BUILDING) != 0;
	bool followingNull = *(int*)(camera + OFF_CAM_FOLLOWING_TYPE) == ITEM_TYPE_NULL;
	bool loadingPaused = IsGameLoadingOrPaused();
	return CameraFocusIsStale(freeMode, hasBuilding, followingNull, loadingPaused);
}

// Tracking-set membership (game.cpp). True when `zone` is an element of the
// ZoneManager's Set A (ZM+1474760) / Set B (ZM+1474824). Main thread: both
// sets are mutated by the zone state machine there. A linear walk of the
// boost::unordered_set node list (the layout WalkSetB reads in island_components.cpp),
// bounded by the set's size; SEH-guarded, so a fault reads as "not a member".
// false for a NULL manager or zone.
bool ZoneInSetA(void* zoneMgr, void* zone);
bool ZoneInSetB(void* zoneMgr, void* zone);



#endif // KEO_ZONE_HELPERS_H
