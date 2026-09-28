// nm_worker_pool.cpp - worker pool, wake and busy bridge; worker threads and the NavMesh bg thread.
// Havok init and retire handshake; queue +152 owns busy count and +265 transitions.

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
// --------------------------------------------------------------------
// Worker pool state
// --------------------------------------------------------------------

static HANDLE          g_workerHandles[NAVMESH_WORKER_COUNT] = {};
volatile long   g_workerShutdown     = 0;
HANDLE          g_jobEvent           = NULL;
uintptr_t       g_navMeshGen         = 0;
// Set by a worker whose Havok registration failed, before it clears its handle
// slot. The flag, not the handle, is the race-free signal: CreateThread may not
// have stored the handle yet when the worker gives up.
static volatile long   g_workerInitFailed[NAVMESH_WORKER_COUNT] = {};

HANDLE WorkerSlotHandle(int i)
{
	if (i < 0 || i >= NAVMESH_WORKER_COUNT) return NULL;
	if (InterlockedCompareExchange(&g_workerInitFailed[i], 0, 0)) return NULL;
	HANDLE h = g_workerHandles[i];
	if (h == INVALID_HANDLE_VALUE) return NULL;
	return h;
}


// --------------------------------------------------------------------
// No MISS after NavMesh::stop
// --------------------------------------------------------------------
//
// NavMesh::shutdown deletes the generator and releases the whole Havok heap
// (hkMemoryInitUtil::quit), and a MISS can wait many seconds between its claim
// and its generation. The rule: once NavMesh::stop is seen, no worker starts a
// MISS, and a worker touches Havok memory (or the generator) after seeing it
// only to release what it already holds.
//   - Every processJobCS wait on the MISS path (CloneNMG's and missLock, on the
//     workers and the bg thread) is EnterProcessJobCSStopAware: a TryEnter loop
//     that re-checks the stop between tries and returns without the lock once
//     it is seen. No MISS thread blocks on processJobCS.
//   - The worker re-checks after the claim, after CloneNMG and after missLock,
//     and the bg thread after missLock, each dropping the job the way a stale
//     drop does (reason shutdown, counted as stopDrop= on the retire line).
//   - The retire returns only once no worker is live, so every worker release
//     ends before the teardown. The releases (the clone, the busy bridge, the
//     Havok thread cleanup) go through WorkerCleanupBegin /
//     WorkerCleanupEnd, a backstop handshake: after the retire has returned a
//     release is skipped rather than run on a freed heap.
// Lock order: processJobCS before nmCacheCS and buildCollisionCS.


namespace nm_workers_detail {
// The retire's backstop handshake (above). g_retireReturned is set by
// RetireNavMeshWorkers once no worker is live, before its drain and its line.
volatile long g_retireReturned        = 0;
volatile long g_workerCleanupInFlight = 0;

} // namespace nm_workers_detail

namespace nm_workers_detail {
// A worker (or the bg thread's neighbour-seed load), before touching Havok
// memory or the generator after the stop may have been seen. True: go ahead,
// then call WorkerCleanupEnd. False: the retire has returned, so skip it; a
// worker never reads false, since its last release ends before its live count
// drops and the retire returns only at a live count of 0. The increment and
// the read are both full barriers,
// as are the retire's set and its read of the count, so either this thread sees
// g_retireReturned or the retire sees the count: a release never overlaps the
// teardown that follows a returned retire.
bool WorkerCleanupBegin()
{
	InterlockedIncrement(&g_workerCleanupInFlight);
	if (InterlockedCompareExchange(&g_retireReturned, 0, 0))
	{
		InterlockedDecrement(&g_workerCleanupInFlight);
		return false;
	}
	return true;
}
} // namespace nm_workers_detail


namespace nm_workers_detail {
// Each worker's phase (NavMeshWorkerPhase, core.h): printed per live worker on
// every retire slice line and the final record, and read by the crash handler
// through the worker's own t_navMeshWorkerPhase, which points at its slot here.
volatile LONG g_workerPhase[NAVMESH_WORKER_COUNT] = {};

} // namespace nm_workers_detail
// --------------------------------------------------------------------
// Worker dequeue
// --------------------------------------------------------------------
//
// A worker claims its job before it looks the cache up. Under the generator's
// queue lock (+152) WorkerTryDequeueAny unlinks exactly one type 0/1 job,
// raises the busy bridge and writes its claimed-zone slot, then releases the
// lock; only after that does it build the key and take nmCacheCS, so the two
// locks never nest. processJobCS, when the job is a MISS, is taken later still,
// with neither held.

