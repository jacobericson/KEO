// nm_job_pipeline.cpp - HIT reconstruction and MISS generation pipeline.
// HIT avoids processJobCS; MISS keeps processJobCS then nmCacheCS, with reset waits unlocked.
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
// Undoes the bg thread's swap-path install (a fresh work buffer written over
// realNMG+256 for the duration of processJobAlt) if something unwinds out of
// the generate branch before the explicit restore. Declared after missLock in
// ProcessNavMeshJob, so on that path it runs first and the restore happens
// while processJobCS is still held; the normal path restores explicitly
// (Restore) before the L1 store, exactly where it always did. The fresh work
// buffer itself is left to leak on the unwind path: freeing one processJobAlt
// may have been part-way through with is the worse risk.
namespace nm_job_pipeline_detail {
struct WbSwapRestore
{
	uintptr_t* slot;
	uintptr_t  orig;

	WbSwapRestore() : slot(NULL), orig(0) {}
	~WbSwapRestore() { Restore(); }

	void Arm(uintptr_t* s, uintptr_t o) { slot = s; orig = o; }

	// Idempotent: writes the original back once, then does nothing.
	void Restore()
	{
		if (!slot) return;
		*slot = orig;
		slot = NULL;
	}

private:
	WbSwapRestore(const WbSwapRestore&);
	WbSwapRestore& operator=(const WbSwapRestore&);
};
}
using namespace nm_job_pipeline_detail;
// Input geometry size of the generation running on this thread, captured by
// hook_nmResultPopulate_diag and read back at the store site. -1 means the hook
// did not run for this job (not installed, or the job never reached populate).
static __declspec(thread) int t_lastInputTriCount  = -1;
static __declspec(thread) int t_lastInputVertCount = -1;
// --------------------------------------------------------------------
// Worker HIT processing
// --------------------------------------------------------------------
//
// WorkerTryDequeueAny already looked up the cache entry. Here we just
// reconstruct the cached hkaiNavMesh and run buildCollision + job finalize.
// No processJobAlt.
//
// Zone re-check first: time has passed since the claim
// (the claim comes first, so that includes the L1 lookup and the L2 read in
// WorkerTryDequeueAny, 31-46 ms per read), and a zone
// the game unloaded in between is dropped the way dispatchJob_orig drops it:
// not freed, not enqueued. The window left after the check is the
// reconstruct plus the buildCollisionCS wait inside the builder hook; it is
// measured (claimAge hit=) rather than checked. Not to be confused with
// hitStale=, which counts a stale cache slot, not a stale zone.
// Returns false when the slot could not be used, leaving the job untouched for
// the caller to regenerate. Never finalizes or deletes the job in that case:
// the null-result path would delete the job and leave the zone with no mesh
// at all.
// Returns true when the job is finished with: served, or dropped because its
// zone was unloaded (the caller must not regenerate a dropped job). Either way
// the caller's single ClaimedJobFinish releases the bridge.
//
// A save-load reset running when the job reaches the rebuild or its collision
// build is waited out here, holding no lock. A job whose zone lost its content
// meanwhile is dropped: before the rebuild it is left untouched, as above;
// after it, the build is skipped and the job is freed as a result that is not
// enqueued.
namespace nm_workers_detail {
// No lock is held here. A job that waited, or crossed a whole reset since
// claim, must still hold the content read immediately before this wait.
static bool ResetWaitAt(ZoneResetSite site, uintptr_t job, LONG raisesAtClaim, int* reasonOut)
{
	uintptr_t contentBefore = JobZoneContent(job);
	return ZoneResetGateWaitSince(&g_zoneResetGate, site, &NavMeshStopSeen, raisesAtClaim)
		== ZONE_RESET_WAIT_WAITED && !ResetWaitRevalidate(job, contentBefore, reasonOut);
}

bool WorkerProcessHit(void* nmg, const ClaimedJob* claimed, int hitIdx, const NavMeshCacheKey& key)
{
	{
		int resetReason = STALE_REASON_NONE;
		if (ResetWaitAt(ZONE_RESET_SITE_HIT, claimed->job, claimed->resetRaises, &resetReason))
		{
			NoteStaleDrop(STALE_SITE_HIT, claimed->job, claimed->jobType, resetReason, claimed->claimQpc);
			return true;
		}
	}

	// Before the busy raise below, so the drop has nothing to release.
	{
		NoteClaimAge(false, claimed->claimQpc, QpcNow());
		int staleReason = STALE_REASON_NONE;
		if (!JobZoneStillLoaded(claimed->job, &staleReason))
		{
			NoteStaleDrop(STALE_SITE_HIT, claimed->job, claimed->jobType, staleReason, claimed->claimQpc);
			return true;
		}
	}


	void* freshNavMesh = NULL;
	bool replaced = false;
	NmCacheLock cacheLock;
	// The ring can publish over the slot after the lookup, so the saved index is re-checked against the key.
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);
	freshNavMesh = ReconstructExpected(cacheLock, key, hitIdx, &replaced);
	QueryPerformanceCounter(&t1);
	if (!replaced)
	{
		long ms10 = (long)(QPCToMs(t0, t1) * 10.0);
		InterlockedExchangeAdd(&navmesh::g_nmCache.nmSavedMsTimes10, ms10);
	}
	cacheLock.Release();

	if (!freshNavMesh)
	{
		// Stale slot, or the reconstruct failed. Hand the job back whole; the
		// worker loop keeps the busy bridge raised and releases it once.
		InterlockedIncrement(&navmesh::g_nmCache.nmHitStaleCount);
		return false;
	}

