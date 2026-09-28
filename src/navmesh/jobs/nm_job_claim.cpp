// nm_job_claim.cpp - worker claim, stale-drop and L2 in-flight accounting.
// queue +152 is a leaf; claim precedes cache lookup and refuses reset admission.
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
using namespace nm_workers_detail;
namespace nm_workers_detail {
// --------------------------------------------------------------------
// Claimed-job zone re-check
// --------------------------------------------------------------------
//
// dispatchJob_orig (0x3CE030) pops a job and, immediately before processJobAlt,
// returns 1 when the job's zone has no mapContent (`if (!**job) return 1;`),
// leaving the node neither freed nor enqueued. ZoneManager__unloadSingleZone
// (0xA09620, main thread) NULLs mapContent (+0) first and then frees and NULLs
// terrainCollision (+0xB8), and nothing in the unload path waits on the
// generator. So vanilla drops every job whose zone was unloaded before the bg
// thread reached it.
//
// The mod's claim sites make the same test at claim time, but a claimed job can
// then wait on processJobCS for seconds (a cold-cache MISS holds it 2-10 s) and
// the zone can be unloaded in between. processJobAlt reads
// *(zone+0xB8)+8 unconditionally for type 0/1 jobs (0x3C1580 via 0xA07B50, from
// 0x3CBE60+0xB3C), which is an access violation at rva
// 0x3C1600. This re-check restores vanilla's test after the last lock wait.
//
// terrainCollision is tested as well as mapContent: every job this code runs is
// type 0/1, where vanilla's processJobAlt dereferences it with no NULL check, so
// a job with content but no terrain would crash vanilla too.
//
// ZoneMap entries live in the ZoneManager's fixed array and are never freed, so
// job+0 and the zone's fields stay readable after an unload. Plain reads, no
// allocation: runs on the NavMesh bg thread and the workers.
bool JobZoneStillLoaded(uintptr_t job, int* reasonOut)
{
	uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	int reason = STALE_REASON_NONE;
	if (!zone)
		reason = STALE_REASON_NO_ZONE;
	else if (!*(uintptr_t*)(KLIB_MEMBER(4, zone, ZoneMap_mapContent, OFF_ZONE_CONTENT)))
		reason = STALE_REASON_NO_CONTENT;
	else if (!*(uintptr_t*)(KLIB_MEMBER(4, zone, ZoneMap_terrainCollision, OFF_ZONE_TERRAIN_COLLISION)))
		reason = STALE_REASON_NO_TERRAIN;
	if (reasonOut) *reasonOut = reason;
	return reason == STALE_REASON_NONE;
}
} // namespace nm_workers_detail


namespace nm_workers_detail {
void NoteStaleDrop(int site, uintptr_t job, int jobType, int reason, LONGLONG claimQpc)
{
	uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	long gx = zone ? *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X)) : -1;
	long gy = zone ? *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y)) : -1;
	long ageUs = claimQpc ? QpcDeltaUs(claimQpc, QpcNow()) : 0;

	// Five separate writes: a racing reader may see a torn last event, which
	// the stats line accepts (nm_cache_core.h).
	InterlockedExchange(&navmesh::g_nmCache.nmStaleLastGridX, gx);
	InterlockedExchange(&navmesh::g_nmCache.nmStaleLastGridY, gy);
	InterlockedExchange(&navmesh::g_nmCache.nmStaleLastType, (long)jobType);
	InterlockedExchange(&navmesh::g_nmCache.nmStaleLastReason, (long)reason);
	InterlockedExchange(&navmesh::g_nmCache.nmStaleLastAgeUs, ageUs);

	// Counted after the last-event fields, so a reporter that sees a non-zero
	// count always finds a real drop in them, never the initial values
	// (staleLast=(-1,-1)t-1/none@0.0ms). Later drops can still tear the event.
	InterlockedIncrement(&navmesh::g_nmCache.nmStaleCount[site]);
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
// A job dropped because NavMesh::stop was seen: recorded as a stale
// drop with reason shutdown, and counted once in stopDrop=. The job itself is
// left exactly as a stale drop leaves it: unlinked, not freed, not enqueued
// (~NavMeshGenerator frees no queued job either, so at quit nothing differs).
void NoteStopDrop(int site, uintptr_t job, int jobType, LONGLONG claimQpc)
{
	NoteStaleDrop(site, job, jobType, STALE_REASON_SHUTDOWN, claimQpc);
	InterlockedIncrement(&g_nmStopDropCount);
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
// After a job waited out a save-load reset: it goes on only while its zone
// still holds the content it held before the wait, and is still loaded. The
// reset may have unloaded the zone and the new world loaded other content into
// it; either way the content is not the job's and the reason is no content.
bool ResetWaitRevalidate(uintptr_t job, uintptr_t contentBefore, int* reasonOut)
{
	if (!ZoneResetContentKept((void*)contentBefore, (void*)JobZoneContent(job)))
	{
		*reasonOut = STALE_REASON_NO_CONTENT;
		return false;
	}
	return JobZoneStillLoaded(job, reasonOut);
}
} // namespace nm_workers_detail
// L2 reads in flight, by key, so two workers handed duplicate jobs for the same
// zone do not read the same file at once. Guarded by nmCacheCS. Small and
// fixed: at most one entry per worker plus the bg thread.
static NavMeshCacheKey g_l2InFlight[NAVMESH_WORKER_COUNT + 1];
static bool            g_l2InFlightUsed[NAVMESH_WORKER_COUNT + 1] = {};