// A job a worker skipped may be claimable now. Any thread, no lock held.
// After an adjacency release an unlocked peek is enough: the release ran under
// +152, and a worker that cleared the event did so under +152 before it. The
// reset gate's lower is taken under +152 too, so a worker that read the gate
// up clears the event before the lower, and this set follows that lower.
void NavMeshWakeWorkersIfQueued()
{
	uintptr_t nmg = g_navMeshGen;
	if (nmg && *(volatile uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)))
		SignalJobAvailable();
}

// The generator's "busy" bridge. NavMeshGenerator::isBusy (0x3BF360) inspects
// only +232 (the current work item) and +136 (the input queue), so a job this
// mod has unlinked but not yet finished is invisible to it: the generator looks
// idle for a zone that is mid-regeneration, and isContentPending's
// generator-idle fallback can answer "ready" for a zone with no mesh. +265 is
// the byte the mod keeps for that, so it has to be raised at claim time and
// held until the job is completely done — across the L2 read, which is 31-46 ms
// on its own. WorkerBusyEnter runs inside the claim's own queue-lock (+152)
// region, the one that unlinks the job; WorkerBusyLeave takes +152 itself and
// is called holding no lock (the worker loop's release and the bg thread's
// two). The bridge takes nothing under +152.
// The generator this thread raised the bridge on, so the release writes the
// same byte the claim did rather than re-reading a global that a future change
// could move underneath it.
namespace nm_workers_detail {
static __declspec(thread) uintptr_t t_busyNmg = 0;

// Caller holds the generator queue lock +152 and releases it after publishing the claim.
void WorkerBusyEnter(uintptr_t nmg)
{
	t_busyNmg = nmg;
	NoteBusyBridge(BusyBridge(GameBusyBridgeOps(nmg), BUSY_BRIDGE_ENTER, true));
}

// Called with no lock held; BusyBridgeLeave takes the generator queue lock +152 itself.
void WorkerBusyLeave()
{
	uintptr_t nmg = t_busyNmg;
	t_busyNmg = 0;
	if (!nmg)
	{
		InterlockedDecrement(&navmesh::g_nmCache.workerBusyCount);   // no generator: nothing to lock or clear
		return;
	}
	NoteBusyBridge(BusyBridgeLeave(GameBusyBridgeOps(nmg)));
}
} // namespace nm_workers_detail

namespace nm_workers_detail
{

// The bridge on the real generator: its queue lock, taken the way every other
// +152 site takes it, and the two words.
void BridgeLockQueue(void* nmg)
{
	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, (uintptr_t)nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
}

void BridgeUnlockQueue(void* nmg)
{
	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, (uintptr_t)nmg, NavMeshGenerator_queue_mutex, 152)));
}

static long BridgeIncrement(void*) { return InterlockedIncrement(&navmesh::g_nmCache.workerBusyCount); }
static long BridgeDecrement(void*) { return InterlockedDecrement(&navmesh::g_nmCache.workerBusyCount); }
static long BridgeReadCount(void*) { return InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0); }

static unsigned char BridgeReadFlag(void* nmg)
{
	return *(volatile unsigned char*)(KLIB_MEMBER(4, (uintptr_t)nmg, NavMeshGenerator_doingStuff, 265));
}

static void BridgeWriteFlag(void* nmg, unsigned char value)
{
	*(volatile unsigned char*)(KLIB_MEMBER(4, (uintptr_t)nmg, NavMeshGenerator_doingStuff, 265)) = value;
}

BusyBridgeOps GameBusyBridgeOps(uintptr_t nmg)
{
	BusyBridgeOps ops;
	ops.ctx         = (void*)nmg;
	ops.lockQueue   = BridgeLockQueue;
	ops.unlockQueue = BridgeUnlockQueue;
	ops.increment   = BridgeIncrement;
	ops.decrement   = BridgeDecrement;
	ops.readCount   = BridgeReadCount;
	ops.readFlag    = BridgeReadFlag;
	ops.writeFlag   = BridgeWriteFlag;
	return ops;
}

void NoteBusyBridge(bool held)
{
	if (!held)
		InterlockedIncrement(&navmesh::g_nmCache.nmBusyBridgeViolCount);
}



} // namespace nm_workers_detail
using namespace nm_workers_detail;


namespace nm_workers_detail {
// The bg thread's backstop for a byte the original left at 1 with nothing
// claimed: it clears the byte when the count reads 0. It takes the generator's
// queue lock (+152) only when the byte reads 1, so an idle poll costs no lock,
// and takes nothing under it. lockHeld: the caller already holds +152.
void ClearBusyBridgeIfIdle(uintptr_t nmg, bool lockHeld)
{
	NoteBusyBridge(BusyBridge(GameBusyBridgeOps(nmg), BUSY_BRIDGE_CLEAR_IF_IDLE, lockHeld));
}
} // namespace nm_workers_detail

