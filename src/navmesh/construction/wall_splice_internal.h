// wall_splice_internal.h - The state the wall progress detour and the main-thread drain share:
// the record ring, the two callees the drain uses, the armed word and the counters.
#ifndef KEO_WALL_SPLICE_INTERNAL_H
#define KEO_WALL_SPLICE_INTERNAL_H

#include "navmesh/construction/splice_ring.h"
#include "game/bindings.h"
#include <stdint.h>

namespace navmesh {

typedef bool (__fastcall *queuesAreClearMT_t)(void* physics);
typedef void (__fastcall *navMeshGenerateAabb_t)(void* navmesh, const float* box /* Ogre::Aabb: centre, half size */);

// POD, zero-initialised. The ring is initialised by InstallWallSplice before the row installs;
// the two callees are NULL until their heads matched, and are read only by the drain, which runs
// only while armed. Counters are interlocked; armed and detourInstalled are written at startup.
struct WallSpliceState
{
	SpliceRing ring;
	queuesAreClearMT_t    fn_queuesAreClearMT;
	navMeshGenerateAabb_t fn_navMeshGenerateAabb;
	volatile LONG armed, detourInstalled, recorded, merged, issued, requeued, droppedGone,
	              droppedLoad, pendingFull, t1Start, t1Racing;
};

extern WallSpliceState g_wallSplice;

// What a held splice reads once per main-thread tick before it judges any box.
struct SpliceGateSnapshot
{
	int       mainCount;     // hullsToChangeGroup.mainThreadData.count
	int       backCount;     // hullsToChangeGroup.backThreadData.count
	bool      queuesClear;   // the caller's PhysicsInterface::queuesAreClearMT
	bool      worldOk;       // SpliceWorldOk, with a zone manager and a section manager
	void*     zoneMgr;
	uintptr_t sectionMgr;
	isContentPending_t isReady;
};
// Main thread: no save load's reset gate, no NavMesh::stop, no transition.
bool SpliceWorldOk();
// Main thread: both change-group counts, fn's answer and the world, once per tick.
SpliceGateSnapshot SpliceGateTake(queuesAreClearMT_t fn);
// Main thread: the cell is neither loading nor accessible (or off the grid). Two flag reads.
bool SpliceCellGone(void* zoneMgr, int x, int y);
// Main thread: accessible and ready by the original isContentPending, which takes the section
// manager's +0x1E0 lock as vanilla's main-thread callers do.
bool SpliceCellReady(const SpliceGateSnapshot& s, int x, int y);

} // namespace navmesh

#endif