	// A reset drop keeps +80 NULL (never built) and takes the non-enqueue branch.
	bool resetDrop = false;
	if (freshNavMesh)
	{
		*(void**)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_mesh, 72)) = freshNavMesh;
		*(void**)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_output, 80)) = NULL;
		InterlockedIncrement(&navmesh::g_nmCache.nmCacheHitCount);
		int resetReason = STALE_REASON_NONE;
		if (ResetWaitAt(ZONE_RESET_SITE_BUILD, claimed->job, claimed->resetRaises, &resetReason))
		{
			NoteStaleDrop(STALE_SITE_HIT, claimed->job, claimed->jobType, resetReason, claimed->claimQpc);
			resetDrop = true;
		}
		else
		{
			NoteWorkerPhase(WPHASE_BUILDING);
			game::g_gameFn.fn_buildCollision(nmg, (void*)claimed->job, 0, 0.0);
			NoteWorkerPhase(WPHASE_STORING);
		}
	}

	void* label29NavInst = *(void**)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_output, 80));
	if (label29NavInst)
		*(int*)(KLIB_MEMBER(4, (uintptr_t)label29NavInst, NavInstance_hash, 64)) = *(int*)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_hash, 32));

	uintptr_t navMeshResult = *(uintptr_t*)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_mesh, 72));
	int faceCount = navMeshResult ? *(int*)(KLIB_MEMBER(4, navMeshResult + NMOFF_FACES, ByteArray_m_size, 8)) : 0;
	if (!resetDrop && navMeshResult && faceCount > 0)
	{
		game::g_gameFn.fn_enqueueToProcQueue((void*)(KLIB_MEMBER(4, (uintptr_t)nmg, NavMeshGenerator_done, 184)), (void*)claimed->job);
	}
	else
	{
		void* delNavInst = *(void**)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_output, 80));
		if (delNavInst) game::g_gameFn.fn_gameDelete(delNavInst);
		void* buildingRef = *(void**)(KLIB_MEMBER(4, claimed->job, NavMeshGenerator__Task_buildings_stuff, 24));
		if (buildingRef) game::g_gameFn.fn_gameDelArr(buildingRef);
		game::g_gameFn.fn_gameDelete((void*)claimed->job);
	}


	return true;
}


} // namespace nm_workers_detail
// --------------------------------------------------------------------
// Job processing pipeline
// --------------------------------------------------------------------
//
// Called by both the NavMesh bg thread (via hook_dispatchJob) and workers
// (for MISSes, via NavMeshWorkerProc).
//
//   realNMG = game's actual NavMeshGenerator. Always used for buildCollision
//             and result enqueue (section BST + output queue are shared).
//   workNMG = either realNMG (bg thread, or worker when CloneNMG failed) or a
//             per-worker clone (worker happy path).
//
// Cache HITs: reconstruct from L1/L2. No processJobCS — ReconstructNavMesh
// and buildCollision are thread-safe.
//
// Cache MISSes: processJobCS serializes fn_processJobAlt with the bg thread
// and any other worker. When workNMG==realNMG we use a swap-settings trick —
// temporarily override realNMG+256 with a freshly-built settings block,
// restore it before LeaveCS so buildCollision sees the original. When
// workNMG!=realNMG the clone already has its own settings installed.
//
// claimQpc: QPC at the job's unlink from the queue (the worker's
// WorkerTryDequeueAny or the bg thread's pop in hook_dispatchJob), 0 if
// unknown. Used only for the claim-age measurement and the stale-drop record.
//
// Zone unloaded while claimed: the MISS path re-checks
// the job's zone right after acquiring processJobCS and, when the game has
// unloaded it, returns without generating, leaving the job node as
// dispatchJob_orig's early return leaves it. The bg-thread caller then returns 1,
// the same value vanilla returns for a dropped job.
//
// Exits, and what each releases:
//   1. HIT in this function's own L1/L2 lookup (bg thread, or a worker MISS
//      whose key hit here after all): no processJobCS. Tail: result enqueued
//      (or job deleted on a null/zero-face result); the caller's own
//      ClaimedJobFinish releases the bridge. A failed reconstruct takes the MISS path.
//   2. MISS, late HIT: missLock released at the top of the branch, rungs 0-2
//      free their pre-lock fresh WB; buildCollision; then the tail as in 1.
//   3. MISS, generate: fresh WB installed at realNMG+256 and restored before the
//      release, L1 store under the lock, missLock released, buildCollision,
//      fresh WB freed; then the tail, and the L2 write last.
//   4. MISS, zone unloaded: missLock released right after it was acquired,
//      rungs 0-2 free their pre-lock fresh WB (never installed); the caller's
//      own ClaimedJobFinish releases the bridge. Job untouched; nothing enqueued,
//      deleted or written. A worker caller frees its clone after the return as always.
//      The same exit takes a job whose missLock wait saw
//      NavMesh::stop (the lock never won) or that sees it just after the lock:
//      reason shutdown, counted in stopDrop=. On the bg thread
//      this is how no new MISS starts after the stop; HITs are still served.
//   5. Reset drop: the job waited out a save-load reset, holding no lock, and
//      its zone no longer holds the content it held before the wait. Before
//      the lookup it leaves as exit 4 does, with nothing registered, locked or
//      allocated yet. Before a collision build (HIT, late HIT, generate) the
//      build is skipped and the tail frees the job as a result it does not
//      enqueue; the generate branch still frees its fresh WB and writes L2.

// The type-1 splice reads the job's zone. Checked under processJobCS right
// before it: an unload that did not wait for the lock can have taken the zone
// during the generation. The splice is then skipped and the mesh goes on
// unspliced; the store refuses it, since the zone content changed.
static inline bool PartialZoneStillLoaded(uintptr_t job)
{
	if (!JobZoneStillLoaded(job, NULL))
	{
		MissParNotePartialSkip();
		return false;
	}
	return true;
}