// Caller holds nmCacheCS. Returns the slot taken, or:
//   L2FLIGHT_BUSY  — another thread is already reading this exact key, so the
//                    caller should skip its own read
//   L2FLIGHT_FULL  — no free slot. Not the same thing: nobody is reading this
//                    key, so the caller SHOULD read it, just without
//                    registering. Counted separately rather than reported as a
//                    duplicate, which would overstate dupL2.
static const int L2FLIGHT_BUSY = -1;
static const int L2FLIGHT_FULL = -2;

namespace nm_workers_detail {
int L2InFlightAcquire(const NavMeshCacheKey& key)
{
	int free = -1;
	for (int i = 0; i < NAVMESH_WORKER_COUNT + 1; ++i)
	{
		if (g_l2InFlightUsed[i])
		{
			if (KeysMatch(g_l2InFlight[i], key))
				return L2FLIGHT_BUSY;
		}
		else if (free < 0)
		{
			free = i;
		}
	}
	if (free < 0)
	{
		InterlockedIncrement(&navmesh::g_nmCache.nmL2FlightFull);
		return L2FLIGHT_FULL;
	}
	g_l2InFlight[free] = key;
	g_l2InFlightUsed[free] = true;
	return free;
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
void L2InFlightRelease(int slot)
{
	if (slot >= 0 && slot < NAVMESH_WORKER_COUNT + 1)
		g_l2InFlightUsed[slot] = false;
}
} // namespace nm_workers_detail

// Claims one job for this worker, then looks the cache up for it.
//
// The claim comes first: the job is unlinked under the queue lock before any
// lookup, so each worker owns a distinct job before it reads L1 or L2, and no
// lookup races another worker's for the same job. Two workers can still hold
// different jobs with one key (the game queues a zone twice): the L2 in-flight
// table lets one of them read the file, and the other is served by the
// late-HIT re-check.
//
// The scan takes the first type 0/1 job whose zone is loaded and is not being
// unloaded. While the save-load reset's admission gate (g_zoneResetGate) is
// up, read under the queue lock as g_unloadingZone is, nothing is claimed and
// the worker leaves as from an empty queue. With the adjacency exclusion on it
// also passes over the job the bg thread has pinned and, enforcing, any job
// whose stitches would touch one in flight or undrained, walks on past the take
// only to find an age reservation's node, and refuses the claim when the
// registry has no free entry.
// Types 2/3/4 and jobs with no loaded zone stay queued for the bg thread.
//
// The scan MUST leave the head and the order of every other node intact. The bg
// thread's hook_dispatchJob peeks the head, releases the queue lock and hands
// to orig_dispatchJob, which re-pops the head itself; the hook's wrap depends on
// the head it peeked still being the head the original pops. This scan preserves
// that in the stronger form: a worker only ever takes the head when the head is
// the job it processes, and otherwise leaves it untouched.
//
// Lock ordering: the queue lock is taken alone here and released before
// nmCacheCS, so it never nests with the cache lock in either direction.
//
// claimedOut->claimQpc: QPC taken right after the job is unlinked and the
// queue lock released, before the cache lookup (whose L2 read is part of the
// exposure the claim age measures). Carried to the HIT and pipeline paths.
//
// claimSlot: this worker's claimed-zone slot (its worker id), written under the
// queue lock with the job's zone; the worker loop clears it after the job.
namespace nm_workers_detail {
uintptr_t WorkerTryDequeueAny(int claimSlot, int* hitIdxOut, bool* isMissOut,
                                     NavMeshCacheKey* keyOut, ClaimedJob* claimedOut)
{
	*hitIdxOut = -1;
	*isMissOut = false;
	claimedOut->claimQpc = 0;

	uintptr_t nmg = g_navMeshGen;
	if (!nmg) return 0;

	// Phase 1: claim a job under the queue lock.
	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);

