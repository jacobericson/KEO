// navmesh_sched.h — NavMesh thread priority boost and job queue prioritization
// Self-contained: no dependency on preload/tracking/worker_pool globals.
// Depends on: core.h, game.h (for gameBase, RVAs, queue lock fns)

#ifndef KEO_NAVMESH_SCHED_H
#define KEO_NAVMESH_SCHED_H

#include "base/config.h"


// =========================================================================
// Priority context structs (lightweight, no preload/tracking types)
// =========================================================================

struct SchedMoverInfo {
	int  currentX, currentY;
	int  destX, destY;
	bool hasMoveOrder;
};

struct SchedZoneInfo {
	int gridX, gridY;
};


// =========================================================================
// NavMesh scheduling functions (impl in navmesh_sched.cpp)
// =========================================================================

// NMG background thread priority boost (game.h globals only, no worker pool)
void BoostNavMeshThread();
void RestoreNavMeshThread();

// 5-tier priority for one zone (1 = most urgent .. 5 = least). Shared by
// PrioritizeNavMeshQueue and the tier-ordered registration order in
// preload_queue.cpp.
int ComputeZonePriority(int gridX, int gridY,
                        int camGridX, int camGridY,
                        const SchedMoverInfo* movers, int moverCount,
                        const SchedZoneInfo* preloaded, int preloadedCount);

// 5-tier job queue reordering — classification data passed explicitly.
// camGridX/Y: resolved camera position (caller handles transition-target override).
// movers/moverCount: watched characters with current/dest zones (can be NULL/0).
// preloaded/preloadedCount: preloaded zone coordinates (can be NULL/0).
void PrioritizeNavMeshQueue(int camGridX, int camGridY,
                            const SchedMoverInfo* movers, int moverCount,
                            const SchedZoneInfo* preloaded, int preloadedCount);


extern volatile long reprioTimerFires;
extern volatile long reprioOrderFires;


#endif // KEO_NAVMESH_SCHED_H