namespace nm_job_pipeline_detail {
// Per-claim scalar/POD state. Live lock and unwind guards stay on the
// ProcessNavMeshJob stack and are passed to the phases that release them.
struct PjCtx
{
	void* realNMG;
	void* workNMG;
	uintptr_t job;
	int jobType;
	LONGLONG claimQpc;
	uintptr_t nmg;
	bool onBgThread;
	uintptr_t jobZone;
	int gridX;
	int gridY;
	int tileId;
	L2WriteBlob* pendingWrite;
	ClaimedJob* claimed;
	NavMeshCacheKey key;
	bool resetDrop;
	uintptr_t hashContent;
	bool keyOk;
	int hitIdx;
	bool isHit;
	bool isL2Hit;
	LARGE_INTEGER t0;
	LARGE_INTEGER t1;
	void* localFreshWB;
	uintptr_t localOrigWB;
	bool usingNMGClone;
	MissParJob mpj;
	LONGLONG missWaitStart;
	LONGLONG missLockAt;
	void* lateMesh;
	bool cloneActive;

	bool Begin();
	void Lookup();
	void OwnHit();
	void MissSetup();
	bool CheckMissAfterLock(ProcessJobLock& missLock);
	void LateLookup();
	void LateBuild(ProcessJobLock& missLock, InflightScope& inflight);
	void GenConstruct();
	void GenPrepare(WbSwapRestore& swapRestore);
	bool AfterProcessJobAlt(bool pjLockLost, ProcessJobLock& missLock);
	void StoreGenerated(WbSwapRestore& swapRestore, ProcessJobLock& missLock, InflightScope& inflight);
	void BuildGenerated();
	void Handoff();
};

bool PjCtx::Begin()
{
	nmg = g_navMeshGen;

	// Worker vs bg thread for the pjWait / stale sites. By thread rather than
	// by workNMG, so a worker whose CloneNMG failed (workNMG == realNMG) still
	// counts as a worker.
	onBgThread = (GetCurrentThreadId() == (DWORD)g_navMeshBgThreadId);

	// bridge: game's isContentPending reads this byte. Every caller raises it
	// at claim time, so it is not raised again here.

	jobZone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	gridX = *(int*)(KLIB_MEMBER(4, jobZone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	gridY = *(int*)(KLIB_MEMBER(4, jobZone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
	tileId = *(int*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_hash, 32));

	// Filled from the L1 deep copy on a MISS, written to disk at the very
	// end of the job — after the game has the mesh.
	memset(pendingWrite, 0, sizeof(*pendingWrite));

	InterlockedExchange(&navmesh::g_nmCache.nmDiagLastGridX, gridX);
	InterlockedExchange(&navmesh::g_nmCache.nmDiagLastGridY, gridY);
	InterlockedExchange(&navmesh::g_nmCache.nmDiagLastType, jobType);

	key.gridX = gridX;
	key.gridY = gridY;
	key.sectionTileId = tileId;
	key.jobType = jobType;
	key.aabbHash = HashAABB((float*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_bounds, 48)));

	// Set by a collision-build site that drops the job after a reset wait; the
	// tail then frees the job instead of enqueueing it.
	resetDrop = false;
	{
		int resetReason = STALE_REASON_NONE;
		if (ResetWaitAt(ZONE_RESET_SITE_HIT, job, claimed->resetRaises, &resetReason))
		{
			// Exit 5 before the lookup: the job is left as exit 4 leaves it.
			NoteStaleDrop(onBgThread ? STALE_SITE_BGMISS : STALE_SITE_WMISS,
			              job, jobType, resetReason, claimQpc);
			InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 36);
			InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 50);
			return false;   // the worker caller still frees its clone
		}
	}
	return true;
}

void PjCtx::Lookup()
{
	// Before missLock, i.e. outside every lock the main
	// thread respects, so the game can unload the content under this read.
	// keyOk false: no L1/L2 lookup, no late-HIT lookup and no L1 store (hence
	// no L2 write) for this job; it is generated, or dropped by the re-check
	// after missLock. hashContent is re-compared at the
	// store (ZoneContentUnchanged) to catch an unload that began after the walk.
	hashContent = 0;
	keyOk = ComputeBuildingHashChecked(jobZone, &key.buildingHash, &hashContent);

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 10);

	hitIdx = -1;
	isHit = false;
	isL2Hit = false;
	if (keyOk && navmesh::g_nmCache.nmDiagStage >= 2 && !InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheDisabled, 0, 0))
	{
		NmCacheLock cacheLock;

		hitIdx = FindCacheEntry(cacheLock, key);
		isHit = (hitIdx >= 0 && navmesh::g_nmL1.nmCache[hitIdx].cachedFaces != NULL && game::g_gameFn.fn_navMeshCtor != NULL);

		if (!isHit && game::g_gameFn.fn_navMeshCtor != NULL)
		{
			cacheLock.Release();

			LARGE_INTEGER tR0, tR1;
			QueryPerformanceCounter(&tR0);

			NavMeshCacheEntry diskEntry;
			memset(&diskEntry, 0, sizeof(diskEntry));
			bool l2Read = ReadDiskCache(key, diskEntry);
			if (l2Read)
			{
				NmCacheLock promoteLock;
				hitIdx = PromoteDiskEntryToL1(promoteLock, diskEntry);
				promoteLock.Release();

				if (hitIdx >= 0)
				{
					isHit = true;
					isL2Hit = true;
					InterlockedIncrement(&navmesh::g_nmCache.nmDiskHitCount);
				}
				else
				{
					l2Read = false;   // promotion refused it: treat as a miss
				}
			}
			if (!l2Read)
			{
				InterlockedIncrement(&navmesh::g_nmCache.nmDiskMissCount);

				long idx = InterlockedIncrement(&navmesh::g_nmCache.l2MissLogCount) - 1;
				if (idx < L2_MISS_LOG_MAX)
				{
					navmesh::g_nmCache.l2MissLog[idx].gridX = key.gridX;
					navmesh::g_nmCache.l2MissLog[idx].gridY = key.gridY;
					navmesh::g_nmCache.l2MissLog[idx].tileId = key.sectionTileId;
					navmesh::g_nmCache.l2MissLog[idx].jobType = key.jobType;
					navmesh::g_nmCache.l2MissLog[idx].aabbHash = key.aabbHash;
					navmesh::g_nmCache.l2MissLog[idx].buildingHash = key.buildingHash;
					// Claim time, before missLock: the content may be gone.
					// Guarded read, -1 when it is.
					navmesh::g_nmCache.l2MissLog[idx].thingsCount = SafeZoneThingsCount(jobZone);
				}
			}

			QueryPerformanceCounter(&tR1);
			long readUs = (long)(QPCToMs(tR0, tR1) * 1000.0);
			InterlockedExchangeAdd(&navmesh::g_nmCache.nmDiskReadUsTimes1, readUs);
		}
		else
		{
			if (isHit)
			{
				void* freshNavMesh = ReconstructExpected(cacheLock, key, hitIdx, NULL);
				cacheLock.Release();

				if (freshNavMesh)
				{
					*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_mesh, 72)) = freshNavMesh;
					*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 80)) = NULL;
				}
				else
				{
					isHit = false;
				}
			}
			else
			{
				cacheLock.Release();
			}
		}
	}

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 11);
}