	uintptr_t head = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136));
	if (!head)
	{
		// Empty under the lock: the event is only ever cleared while holding
		// this lock, so a worker cannot miss a job queued after the check.
		ClearJobAvailable();
		game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
		return 0;
	}

	// A save-load reset is running: claim nothing and leave the list, the head
	// and the adjacency registry untouched. The lower takes this same queue
	// lock, then wakes workers after it, so the event clear precedes its set.
	if (ZoneResetGateUp(&g_zoneResetGate) && ZoneResetAdmit(true, NavMeshStopRequested(), ZONE_RESET_SITE_CLAIM) == ZONE_RESET_DEFER_RESET)
	{
		ClearJobAvailable();
		game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
		ZoneResetGateNoteDeferred(&g_zoneResetGate, ZONE_RESET_SITE_CLAIM);
		return 0;
	}

	uintptr_t prev = 0;     // predecessor of `job`, 0 when job is the head
	uintptr_t job  = 0;
	uintptr_t jobZone = 0;
	int jobType = -1;

	// The zone the main thread is unloading, read once under the lock
	// NavMeshBeginZoneUnload publishes it under. Its jobs stay queued.
	uintptr_t unloading = UnloadingZone();
	bool heldForUnload = false;

	// Adjacency exclusion: a candidate whose stitches would touch a job still
	// in flight or undrained is skipped, never waited for. With an age
	// reservation outstanding the walk goes on past the take to find its node.
	const bool adj = NmAdjActive();
	NmAdjScan adjScan;
	NmJobDesc adjDesc, adjTakenDesc;
	if (adj)
		NmAdjWorkerScanBeginLocked(&adjScan);

	for (uintptr_t node = head; node; node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96)))
	{
		int t = *(int*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_flags, 88)) & 7;
		uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, node, NavMeshGenerator__Task_zone, 0);
		// Types 2/3/4 stay on the bg thread (stitching mutates shared edge
		// data), and a job with no loaded zone is the bg thread's to forward.
		bool candidate = (t == 0 || t == 1) && zone && *(uintptr_t*)KLIB_MEMBER(4, zone, ZoneMap_mapContent, 0);
		if (candidate && zone == unloading)
		{
			// Being unloaded right now: never claim it. Vanilla drops it
			// later, once the unload has NULLed its content.
			heldForUnload = true;
			candidate = false;
		}
		if (adj)
		{
			if (job)
			{
				NmAdjWorkerObserveLocked(&adjScan, node, candidate);
				if (!NmAdjScanWantsRest(&adjScan))
					break;
				continue;
			}
			if (!candidate || NmAdjWorkerOfferLocked(&adjScan, node, true, &adjDesc) != NMADJ_OFFER_TAKE)
			{
				if (!candidate)
					NmAdjWorkerObserveLocked(&adjScan, node, false);
				prev = node;
				continue;
			}
			job = node;
			jobZone = zone;
			jobType = t;
			adjTakenDesc = adjDesc;
			if (!NmAdjScanWantsRest(&adjScan))
				break;
			continue;
		}
		if (candidate)
		{
			job = node;
			jobZone = zone;
			jobType = t;
			break;
		}
		prev = node;
	}

	if (heldForUnload)
		InterlockedIncrement(&navmesh::g_nmCache.nmUlHeld);

	// Registers the claim; enforcing, a claim with no free entry is refused
	// and the job stays queued.
	if (adj && !NmAdjWorkerScanEndLocked(&adjScan, claimSlot, job ? &adjTakenDesc : NULL,
	                                    InterlockedCompareExchange(&navmesh::g_nmCache.g_navMeshWorkersLive, 0, 0) > 0))
		job = 0;

	if (!job)
	{
		// The queue is not empty but holds nothing a worker may take: a run of
		// type 2/3/4 jobs, or jobs whose zone is not loaded. Clear the event
		// anyway. Leaving it set made every worker spin — the manual-reset wait
		// returns immediately, each worker re-takes the +152 lock the bg thread
		// needs, and none of them can make progress. The bg thread re-sets the
		// event after its own dequeue whenever the queue is still non-empty, so
		// the first eligible job wakes everyone; the 500 ms wait is the
		// backstop. The list is not touched.
		ClearJobAvailable();
		game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
		if (adj)
			NmAdjAfterScan(&adjScan);
		return 0;
	}

	// Unlink exactly this node. Head and relative order of everything else are
	// unchanged; +144 is the address of the last node's next-pointer, so it only
	// moves when the node being removed was the tail.
	uintptr_t next = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_next, 96));
	if (prev)
		*(uintptr_t*)(KLIB_MEMBER(4, prev, NavMeshGenerator__Task_next, 96)) = next;
	else
		*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)) = next;

	if (!next)
		*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_back, 144)) = prev ? (KLIB_MEMBER(4, prev, NavMeshGenerator__Task_next, 96)) : (KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136));

	bool queueStillHasWork = (*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)) != 0);

	// Busy from the moment the job leaves the queue, not from the moment
	// processing starts: between those two points the job is in neither +136
	// nor +232 and isBusy would report idle. Released once, at the end of the
	// worker loop body, on every exit path.
	WorkerBusyEnter(nmg);
	// The claim marker for the mod-unload protocol, also before the unlock:
	// from here until the worker loop clears it, NavMeshBeginZoneUnload refuses
	// this zone (the building hash below reads its content with no lock).
	ClaimZoneSet(claimSlot, jobZone);
	claimedOut->resetRaises = ZoneResetGateRaises(&g_zoneResetGate);

	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
	if (adj)
		NmAdjAfterScan(&adjScan);

	// Claim time: the job left the queue under the lock just released.
	claimedOut->claimQpc = QpcNow();
	NoteWorkerPhase(WPHASE_CLAIMED);

	// The job is ours now: nothing else can take or free it.
	if (queueStillHasWork)
		SignalJobAvailable();

	uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	NavMeshCacheKey key;
	key.gridX = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	key.gridY = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
	key.sectionTileId = *(int*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_hash, 32));
	key.jobType = jobType;
	key.aabbHash = HashAABB((float*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_bounds, 48)));
	// Claim time, outside every lock the main thread respects: the content can
	// be unloaded under this read. An untrusted hash skips
	// the L1 and L2 lookups, so the job takes the MISS path, where the early
	// and post-missLock zone re-checks decide whether it is generated or
	// dropped, and ProcessNavMeshJob computes (and checks) its own key.
	uintptr_t hashContent = 0;
	bool keyOk = ComputeBuildingHashChecked(zone, &key.buildingHash, &hashContent);

	// Phase 2: cache lookup for the claimed job.
	int hitIdx = -1;

	if (keyOk)
	{
		EnterCriticalSection(&nmCacheCS);
		int found = FindCacheEntry(key);
		if (found >= 0 && navmesh::g_nmL1.nmCache[found].cachedFaces != NULL && game::g_gameFn.fn_navMeshCtor != NULL)
			hitIdx = found;
		LeaveCriticalSection(&nmCacheCS);
	}

	if (keyOk && hitIdx < 0 && game::g_gameFn.fn_navMeshCtor != NULL)
	{
		// Duplicate jobs for one zone do exist, so two workers can hold
		// different jobs with the same key. Only one of them reads the file.
		EnterCriticalSection(&nmCacheCS);
		int flight = L2InFlightAcquire(key);
		LeaveCriticalSection(&nmCacheCS);

		if (flight == L2FLIGHT_BUSY)
		{
			// Another worker is reading this exact key. Skip the read and take
			// the MISS path, where the late-HIT re-check under processJobCS
			// picks up its result rather than generating again.
			InterlockedIncrement(&navmesh::g_nmCache.nmDupL2Avoided);
		}
		else
		{
			NavMeshCacheEntry diskEntry;
			memset(&diskEntry, 0, sizeof(diskEntry));
			LARGE_INTEGER tR0, tR1;
			QueryPerformanceCounter(&tR0);
			bool l2Hit = ReadDiskCache(key, diskEntry);
			QueryPerformanceCounter(&tR1);
			long readUs = (long)(QPCToMs(tR0, tR1) * 1000.0);
			InterlockedExchangeAdd(&navmesh::g_nmCache.nmDiskReadUsTimes1, readUs);

			EnterCriticalSection(&nmCacheCS);
			if (l2Hit)
				hitIdx = PromoteDiskEntryToL1(diskEntry);
			if (flight >= 0)
				L2InFlightRelease(flight);
			LeaveCriticalSection(&nmCacheCS);

			if (l2Hit && hitIdx >= 0)
				InterlockedIncrement(&navmesh::g_nmCache.nmDiskHitCount);
			else
				InterlockedIncrement(&navmesh::g_nmCache.nmDiskMissCount);
		}
	}

	InterlockedIncrement(&navmesh::g_nmCache.nmJobCount);
	*hitIdxOut = hitIdx;
	*isMissOut = (hitIdx < 0);
	*keyOut = key;
	claimedOut->job = job;
	claimedOut->claimSlot = claimSlot;
	return job;
}

} // namespace nm_workers_detail
