// nm_force_rebuild.cpp - The rebuild-navmesh key: the force-mark table, the claim and
// finish, the forced job's cache clear and the cache line's token.
// Claims and finishes run on the NavMesh threads; nothing here allocates or logs.
#include "navmesh/nm_workers.h"
#include "zone/geometry/zone_geometry_epoch.h"
#include "zone/reset/zone_reset_gate.h"
#include "navmesh/generation/nm_misspar.h"
#include "navmesh/jobs/nm_buildlock.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include "plugin/hook_manifest.h"
#include "diag/exit_capture.h"
#include "navmesh/jobs/nm_busy_bridge_policy.h"
#include "navmesh/workers/nm_worker_gate_policy.h"
#include "navmesh/workers/nm_retire_policy.h"
#include "navmesh/nm_workers_internal.h"
#include "navmesh/jobs/nm_queue_lock.h"
#include "navmesh/cache/nm_force_rebuild.h"
#include "navmesh/cache/nm_force_rebuild_policy.h"

namespace nm_force_rebuild_detail {
struct LiveMark { int cell; long word; void* zone; };
}
using namespace nm_force_rebuild_detail;

static const int MARK_CELLS = NM_REBUILD_GRID * NM_REBUILD_GRID;

// Index gx * 64 + gy. The stamp is written before its word, both Interlocked.
static volatile LONG     s_markWord[MARK_CELLS];
static volatile LONGLONG s_markQpc[MARK_CELLS];

static volatile LONG s_marked, s_claimed, s_stored, s_refused, s_dropped, s_expired, s_lost;
static volatile LONG s_l1Evicted, s_l2Deleted, s_l2Absent, s_l2Failed;

// The zone's grid cell as a table index; -1 off the grid.
static int CellIndex(uintptr_t zone)
{
	const int gx = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	const int gy = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
	if (gx < 0 || gx >= NM_REBUILD_GRID || gy < 0 || gy >= NM_REBUILD_GRID)
		return -1;
	return gx * NM_REBUILD_GRID + gy;
}

static double AgeSec(LONGLONG from, LONGLONG now)
{
	return QpcToMs(now - from) / 1000.0;
}

int NmForceRebuildClaimLocked(const NmQueueLock&, uintptr_t zone, int jobType, long* wordOut)
{
	*wordOut = 0;
	if (!zone || jobType != 0)
		return -1;
	const int cell = CellIndex(zone);
	if (cell < 0)
		return -1;
	const LONG w = InterlockedCompareExchange(&s_markWord[cell], 0, 0);
	if (NmMarkStateOf(w) != NM_MARK_MARKED)
		return -1;   // nearly every claim: one read, no clock
	const double age = AgeSec(InterlockedCompareExchange64(&s_markQpc[cell], 0, 0), QpcNow());
	if (NmMarkClaimDecide(w, age, jobType) != NM_CLAIM_CONSUME)
		return -1;
	const LONG claimedWord = NmMarkWord(NmMarkSeqOf(w), NM_MARK_CLAIMED);
	if (InterlockedCompareExchange(&s_markWord[cell], claimedWord, w) != w)
		return -1;
	InterlockedIncrement(&s_claimed);
	*wordOut = claimedWord;
	return cell;
}

void NmForceRebuildFinish(int cell, long claimedWord, int outcome)
{
	if (cell < 0 || cell >= MARK_CELLS)
		return;
	InterlockedIncrement(outcome == NM_FORCE_STORED ? &s_stored : outcome == NM_FORCE_REFUSED ? &s_refused : &s_dropped);
	InterlockedCompareExchange(&s_markWord[cell], NmMarkWord(NmMarkSeqOf(claimedWord), NM_MARK_DONE), claimedWord);
}

static L2DeleteOutcome CountDelete(L2DeleteOutcome outcome)
{
	InterlockedIncrement(outcome == L2DEL_DELETED ? &s_l2Deleted : outcome == L2DEL_ABSENT ? &s_l2Absent : &s_l2Failed);
	return outcome;
}

bool NmForceRebuildClearKey(bool keyOk, const NavMeshCacheKey& key)
{
	if (!keyOk)
		return false;
	NmCacheLock cacheLock;
	const int evicted = EvictCacheEntriesForKey(cacheLock, key);
	cacheLock.Release();
	InterlockedExchangeAdd(&s_l1Evicted, evicted);
	return CountDelete(DeleteDiskCacheFile(key)) == L2DEL_FAILED;
}

void NmForceRebuildRetryDelete(const NavMeshCacheKey& key)
{
	CountDelete(DeleteDiskCacheFile(key));
}

void NmForceRebuildNoteEvicted(int n)
{
	if (n > 0)
		InterlockedExchangeAdd(&s_l1Evicted, n);
}

void NmForceRebuildFormatCacheToken(char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	const long claimed = InterlockedCompareExchange(&s_claimed, 0, 0);
	const long expired = InterlockedCompareExchange(&s_expired, 0, 0);
	const long lost = InterlockedCompareExchange(&s_lost, 0, 0);
	if (!claimed && !expired && !lost)
		return;
	_snprintf_s(out, outSize, _TRUNCATE, " forced=%ld/%ld/%ld/%ld/%ld/%ld forcedEvict=%ld/%ld/%ld/%ld",
	            claimed,
	            (long)InterlockedCompareExchange(&s_stored, 0, 0),
	            (long)InterlockedCompareExchange(&s_refused, 0, 0),
	            (long)InterlockedCompareExchange(&s_dropped, 0, 0),
	            expired, lost,
	            (long)InterlockedCompareExchange(&s_l1Evicted, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Deleted, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Absent, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Failed, 0, 0));
}
