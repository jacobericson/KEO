// wall_splice_internal.h - The state the wall progress detour and the main-thread drain share:
// the record ring, the two callees the drain uses, the armed word and the counters.
#ifndef KEO_WALL_SPLICE_INTERNAL_H
#define KEO_WALL_SPLICE_INTERNAL_H

#include "navmesh/construction/splice_ring.h"

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

} // namespace navmesh

#endif