// --------------------------------------------------------------------
// Worker thread entry
// --------------------------------------------------------------------

namespace nm_workers_detail {
static bool WorkerInitHavok(int workerId, char (&ctx128)[128], char (&buf8)[8], char (&name)[32])
{
	// Havok thread init (5-step sequence, matches Kenshi's 4 game threads):
	//   contextInit → getManager → manager->vt+24(ctx, name, 3)
	//     → postRegInit → _mm_setcsr denormal flush
	// Step 3 populates both Havok TLS slots — workers can't call the
	// router-dependent allocator without it.
	memset(ctx128, 0, sizeof(ctx128));
	game::g_gameFn.fn_havokContextInit(ctx128);

	void* mgr = game::g_gameFn.fn_havokGetManager(0);
	if (!mgr)
	{
		LogMsg("Worker: HavokGetManager returned NULL, aborting");
		return false;
	}

	uintptr_t mgrVtable = *(uintptr_t*)mgr;
	// threadInit (0xBCA8D0) is
	//   void* __fastcall(void* memSystem, void** router, const char* name, char flags)
	// Four arguments, the last a char. The old 5-argument typedef passed a
	// trailing -2 that landed in shadow space and was ignored, so the call
	// happened to work, but the declaration was wrong.
	typedef void* (*havokRegister_t)(void*, void*, const char*, char);
	havokRegister_t fn_reg = (havokRegister_t)(*(uintptr_t*)(mgrVtable + 24));

	sprintf_s(name, sizeof(name), "ZoneOpt_W%d", workerId);
	fn_reg(mgr, ctx128, name, 3);

	memset(buf8, 0, sizeof(buf8));
	game::g_gameFn.fn_havokPostRegInit(buf8, ctx128);
	_mm_setcsr(_mm_getcsr() | 0x8000);

	// Did the registration actually take?
	//
	// Neither the return value nor the TLS slot answers that. threadInit
	// (0xBCA8D0) returns memSystem+40 on every path, and postRegInit (0xBAECF0)
	// sets the TLS slot unconditionally just above, so both are non-NULL even
	// when registration failed.
	//
	// The real signal is the thread-table slot index. threadInit scans for a
	// free slot and gives up at 64 (`if (v9 >= 64) goto LABEL_7`), reports "Too
	// many threads", and then falls through and writes that index to router[14]
	// anyway (`router[14] = v11`). So router[14] >= 64 is exactly the failure
	// case, and it is the only unambiguous one: router[11] is written on the
	// same path whether or not a slot was found, so it says only that the
	// flags & 1 branch ran, which it always does for us.
	//
	// Reading router[14] is only meaningful because flags = 3 guarantees that
	// branch writes it. contextInit (0xBA4770) zeroes qword indices 10..13 and
	// dword 30, but NOT index 14, and the memset above leaves it 0 — which
	// would read as a valid slot index. With a flags value that omitted bit 0
	// this check would silently always pass.
	{
		uintptr_t slotIndex = *(uintptr_t*)((char*)ctx128 + 14 * sizeof(void*));
		if (slotIndex >= 64)
		{
			std::ostringstream ss;
			ss << "Worker " << workerId
			   << ": Havok registration failed (thread table full, slot="
			   << (unsigned long long)slotIndex << "), exiting";
			LogMsg(ss.str());

			// Both cleanups are safe here, and the first is necessary.
			// HavokThread__cleanup (0xBAED40) only clears the two TLS slots;
			// postRegInit set one of them to point at ctx128, which dies with
			// this frame, so it has to be cleared. HavokThread__contextCleanup
			// (0xBA9580) only rewrites the context's vtable pointer. Neither
			// touches the thread table or reads router[14], so the
			// out-of-range index cannot propagate through them.
			game::g_gameFn.fn_havokCleanup(buf8);
			game::g_gameFn.fn_havokCtxCleanup(ctx128);

			// Flag first, then clear the handle: CreateThread may not have
			// stored it yet, and the flag is what the loops actually test.
			InterlockedExchange(&g_workerInitFailed[workerId], 1);
			g_workerHandles[workerId] = NULL;

			return false;   // never counted live, so nothing to decrement
		}
	}
	return true;
}

static void WorkerStart(int workerId)
{
	// This thread's phase word, for the retire line and the crash
	// record (core.h).
	InterlockedExchange(&g_workerPhase[workerId], WPHASE_IDLE);
	t_navMeshWorkerId    = workerId;
	t_navMeshWorkerPhase = &g_workerPhase[workerId];

	{
		std::ostringstream ss;
		ss << "Worker " << workerId << " started, Havok TLS initialized";
		LogMsg(ss.str());
	}
}

static bool WorkerRunClaimedJob(int workerId)
{
	// Wake protocol: g_jobEvent is manual-reset and means "the
	// queue may be non-empty", not "one job is waiting". Anything that
	// observes NMG+136 non-empty sets it — the bg thread after its own
	// dequeue, and a worker that leaves work behind after claiming. Only a
	// worker holding the queue lock clears it, at either of two points: the
	// queue is empty, or it holds nothing a worker may take (a run of type
	// 2/3/4 jobs, or jobs whose zone is not loaded). The second case matters
	// as much as the first — leaving the event set there makes every worker
	// spin on an immediately-returning wait, each re-taking the +152 lock
	// the bg thread needs.
	//
	// No wakeup is lost, because the clear only ever happens under the queue
	// lock: the setter runs after the insert, and the clearer holds the lock
	// the inserter needs. A burst therefore wakes every idle worker instead
	// of exactly one, which is the starvation this design avoids. The 500 ms
	// timeout stays as a backstop and as the shutdown poll.
	WaitForSingleObject(g_jobEvent, 500);
	// No new claim once NavMesh::stop is seen. hook_navMeshStop
	// sets g_navMeshStopSeen just before the retire sets g_workerShutdown.
	if (NavMeshStopRequested()) return false;

	int hitIdx = -1;
	bool isMiss = false;
	// QPC at the job's unlink, carried in ClaimedJob to the HIT and
	// pipeline paths for the claim-age measurement.
	ClaimedJob claimed;
	NavMeshCacheKey key;
	memset(&key, 0, sizeof(key));
	uintptr_t job = WorkerTryDequeueAny(workerId, &hitIdx, &isMiss, &key, &claimed);
	if (!job) return true;

	int jobType = *(int*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_flags, 88)) & 7;
	claimed.jobType = jobType;

