// camera_zone_hook.h - Main-thread camera hook, focus reset and scheduling context.

#ifndef KEO_CAMERA_ZONE_HOOK_H
#define KEO_CAMERA_ZONE_HOOK_H

#include "base/config.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "zone/preload/preload.h"
#include "movement/tracking.h"

void hook_updateCameraZone(void* zoneMgr, void* cameraPos);

// Drops the camera-focus dwell state (camera_focus.h). Called wherever
// lastCameraGX/GY also reset (save load, transition-end compaction) so no
// stale pending zone from before the reset can carry into the new state.
void ResetCameraFocusState();


// =========================================================================
// NavMesh scheduling context (shared by PrioritizeNavMeshQueue callers and
// the tier-ordered registration order in preload_queue.cpp). Main thread only.
// =========================================================================

struct SchedContext {
	int camX, camY;                      // transition target if active, else last camera zone
	SchedMoverInfo movers[MAX_WATCHED];
	int moverCount;
	SchedZoneInfo zones[MAX_PRELOADED];
	int zoneCount;
};

void BuildSchedContext(SchedContext* ctx);

#endif // KEO_CAMERA_ZONE_HOOK_H