void PjCtx::OwnHit()
{
	if (isHit)
	{
		if (!isL2Hit)
			InterlockedIncrement(&navmesh::g_nmCache.nmCacheHitCount);
		InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 20);
		InterlockedExchange(&navmesh::g_nmCache.nmDiagHitGrid, gridX * 100 + gridY);

		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);

		if (isL2Hit)
		{
			// The promotion released nmCacheCS, so the ring may have published
			// over the slot since; a replaced slot regenerates, the job untouched.
			NmCacheLock cacheLock;
			void* freshNavMesh = ReconstructExpected(cacheLock, key, hitIdx, NULL);
			cacheLock.Release();

			if (freshNavMesh)
			{
				*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_mesh, 72)) = freshNavMesh;
				*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 80)) = NULL;
			}
			else
			{
				isHit = false;
			}
		}

		if (isHit)
		{
			InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 23);
			int resetReason = STALE_REASON_NONE;
			if (ResetWaitAt(ZONE_RESET_SITE_BUILD, job, claimed->resetRaises, &resetReason))
			{
				NoteStaleDrop(STALE_SITE_HIT, job, jobType, resetReason, claimQpc);
				resetDrop = true;
			}
			else
			{
				NoteWorkerPhase(WPHASE_BUILDING);
				game::g_gameFn.fn_buildCollision(realNMG, (void*)job, 0, 0.0);
			}
			InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 24);
		}

		QueryPerformanceCounter(&t1);
		if (isL2Hit)
		{
			long reconUs = (long)(QPCToMs(t0, t1) * 1000.0);
			InterlockedExchangeAdd(&navmesh::g_nmCache.nmDiskReadUsTimes1, reconUs);
		}
		else if (isHit)
		{
			long ms10 = (long)(QPCToMs(t0, t1) * 10.0);
			InterlockedExchangeAdd(&navmesh::g_nmCache.nmSavedMsTimes10, ms10);
		}
	}
}

void PjCtx::MissSetup()
{
	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 30);

			QueryPerformanceCounter(&t0);

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 31);

	// The workBuffer fn_processJobAlt will see.
	//   workNMG!=realNMG → CloneNMG already installed freshWB at workNMG+256,
	//     built there under processJobCS.
	//   workNMG==realNMG → build freshWB below, inside the lock, and swap
	//     realNMG+256 for the duration of processJobAlt. Restore before
	//     LeaveCS so buildCollision sees the original.
	localFreshWB = NULL;
	localOrigWB = 0;
	usingNMGClone = (workNMG != realNMG);
}

bool PjCtx::CheckMissAfterLock(ProcessJobLock& missLock)
{
		// Authoritative zone re-check, after the last processJobCS wait and
		// before anything below reads the zone or touches realNMG+256: the
		// late-HIT check, ConstructFreshSettings, the swap, processJobAlt (which
		// reads *(zone+0xB8)+8 unconditionally — a crash at rva 0x3C1600 when a
		// worker waits here long enough for the game to unload the zone).
		// claimAge miss= is measured here, for every
		// job that reaches this point, dropped or not.
		NoteClaimAge(true, claimQpc, missLockAt);
		{
			int staleReason = STALE_REASON_NONE;
			// The last re-check (the worker's third): NavMesh::stop
			// seen during the wait, or just after the lock was won. The job
			// takes this same exit with reason shutdown, before anything below
			// runs: no late HIT, no fresh work buffer, no processJobAlt.
			bool stopDrop = !missLock.held || NavMeshStopRequested();
			if (stopDrop)
				staleReason = STALE_REASON_SHUTDOWN;
			if (stopDrop || !JobZoneStillLoaded(job, &staleReason))
			{
				// Exit 4 in the header list.
				// The job is left exactly as dispatchJob_orig's early return
				// leaves it: not freed, not enqueued, +72/+80 untouched. Nothing was
				// generated, so nothing is counted as a miss (miss=, miss=w/bg,
				// avgMiss are unaffected) and no disk write is pending. A no-op
				// when the stop-aware wait bailed without the lock.
				missLock.Release();
				if (stopDrop)
					NoteStopDrop(onBgThread ? STALE_SITE_BGMISS : STALE_SITE_WMISS,
					             job, jobType, claimQpc);
				else
				NoteStaleDrop(onBgThread ? STALE_SITE_BGMISS : STALE_SITE_WMISS,
				              job, jobType, staleReason, claimQpc);
				InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 36);   // 36 = dropped (zone unloaded, or the stop)

				// The bridge is released by the caller's ClaimedJobFinish.
				InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 50);
				return true;   // the worker caller still frees its clone
			}
		}
	return false;
}