	// Re-check 1 of 3: after the claim. It covers the window
	// between the check above and the claim, and HITs as well: a quit
	// wants no new buildCollision either. The job is dropped as a stale
	// drop leaves it (NoteStopDrop); the release below still runs.
	bool stopDropped = false;
	if (NavMeshStopRequested())
	{
		NoteStopDrop(isMiss ? STALE_SITE_EARLY : STALE_SITE_HIT, job, jobType, claimed.claimQpc);
		stopDropped = true;
	}

	// A HIT whose slot turned out to be stale falls through to the MISS
	// path and regenerates, rather than deleting the job. A HIT whose zone
	// was unloaded returns true (dropped) and does not.
	if (!stopDropped && !isMiss
	    && !WorkerProcessHit((void*)g_navMeshGen, job, jobType, hitIdx, key, claimed.claimQpc, claimed.resetRaises))
		isMiss = true;

	if (!stopDropped && isMiss)
	{
		// Early zone re-check, before paying for a clone: a job already
		// stale here is dropped the way dispatchJob_orig drops it (not
		// freed, not enqueued). The re-check after missLock inside
		// ProcessNavMeshJob stays the authoritative one: CloneNMG and the
		// missLock both wait on processJobCS. Nothing to release here: the
		// loop's single ClaimedJobFinish below releases the bridge.
		int earlyReason = STALE_REASON_NONE;
		if (!JobZoneStillLoaded(job, &earlyReason))
		{
			NoteStaleDrop(STALE_SITE_EARLY, job, jobType, earlyReason, claimed.claimQpc);
		}
		else
		{
			// Worker MISS: clone the NMG so processJobAlt operates on our own
			// workBuffer + queue state. On clone-alloc failure, fall back to
			// running on realNMG under processJobCS — same path the bg thread
			// uses.
			bool cloneStopped = false;
			void* clonedNMG = CloneNMG((void*)g_navMeshGen, &cloneStopped);
			claimed.clone = clonedNMG;
			// Re-check 2 of 3: after CloneNMG, whose wait bails on
			// the stop (cloneStopped) and whose snapshot may have finished
			// just as the stop came. Free the clone and drop the job; a stop
			// never turns into the realNMG fallback below.
			if (cloneStopped || NavMeshStopRequested())
			{
				ClaimedJobReleaseClone(&claimed);
				NoteStopDrop(STALE_SITE_EARLY, job, jobType, claimed.claimQpc);
				stopDropped = true;
			}
			else if (!clonedNMG)
			{
				LogMsg("Worker: CloneNMG failed, fallback to realNMG");
				ProcessNavMeshJob((void*)g_navMeshGen, (void*)g_navMeshGen, &claimed);   // busy bridge held from claim time
			}
			else
			{
				// ProcessNavMeshJob returns normally on its stale-drop exit
				// too, so the clone is freed on every path.
				ProcessNavMeshJob((void*)g_navMeshGen, clonedNMG, &claimed);
				// Re-check 3 of 3 is inside ProcessNavMeshJob, after missLock;
				// its shutdown drop returns here like any other exit. The free
				// goes through the retire's handshake, since the stop may have
				// come at any point of the job.
				ClaimedJobReleaseClone(&claimed);
			}
		}
	}

