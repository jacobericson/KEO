// tracking.h — Character tracking: watched characters, movement polling (Layer 3)
// Depends on: config.h

#ifndef KENSHI_ZONE_OPT_TRACKING_H
#define KENSHI_ZONE_OPT_TRACKING_H

#include "base/config.h"


// =========================================================================
// Character tracking constants
// =========================================================================

const double BASELINE_SCAN_INTERVAL = 5.0;
const double ACTIVE_POLL_INTERVAL   = 1.0;
// Array bound. How many entries the registry actually fills is a runtime rule
// (WatchedCapacity, mover_policy.h): 64 with the full player-faction registry,
// 32 without it.
const int    MAX_WATCHED            = 64;
const float  EDGE_THRESHOLD         = 1800.0f;  // must be < zoneStep/2 (2304)


// =========================================================================
// Watched character struct + state (defined in tracking.cpp)
// =========================================================================

struct WatchedCharacter {
	uintptr_t character;
	uintptr_t charMovement;
	bool      hasMoveOrder;
	int       destZoneX;
	int       destZoneY;
	int       currentZoneX;
	int       currentZoneY;
	double    addedTime;
};


extern WatchedCharacter watchedChars[MAX_WATCHED];
extern int numWatched;
extern double lastBaselineScan;
extern double lastActivePoll;
extern int hookOrderCount;


// =========================================================================
// Tracking functions (impl in tracking.cpp)
// =========================================================================

bool AddWatchedCharacter(uintptr_t character, uintptr_t charMovement,
                         int destZX, int destZY, int curZX, int curZY,
                         bool hasMoveOrder);
void RemoveWatchedCharacter(int index);
bool IsCharacterWatched(uintptr_t character);
// Watched and still following a move order (PollActiveMovers preloads ahead of it).
bool IsCharacterMovingOnOrder(uintptr_t character);
void ScanCharacterZones(void* zoneMgr);
void PollActiveMovers(void* zoneMgr, double now);
void TieredCharacterPoll(void* zoneMgr, double now);
void EnsurePlayerCharsWatched();

// Nearest of PlayerInterface::playerCharacters to (px,pz), for the camera
// focus distance cap (camera_focus.h/camera_zone_hook.cpp). False if the player global
// is unreadable or the roster is empty.
bool FindNearestPlayerCharacterXZ(float px, float pz, float* outX, float* outZ);

// Every watched mover's current zone and the next zone toward its
// destination, for the zone-lifecycle retention set (zone_life.cpp). Writes up
// to cap cells into gx/gy and returns how many. Main thread; stored fields
// only (currentZoneX/Y, destZoneX/Y), never the character pointer.
int CollectMoverRetainZones(int* gx, int* gy, int cap);


#endif // KENSHI_ZONE_OPT_TRACKING_H