void PjCtx::LateLookup()
{
	// Duplicate-job check. The game submits the same zone more than once
	// (it re-registers sections; Step2A-fixed 2026-09-10: a worker and then
	// the bg thread both generated (21,41), 14 s + 17 s back-to-back while
	// the game waited to exit). If another thread finished this exact key
	// while we waited for processJobCS, L1 has it (stored under the lock):
	// take the HIT instead of generating again.
	NoteWorkerPhase(WPHASE_GENERATING);
	lateMesh = NULL;
	if (keyOk && navmesh::g_nmCache.nmDiagStage >= 2 && game::g_gameFn.fn_navMeshCtor != NULL
	    && !InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheDisabled, 0, 0))
	{
		NmCacheLock cacheLock;
		int lateIdx = FindCacheEntry(cacheLock, key);
		if (lateIdx >= 0)
			lateMesh = ReconstructExpected(cacheLock, key, lateIdx, NULL);
		cacheLock.Release();
	}
}

void PjCtx::LateBuild(ProcessJobLock& missLock, InflightScope& inflight)
{
	missLock.Release();
	inflight.Release();

	*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_mesh, 72)) = lateMesh;
	*(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 80)) = NULL;
	InterlockedIncrement(&navmesh::g_nmCache.nmCacheHitCount);
	InterlockedIncrement(&navmesh::g_nmCache.nmLateHitCount);
	{
		std::ostringstream ss;
		ss << "Late HIT (duplicate job): grid=(" << gridX << "," << gridY
		   << ") type=" << jobType << (usingNMGClone ? " [worker]" : " [bg]");
		LogMsg(ss.str());
	}

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 23);
	int resetReason = STALE_REASON_NONE;
	if (ResetWaitAt(ZONE_RESET_SITE_BUILD, job, claimed->resetRaises, &resetReason))
	{
		NoteStaleDrop(onBgThread ? STALE_SITE_BGMISS : STALE_SITE_WMISS,
		              job, jobType, resetReason, claimQpc);
		resetDrop = true;
	}
	else
	{
		NoteWorkerPhase(WPHASE_BUILDING);
		game::g_gameFn.fn_buildCollision(realNMG, (void*)job, 0, 0.0);
	}
	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 24);

	QueryPerformanceCounter(&t1);
	long ms10 = (long)(QPCToMs(t0, t1) * 10.0);
	InterlockedExchangeAdd(&navmesh::g_nmCache.nmSavedMsTimes10, ms10);
}

void PjCtx::GenConstruct()
{
	InterlockedIncrement(&navmesh::g_nmCache.nmCacheMissCount);
	// miss=w/bg splits by workNMG (a clone or the real generator), while
	// stale=w/bg and pjWait wMiss/bgMiss split by thread (onBgThread). A
	// worker whose CloneNMG failed runs on the real generator, so it
	// counts as bg here and as w there: do not compare the two tokens
	// as like for like.
	if (workNMG != realNMG)
		InterlockedIncrement(&navmesh::g_nmCache.nmWorkerMissCount);
	else
		InterlockedIncrement(&navmesh::g_nmCache.nmBgMissCount);

	// The real work buffer is read only under processJobCS, so the
	// fresh copy is built here rather than before the Enter above.
	// Doing it after the late-HIT check also means a duplicate job no
	// longer allocates a work buffer just to free it again. The worker
	// path built its copy inside CloneNMG, under this same lock.
	if (!usingNMGClone)
	{
		localOrigWB = *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)realNMG, NavMeshGenerator_settings, 256));
		localFreshWB = ConstructFreshSettings(localOrigWB);
		if (localFreshWB)
			InterlockedIncrement(&navmesh::g_nmCache.nmCloneConstructCount);
		else
			InterlockedIncrement(&navmesh::g_nmCache.nmCloneConstructFailCount);
	}
	cloneActive = usingNMGClone || (localFreshWB != NULL);
}

void PjCtx::GenPrepare(WbSwapRestore& swapRestore)
{
	{
		if (usingNMGClone)
		{
			std::ostringstream ss;
			ss << "MISS NMG-clone: realNMG=" << realNMG
			   << " workNMG=" << workNMG
			   << " workWB=" << *(void**)(KLIB_MEMBER(4, (uintptr_t)workNMG, NavMeshGenerator_settings, 256))
			   << " grid=(" << gridX << "," << gridY << ") type=" << jobType;
			LogMsg(ss.str());
		}
		else if (localFreshWB)
		{
			uintptr_t* wbSlot = (uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)realNMG, NavMeshGenerator_settings, 256));
			swapRestore.Arm(wbSlot, localOrigWB);
			*wbSlot = (uintptr_t)localFreshWB;

			std::ostringstream ss;
			ss << "MISS swap: realNMG=" << realNMG
			   << " origWB=" << (void*)localOrigWB << " freshWB=" << localFreshWB
			   << " grid=(" << gridX << "," << gridY << ") type=" << jobType;
			LogMsg(ss.str());
		}
		else
		{
			LogMsg("MISS no-swap: using real WB (ConstructFreshSettings failed)");
		}

		EnsureGlobalScratchBuffer();
	}

	LogMsg("MISS: entering processJobAlt");

	// Clear before the run so the store site cannot read the previous
	// job's counts if this one never reaches populate.
	t_lastInputTriCount  = -1;
	t_lastInputVertCount = -1;

	// The geometry certificate is captured here, ahead of every read
	// of the world's geometry this generation will make: the queries
	// are inside the call below, and so is the point where a clone
	// releases processJobCS. Capturing before the call therefore
	// covers the released window as well as the held one, whichever
	// thread runs it and however many generations are in flight.
	ZoneGeometryCaptureForJob(gridX, gridY);

	// The clone-guard is no longer armed. The two entries it
	// used to skip per MISS are fully built by then, and finalizeDeep
	// stops at the zeroed guard slot, so letting edgeProcess run its
	// normal cleanup brings the leaked volume reference back to
	// vanilla's one. The hook and its counters stay for validation:
	// edge= should read a0/uN with N = 2 per MISS.
	(void)cloneActive;

	// The neighbour-seed hook records this type-0
	// generation's four directions into the thread's record.
	NbrSeedJobBegin(jobType, gridX, gridY);

	mpj.jobAltStart = QpcNow();
}