	// One release per claimed job, on every exit path above: HIT, stale
	// HIT that regenerated, MISS, the clone-failure fallback, the three
	// zone-unloaded drops (HIT, early, after missLock) and the three stop
	// drops. The claimed-zone slot is cleared on the same single
	// path, so it covers the whole job. The busy bridge is a byte in the
	// generator, which NavMesh::stop frees once the retire has returned: the
	// retire's handshake again.
	// The job's adjacency window runs until the drain pops its task;
	// a dropped job is freed by the observer, which never finds it.
	ClaimedJobFinish(&claimed, CJ_FINISH_WORKER, false);

	// A stop seen anywhere in the job (a drop above, or the shutdown drop
	// inside ProcessNavMeshJob) ends the loop now: nothing more is claimed.
	if (stopDropped || NavMeshStopRequested())
		return false;
	return true;
}

} // namespace nm_workers_detail

DWORD WINAPI NavMeshWorkerProc(LPVOID param)
{
	int workerId = (int)(uintptr_t)param;
	char ctx128[128];
	char buf8[8];
	char name[32];
	if (!WorkerInitHavok(workerId, ctx128, buf8, name)) return 1;
	InterlockedIncrement(&navmesh::g_nmCache.g_navMeshWorkersLive);
	WorkerStart(workerId);
	while (!g_workerShutdown)
	{
		if (!WorkerRunClaimedJob(workerId)) break;
	}
	// HavokThread__cleanup (0xBAED40) is not only TLS work. When the
	// monitor-stream slot is set (postRegInit set it) it frees the stream's
	// buffer and the stream itself through this thread's router's allocator
	// (0xBD4470 -> 0xBD4400, router[12]), i.e. Havok memory. So it runs only
	// while the retire has not returned; otherwise the thread just ends, and its
	// TLS slots with it. hkMemoryAllocator__dtor (0xBA9580) writes only ctx128's
	// vtable pointer, a stack local, but it pairs with the cleanup.
	if (WorkerCleanupBegin())
	{
		game::g_gameFn.fn_havokCleanup(buf8);
		game::g_gameFn.fn_havokCtxCleanup(ctx128);
		WorkerCleanupEnd();
	}

	InterlockedDecrement(&navmesh::g_nmCache.g_navMeshWorkersLive);

	// Deferred, not LogMsg: no CRT strings on a worker, and at quit this runs
	// while the main thread is in RetireNavMeshWorkers, which flushes it.
	{
		char line[64];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "Worker %d exiting", workerId);
		LogMsgDeferrable(line);
	}
	return 0;
}

void CreateNavMeshWorkers()
{
	if (!game::g_gameFn.fn_havokContextInit || !game::g_gameFn.fn_havokGetManager || !game::g_gameFn.fn_havokPostRegInit)
	{
		LogMsgDeferrable("NavMesh workers: SKIPPED (Havok fn ptrs missing)");
		return;
	}

	// The live count, not the capacity: NAVMESH_WORKER_COUNT only sizes the
	// arrays and bounds the INI value.
	int want = navmesh::g_navmeshCfg.g_navMeshWorkerCount;
	if (want < 1) want = 1;
	if (want > NAVMESH_WORKER_COUNT) want = NAVMESH_WORKER_COUNT;

	int created = 0;
	for (int i = 0; i < want; ++i)
	{
		g_workerHandles[i] = CreateThread(NULL, 0, NavMeshWorkerProc,
		                                  (LPVOID)(uintptr_t)i, 0, NULL);
		if (g_workerHandles[i]) created++;
	}
	// Called from the first hook_dispatchJob, on the NavMesh bg thread: fixed
	// buffer and the deferred log.
	char ws[128];
	_snprintf_s(ws, sizeof(ws), _TRUNCATE,
		"NavMesh workers: %d/%d created (capacity %d, lazy, from first dispatchJob)",
		created, want, (int)NAVMESH_WORKER_COUNT);
	LogMsgDeferrable(ws);
}
