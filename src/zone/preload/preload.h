// preload.h — Zone preloading: queues, registration, grid calibration
// (Layer 3). Declares the preload and zone-lifecycle public APIs.
// Implementation: preload_queue.cpp (dequeue/load), preload_prepare.cpp
// (registration/processContent), preload_saveload.cpp (reset/compaction),
// preload_pipe_stats.cpp (latency), zone_life.cpp (records/tick/reset),
// zone_registry.cpp, zone_unload.cpp, zone_leak_report.cpp,
// zone_first_time.cpp and zone_evict.cpp. Shared preload declarations are
// in preload_internal.h; lifecycle-only details are in zone_life_internal.h.
// Depends on: config.h

#ifndef KENSHI_ZONE_OPT_PRELOAD_H
#define KENSHI_ZONE_OPT_PRELOAD_H

#include "base/config.h"
#include "zone/grid.h"


// =========================================================================
// Preloading constants
// =========================================================================

const int MAX_PRELOADED = 45;             // camera (12) + 2 moving squads (12 each) + still (9)
const float PRELOAD_THRESHOLD = 2500.0f;  // units from zone center (transition at 4147.2)
const double CHAR_SCAN_INTERVAL = 2.0;    // seconds between character scans (fallback)
const int MAX_CHAR_ZONES = 24;            // max unique zones from character scan
const int CAMERA_RESERVED = 12;
const double CAM_LOG_INTERVAL = 10.0;


// =========================================================================
// Preloading structs
// =========================================================================

struct PreloadedZone {
	void*  zoneEntry;
	int    gridX;
	int    gridY;
	bool   gameOwned;
	bool   pending;
	bool   registered;
	bool   registeredEmpty;
	bool   contentProcessed;   // vtable[4] called to populate things/objects
	double loadTimeSec;
	int    owner;
	// Kept across the last transition end by
	// CompactPreloadZones. ProcessPreloadQueue's one-zone-in-flight gate
	// ignores such an entry, so the new location's zones start loading at
	// once. Set for every kept entry at compaction, false for every new one.
	bool   carried;
	// pipeLoadSec is the stable base for every pipeline-latency sample:
	// loadTimeSec itself gets reset at registration (it becomes the
	// age-since-registration clock the stall timeout and the zombie handler
	// read), so it cannot be reused here. -1.0 = this slot did not come from
	// our own loadSingleZone call (already-accessible / already-loading /
	// game-owned branches in ProcessPreloadQueue), so no sample is taken
	// for it.
	double pipeLoadSec;
	bool   pipeIsReadySeen;    // isZoneReady(zoneEntry) observed true once
	bool   pipe264Seen;        // content+264 observed cleared once
};

struct QueuedZone {
	int gridX, gridY;
};


// =========================================================================
// Main-thread preload split units write this working state with plain stores,
// and reset/compact it on save load and transition completion. Two readers
// run at bracket start in hook_showLoadingMessage (transition_hook.cpp), on
// whichever thread opens the bracket, the main thread or the contentStream
// thread: TargetZoneLetter and CaptureTransitionStart each clamp numPreloaded
// to MAX_PRELOADED and read the table without allocation or lock.
// There is no coherent publication; mixed values are tolerated diagnostics.
// =========================================================================

extern PreloadedZone preloadedZones[MAX_PRELOADED];
extern int numPreloaded;
extern int pendingCount;
extern int predictedCenterX;
extern int predictedCenterY;

extern QueuedZone cameraQueue[CAMERA_RESERVED];
extern int cameraQueueCount;
extern int cameraQueueNext;

extern QueuedZone charQueue[MAX_PRELOADED];
extern int charQueueCount;
extern int charQueueNext;

// Character scan state
extern double lastCharScanTime;
extern int charZonesQueued;

// Debug: camera zone logging
extern int lastCameraGX;
extern int lastCameraGY;

// Game-owned-zone guard: how many times ProcessPreloadQueue declined to call
// loadSingleZone because the game already owned the zone. Cumulative for the
// session, not per transition. See preload_queue.cpp for why it should stay 0.
extern int preloadSkipLoaded;

// Registry guard: how many times a zone was refused because its
// ZoneMap handle registration is not this content's (zone_registry.cpp,
// ZoneRegistrationOk). Cumulative for the session, like preloadSkipLoaded, and
// printed beside it on the Transition line as regSkip=. Should stay 0: with
// the save-load reset unload in place no mod zone survives a load.
extern int regSkipCount;


// =========================================================================
// Preloading functions (impl across the split units listed in the banner
// comment at the top of this file)
// =========================================================================

void ClearPreloadZones();
// Full reset, including the navmesh caches. Startup only.
void ClearPreloadState();
// Save-load reset: the same, but keeps the world-keyed navmesh caches.
void ClearPreloadStateForLoad();
// Save-load detector (main thread). Returns true while the game is loading a
// save, in which case the caller must skip all preload processing this frame.
bool PreloadCheckSaveLoad(void* zoneMgr);
void CalibrateZoneGrid(void* zoneMgr);
bool WorldToZoneGrid(float worldX, float worldZ, int* outX, int* outY);
bool EnqueueCameraZone(int gx, int gy);
bool EnqueueCharacterZone(int gx, int gy);
void FlushCameraQueue();
// Queues the camera 3x3 and returns how many cells the queue took; the rest
// were already queued or already preloaded.
int EnqueueCameraGrid(int centerX, int centerY);
void EnqueueAheadZones(int centerX, int centerY, int fromX, int fromY, int owner);
void ProcessPreloadQueue(void* zoneMgr);
bool TrySquadSwitchSwap(void* zoneMgr, int camGX, int camGY);
void EvictStaleZones(void* zoneMgr, double now);
void TryRegisterPreloadedZones(void* zoneMgr, double now);

// The number of zones the mod loaded that are still loaded but no
// longer tracked (the ZoneLeak: orphans), for the PLAYER STUCK line's orph=
// field. -1 = not measured (prints "-"). Main thread.
int PreloadZoneLeakOrphans();

// The transition-end call. Replaces ClearPreloadZones there: keeps the
// working table's pending and registered entries, its game-owned ones (they
// still leave after 30 s through EvictStaleZones) and its stalled ones (for
// the zombie handler's unload); drops both queues; never touches the
// per-cell zone-lifecycle record.
// transitionSec is the transition's length: every kept pending entry's
// loadTimeSec moves forward by it (capped at now), so the stall timer, which
// does not run during a transition, resumes where it stopped. Every
// kept entry is marked carried. Main thread.
void CompactPreloadZones(double transitionSec);

// Per-frame zone-lifecycle work, called from hook_updateCameraZone
// after PreloadCheckSaveLoad and the destroyListOE replay (main thread, never
// during a save load): the ZoneLeak: line every 30 s, and with zoneLifeUnload
// on, the unload pass.
void ZoneLifeTick(void* zoneMgr, double now);

// Hook on the save-load reset's Set A/B unload, sub_14036C1E0
// (save_load_bindings.h RVA_RESET_UNLOAD_ZONES). Runs the original, then unloads every zone
// that still holds a content (the mod's: in neither set) through the same call
// the original uses, logs "Save load reset: ...", and (main thread only)
// clears the mod's state for the load. INI saveLoadUnload=false counts only.
void __fastcall hook_resetUnloadZones(void* zoneMgr);

// Each loading-screen dismissal logs a "screen=<ms>ms" line, the "Transition:
// ... ms" duration restated so the line stands alone. preload_pipe_stats.cpp
// detects the dismissal itself; it has no public entry point.


#endif // KENSHI_ZONE_OPT_PRELOAD_H