bool PjCtx::AfterProcessJobAlt(bool pjLockLost, ProcessJobLock& missLock)
{
			if (pjLockLost)
			{
				// NavMesh::stop was seen while this worker waited to re-acquire
				// processJobCS after its generation: it does not hold the lock
				// and starts nothing more (no splice, no L1 store, no
				// buildCollision, no enqueue). processJobAlt's tail ran under the
				// cleanup handshake begun in NavMeshReenterAfterGenerate, ended
				// here. The generated mesh stays on the job, which is left
				// unlinked, not freed and not enqueued like every other stop
				// drop; its Havok blocks go with the heap NavMesh::stop releases.
				WorkerCleanupEnd();
				missLock.Abandon();
				NoteStopDrop(STALE_SITE_WMISS, job, jobType, claimQpc);
				InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 36);
				InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 50);
				return true;   // the worker caller frees its clone through the handshake
			}


#ifdef ZONEOPT_DEBUG
			// finalizeDeep has run inside processJobAlt and popped the single
			// entry the job appended. What is left should be exactly the four
			// material overrides; anything else means the pop loop walked past
			// them or the append count changed.
			{
				uintptr_t probeWB = localFreshWB
				                  ? (uintptr_t)localFreshWB
				                  : (usingNMGClone ? *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)workNMG, NavMeshGenerator_settings, 256)) : 0);
				if (probeWB)
					InterlockedExchange(&navmesh::g_nmCache.g_wbOverrideAfterPop, *(int*)(KLIB_MEMBER(4, probeWB + 520, ByteArray_m_size, 8)));
			}
#endif

			InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 32);

			if (jobType == 1 && PartialZoneStillLoaded(job))
			{
				// partialGeneration (0x3CA290) needs the real generator, not a
				// clone. It reads three `this` fields and every one of them is
				// wrong on a clone:
				//   +184  the processing queue, walked by lookupSection
				//         (0x3C75D0) to find this tile's section. CloneNMG
				//         zeroes it, so the walk always misses and the call
				//         silently drops to the +240 fallback scan.
				//   +240  the section manager, memcpy'd from the real NMG, so
				//         the fallback scan does reach real data — but pinned
				//         and read under the wrong lock.
				//   +272  the boost::shared_mutex that lookupSection takes
				//         around setting the pin bit, and that partialGeneration
				//         takes again at its tail. fn_queueLockInit re-created
				//         the clone's, so it serializes against nothing.
				// This call is inside processJobCS (entered before the
				// duplicate-job check above, left after the L1 store below), so
				// passing realNMG is safe against the bg thread and the other
				// workers.
				if (!BuildLockNarrowActive())
				{
					BuildCollisionScope guard;
					game::g_gameFn.fn_partialFixup(realNMG, (void*)job);
				}
				else
					game::g_gameFn.fn_partialFixup(realNMG, (void*)job);   // stitches two job-private meshes only
				InterlockedIncrement(&navmesh::g_nmCache.nmPartialRealCount);
			}
			mpj.fixEnd = QpcNow();
	return false;
}

void PjCtx::StoreGenerated(WbSwapRestore& swapRestore, ProcessJobLock& missLock, InflightScope& inflight)
{

	// Swap-path: restore realNMG+256 before releasing processJobCS so
	// buildCollision and later code see the original. Through the guard,
	// which then has nothing left to do (a no-op off the swap path).
	swapRestore.Restore();
	NoteWorkerPhase(WPHASE_STORING);

	// L1 store while still holding processJobCS: a duplicate job waiting
	// on the lock then finds the result (late HIT) instead of generating
	// it again. The cached copy is the pre-buildCollision mesh, which is
	// exactly what the HIT path feeds into buildCollision.
	uintptr_t storedResult = 0;
	int storeIdx = -1;
	if (navmesh::g_nmCache.nmDiagStage >= 2 && !InterlockedCompareExchange(&navmesh::g_nmCache.nmCacheDisabled, 0, 0))
	{
		storedResult = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_mesh, 72));
		if (storedResult)
		{
			// A generation that came back with no faces: either the tile
			// is genuinely empty or the run aborted (a Havok keycode,
			// out of memory). The populate hook left this job's input
			// triangle count in thread-local storage, which tells the
			// two apart. StoreCacheEntry refuses the mesh,
			// and with it the disk blob below, which is only built from
			// a published slot.
			if (hkArrayGetCount(storedResult, NMOFF_FACES) == 0)
			{
				// processJobCS does not keep the game's own unload out,
				// and a generation takes 1-2 s, so this read is guarded
				// like the claim-time ones.
				NoteZeroFaceMesh(key, t_lastInputTriCount, t_lastInputVertCount,
				                 SafeZoneThingsCount(jobZone));
			}

			// Publish only a key whose hash checked
			// out, and only while the zone still holds the content the
			// hash was computed from. An unload (or unload and reload)
			// that began after the walk shows up here as a different
			// or NULL zone+0, and the mesh is then used for this job
			// only, never cached. keyOk false was counted at the hash.
			// The certificate's other end. It is checked on every
			// store, and its verdict counted, whatever the mode: the
			// rate and the reason split are the measurement that has
			// to come back from a session before publication could
			// ever depend on it. Under the mode every build ships
			// with, the answer is always false and the store below
			// decides exactly what it decided before.
			bool certRefused = ZoneGeometryStoreRefused(gridX, gridY);

			bool storeOk = keyOk && ZoneContentUnchanged(jobZone, hashContent)
			               && !certRefused;
			if (keyOk && !storeOk && !certRefused)
				InterlockedIncrement(&navmesh::g_nmCache.nmHashRaceCount);
			if (storeOk)
			{
				NmCacheLock storeLock;
				storeIdx = StoreCacheEntry(storeLock, key, storedResult);
				if (storeIdx >= 0)
					MissParNoteHash(navmesh::g_nmL1.nmCache[storeIdx], !onBgThread);
				storeLock.Release();
			}
		}
	}

	missLock.Release();
	mpj.released = QpcNow();
	// The result is in L1 (or was refused); a waiter's late-HIT check
	// under processJobCS now sees the same thing a later job would.
	inflight.Release();

	// Serialize the L1 deep copy now, write the file after the
	// result is enqueued. The bytes come from the cache entry, not from
	// the game's arrays, which buildCollision is about to touch.
	//
	// A generation that needed a
	// stand-in and found its record late has a pruned mesh the key
	// cannot tell from a complete one, so it is not written to L2 (it
	// regenerates next session). The L1 entry above stays: per session,
	// and the late-HIT re-check depends on it. No blob is built, which
	// is the same no-write path an L1-refused store takes; the claim,
	// busy and late-HIT bookkeeping are untouched. Counted l2LateSkip=.
	if (NbrSeedJobTakeLate() && storeIdx >= 0)
	{
		InterlockedIncrement(&navmesh::g_nmCache.nmNbrL2LateSkip);
		storeIdx = -1;
	}
	if (storeIdx >= 0)
	{
		NmCacheLock blobLock;
		if (CacheSlotMatches(navmesh::g_nmL1.nmCache[storeIdx], key))
			BuildDiskCacheBlob(key, navmesh::g_nmL1.nmCache[storeIdx], pendingWrite);
		blobLock.Release();
	}
}

void PjCtx::BuildGenerated()
{
	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 33);

	// A reset drop still records the job, with a build phase of 0, and
	// still frees the fresh WB and writes L2 below: the L1 entry was
	// stored while the zone held the content its key was hashed from.
	int resetReason = STALE_REASON_NONE;
	if (ResetWaitAt(ZONE_RESET_SITE_BUILD, job, claimed->resetRaises, &resetReason))
	{
		NoteStaleDrop(onBgThread ? STALE_SITE_BGMISS : STALE_SITE_WMISS, job, jobType, resetReason, claimQpc);
		resetDrop = true;
		mpj.bcStart = mpj.bcEnd = QpcNow();
	}
	else
	{
		NoteWorkerPhase(WPHASE_BUILDING);
		mpj.bcStart = QpcNow();
		game::g_gameFn.fn_buildCollision(realNMG, (void*)job, 0, 0.0);
		mpj.bcEnd = QpcNow();
	}
	MissParRecordJob(mpj);

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 34);

	QueryPerformanceCounter(&t1);
	long ms10 = (long)(QPCToMs(t0, t1) * 10.0);
	InterlockedExchangeAdd(&navmesh::g_nmCache.nmTotalMsTimes10, ms10);

	// Free the swap-path freshWB. FreeFreshSettings runs the
	// game's settings dtor body on it, so the arrays processJobAlt grew
	// are released here.
	if (localFreshWB)
		FreeFreshSettings(localFreshWB);
}

void PjCtx::Handoff()
{
	void* label29NavInst = *(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 80));
	if (label29NavInst)
		*(int*)(KLIB_MEMBER(4, (uintptr_t)label29NavInst, NavInstance_hash, 64)) = *(int*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_hash, 32));

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 41);

	uintptr_t navMeshResult = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_mesh, 72));
	int faceCount = navMeshResult ? *(int*)(KLIB_MEMBER(4, navMeshResult + NMOFF_FACES, ByteArray_m_size, 8)) : 0;
	if (!resetDrop && navMeshResult && faceCount > 0)
	{
		InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 42);
		game::g_gameFn.fn_enqueueToProcQueue((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_done, 184)), (void*)job);
	}
	else
	{
		InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 43);
		void* delNavInst = *(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_output, 80));
		if (delNavInst)
			game::g_gameFn.fn_gameDelete(delNavInst);

		void* buildingRef = *(void**)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_buildings_stuff, 24));
		if (buildingRef)
			game::g_gameFn.fn_gameDelArr(buildingRef);

		game::g_gameFn.fn_gameDelete((void*)job);
	}

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 44);

	// The file write is the last thing this job does. The game already has
	// the mesh, so a slow disk no longer delays the result.
	ClaimedJobWritePending(claimed);

	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 50);
}

} // namespace nm_job_pipeline_detail

using namespace nm_job_pipeline_detail;

void ProcessNavMeshJob(void* realNMG, void* workNMG, ClaimedJob* claimed)
{
	nm_job_pipeline_detail::PjCtx c;
	c.realNMG = realNMG;
	c.workNMG = workNMG;
	c.job = claimed->job;
	c.jobType = claimed->jobType;
	c.claimQpc = claimed->claimQpc;
	c.pendingWrite = &claimed->pendingWrite;
	c.claimed = claimed;
	if (!c.Begin()) return;
	c.Lookup();
	c.OwnHit();
	if (!c.isHit)
	{
		c.MissSetup();
		// Held from here to one of two explicit Release() calls, both inside
		// this block: at the top of the late-HIT branch, and after the L1 store
		// on the generate branch (the late-HIT invariant needs the store inside
		// the lock). Neither branch calls buildCollision with it held. The
		// guard's destructor only acts if something unwinds out of the block
		// before a Release(). On that path the lock is freed (before, it stayed
		// held and hung every NavMesh thread), and the swap-path work buffer
		// install at realNMG+256 is undone first by WbSwapRestore, declared
		// after this guard in the generate branch.
		//
		// pjWait wMiss= / bgMiss=: QPC either side of the guard's construction,
		// so the lock is taken exactly where it was and the owner-tid
		// bookkeeping in EnterProcessJobCS is untouched.
		// The stand-in prefetch, on the MISS path only
		// (every L1/L2 lookup above failed) and before missLock, so it never
		// runs under processJobCS. The worker has already released the lock
		// CloneNMG took; the bg thread released the queue lock and the
		// first-dispatch latch in hook_dispatchJob. Type 0 only: the seeds are
		// read only by generateTaskBT's type-0 branch. Phase "claimed" while it
		// loads (a retire slice line with a worker there names it).
		if (c.jobType == 0)
		{
			NoteWorkerPhase(WPHASE_CLAIMED);
			NbrSeedPrefetch((uintptr_t)realNMG, c.jobZone);
		}
		// Every L1/L2 lookup missed. If another thread is generating this key
		// right now (with processJobCS released around its realGenerate), wait
		// for it here, holding no lock, so the late-HIT check below finds its
		// L1 store instead of generating the key twice. Declared before
		// missLock and released only after missLock is, on every exit.
		InflightScope inflight;
		if (c.keyOk)
			inflight.Register(c.key);

		memset(&c.mpj, 0, sizeof(c.mpj));
		c.mpj.worker = !c.onBgThread;
		c.missWaitStart = QpcNow();
		c.mpj.waitStart = c.missWaitStart;
		c.mpj.holderAtWait = MissParHolderGet();
		NoteWorkerPhase(WPHASE_WAIT_MISS);
		// Same back-off as CloneNMG's, on the workers
		// and the bg thread alike; neither holds a lock here.
		BackOffForPjPoll();
		// A stop-aware wait, on the workers and the bg thread alike
		// (the bg thread is where "no new MISS after NavMesh::stop" is enforced
		// for the mod's own type 0/1 jobs). missLock.held is false when the stop
		// was seen before the lock was won; the exit below handles it. The whole
		// loop counts as wait in pjWait wMiss= / bgMiss=, the bail included.
		ProcessJobLock missLock(PJ_STOP_AWARE);
		c.missLockAt = QpcNow();
		c.mpj.locked = c.missLockAt;
		if (missLock.held)
			MissParHolderSet(c.onBgThread ? MP_HOLD_BGMISS : MP_HOLD_WMISS);
		NotePjWait(c.onBgThread ? PJWAIT_BGMISS : PJWAIT_WMISS, c.missWaitStart, c.missLockAt);
		if (c.CheckMissAfterLock(missLock)) return;
		c.LateLookup();
		if (c.lateMesh)
		{
			c.LateBuild(missLock, inflight);
		}
		else
		{
			c.GenConstruct();
			// Declared after missLock (outer block), destroyed before it: see
			// WbSwapRestore. Armed only on the swap path below.
			WbSwapRestore swapRestore;
			c.GenPrepare(swapRestore);
			bool pjLockLost = false;
			{
				// Only a clone's run may release processJobCS inside populate;
				// processJobAlt is still entered, and normally left, under it.
				MissParArmScope arm(c.usingNMGClone, c.realNMG);
				game::g_gameFn.fn_processJobAlt(c.workNMG, (void*)c.job);
				pjLockLost = MissParLockLost();
			}
			c.mpj.jobAltEnd = QpcNow();

			NbrSeedJobEnd();
			if (c.AfterProcessJobAlt(pjLockLost, missLock)) return;
			c.StoreGenerated(swapRestore, missLock, inflight);
			c.BuildGenerated();
		}
		InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 35);
	}
	InterlockedExchange(&navmesh::g_nmCache.nmDiagStep, 40);
	NoteWorkerPhase(WPHASE_STORING);   // the hand-off below, then the L2 write
	c.Handoff();
}


// --------------------------------------------------------------------
// Hooks
// --------------------------------------------------------------------

void hook_realGenerate(void* workBuffer, void* localData, void* hkaiNavMesh, int param, int timeLowPart)
{
	// MinHook can't relocate the first instructions of the 61K-byte realGenerate,
	// so this hook is never actually installed. Body kept as a passthrough for
	// the function-pointer slot.
	game::g_hookOrig.orig_realGenerate(workBuffer, localData, hkaiNavMesh, param, timeLowPart);
}

// Pass-through over the 49-byte wrapper that calls realGenerate. Its second
// argument is the input geometry processJobAlt built for this job — the only
// place the generator's input size is visible, because the geometry is a stack
// local of processJobAlt and nothing copies it onto the job or the result.
//
// The counts go into thread-local storage: the job's store site runs on the
// same thread, later in the same call, so it can read them back without any
// synchronization. A job that never reaches populate leaves the previous
// values, which is why ProcessNavMeshJob clears them before processJobAlt.
void hook_nmResultPopulate_diag(void* navData, void* localData, void* result, int param)
{
	if (localData)
	{
		t_lastInputTriCount  = *(int*)(KLIB_MEMBER(4, (uintptr_t)localData + 32, ByteArray_m_size, 8));
		t_lastInputVertCount = *(int*)(KLIB_MEMBER(4, (uintptr_t)localData + 16, ByteArray_m_size, 8));
	}
	MissParPopulate(game::g_hookOrig.orig_nmResultPopulate, navData, localData, result, param);
}
