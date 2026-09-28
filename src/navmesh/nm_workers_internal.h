// nm_workers_internal.h - private navmesh worker cross-unit contract.
// Private declarations shared by the navmesh worker units; each names one definition.
#ifndef KENSHI_ZONE_OPT_NM_WORKERS_INTERNAL_H
#define KENSHI_ZONE_OPT_NM_WORKERS_INTERNAL_H
#include "navmesh/nm_workers.h"
#include "navmesh/jobs/nm_claimed_job.h"
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
namespace nm_workers_detail {
enum { CLAIM_SLOT_BG = NAVMESH_WORKER_COUNT, CLAIM_SLOT_COUNT = NAVMESH_WORKER_COUNT + 1 };
extern volatile long g_cloneProcessing;
extern volatile long g_processJobOwnerTid;
extern __declspec(thread) int t_pjDepth;
extern HANDLE g_pjReleaseEvent;
extern volatile long g_pjPollers;
extern volatile long g_retireReturned;
extern volatile long g_workerCleanupInFlight;
extern volatile long g_nmStopDropCount;
extern volatile LONG g_workerPhase[];
extern volatile long g_pjLockReady;
extern nmgGetSeedPointsAdj_t orig_getSeedPointsAdj;
extern __declspec(thread) uintptr_t t_busyNmg;

bool WorkerCleanupBegin();
bool EnterProcessJobCSStopAware();
bool JobZoneStillLoaded(uintptr_t job, int* reasonOut);
void BackOffForPjPoll();
void NoteStaleDrop(int site, uintptr_t job, int jobType, int reason, LONGLONG claimQpc);
void NoteStopDrop(int site, uintptr_t job, int jobType, LONGLONG claimQpc);
void NbrSeedPrefetch(uintptr_t realNMG, uintptr_t jobZone);
bool NbrCheckStandInCallees();
void NbrSeedFreeTable();
int hook_getSeedPointsAdj(void* nmg, const void* zone, const int* dir);
void NbrSeedJobBegin(int jobType, int gridX, int gridY);
void NbrSeedJobEnd();
void InstallNavMeshLazyHooks();
void* ConstructFreshSettings(uintptr_t origWB);
void* CloneNMG(void* realNMG, bool* stoppedOut);
void FreeClonedNMG(void* clone);
bool ResetWaitRevalidate(uintptr_t job, uintptr_t contentBefore, int* reasonOut);
void ClearBusyBridgeIfIdle(uintptr_t nmg, bool lockHeld);
int L2InFlightAcquire(const NavMeshCacheKey& key);
void L2InFlightRelease(int slot);
uintptr_t WorkerTryDequeueAny(int claimSlot, int* hitIdxOut, bool* isMissOut, NavMeshCacheKey* keyOut, ClaimedJob* claimedOut);
bool WorkerProcessHit(void* nmg, uintptr_t job, int jobType, int hitIdx, const NavMeshCacheKey& key, LONGLONG claimQpc, LONG resetRaises);
void BridgeLockQueue(void* nmg);
void BridgeUnlockQueue(void* nmg);
BusyBridgeOps GameBusyBridgeOps(uintptr_t nmg);
void NoteBusyBridge(bool held);
// NavMesh::stop seen (g_navMeshStopSeen, set first by hook_navMeshStop) or the
// workers told to shut down (RetireNavMeshWorkers, DllMain). Any thread.
static inline bool NavMeshStopRequested()
{
	return InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0
	    || InterlockedCompareExchange(&g_workerShutdown, 0, 0) != 0;
}
static inline void WorkerCleanupEnd()
{
	InterlockedDecrement(&g_workerCleanupInFlight);
}
// No-op on every thread but a worker (its TLS pointer is NULL there), so the
// shared code in ProcessNavMeshJob can call it on the bg thread too.
static inline void NoteWorkerPhase(LONG phase)
{
	volatile LONG* slot = t_navMeshWorkerPhase;
	if (slot)
		InterlockedExchange(slot, phase);
}
static inline void EnterProcessJobCS()
{
	EnterCriticalSection(&processJobCS);
	++t_pjDepth;
	InterlockedExchange(&g_processJobOwnerTid, (long)GetCurrentThreadId());
	MissParHolderSet(MP_HOLD_OTHER);
}

static inline void LeaveProcessJobCS()
{
	--t_pjDepth;
	MissParHolderSet(MP_HOLD_NONE);
	InterlockedExchange(&g_processJobOwnerTid, 0);
	LeaveCriticalSection(&processJobCS);
	// After the release, so a woken poller finds the lock free. A poller that
	// registers just after this read tries once more before it waits, and sees
	// the release itself.
	if (InterlockedCompareExchange(&g_pjPollers, 0, 0) && g_pjReleaseEvent)
		SetEvent(g_pjReleaseEvent);
}
} // namespace nm_workers_detail
namespace nm_workers_detail {
// RAII over the pair above, the same shape BuildCollisionScope gives
// buildCollisionCS. Every region that holds processJobCS calls into game code
// with C++ unwind state (processJobAlt, partialGeneration, the settings ctor
// and dtor, orig_dispatchJob), and a processJobCS left held by an unwinding
// frame parks every MISS, every type 2/3/4 dispatch and every worker clone
// behind it for the rest of the session. The destructor releases on that path.
//
// Release() is for the one region whose normal paths let go before the scope
// ends: the MISS block in ProcessNavMeshJob drops the lock at the top of the
// late-HIT branch and after the L1 store on the generate branch, both inside
// the scope that owns the guard. Calling it keeps those exact release points;
// the destructor then has nothing left to do. It is idempotent.
//
// The owner-tid bookkeeping stays in EnterProcessJobCS/LeaveProcessJobCS, so the
// processJobAlt tripwire reads the same thing whether a region uses the guard
// or not.
//
// The PJ_STOP_AWARE form acquires through
// EnterProcessJobCSStopAware: `held` is false when NavMesh::stop was seen before
// the lock was won, and the caller must test it before using the region.
enum PjStopAwareTag { PJ_STOP_AWARE };

struct ProcessJobLock
{
	bool held;

	ProcessJobLock() : held(true) { EnterProcessJobCS(); }
	explicit ProcessJobLock(PjStopAwareTag) : held(false) { held = EnterProcessJobCSStopAware(); }
	~ProcessJobLock() { Release(); }

	void Release()
	{
		if (!held) return;
		held = false;
		LeaveProcessJobCS();
	}

	// The hold was given up elsewhere (the populate split, at the stop):
	// forget it without a Leave.
	void Abandon() { held = false; }

private:
	ProcessJobLock(const ProcessJobLock&);
	ProcessJobLock& operator=(const ProcessJobLock&);
};
// Measurement helpers for the stats line's pjWait / claimAge / stale tokens.
// Interlocked only; safe on any thread.
static inline void NoteMaxUs(volatile long* slot, long us)
{
	for (;;)
	{
		long prev = InterlockedCompareExchange(slot, 0, 0);
		if (us <= prev) break;
		if (InterlockedCompareExchange(slot, us, prev) == prev) break;
	}
}

static inline void NotePjWait(int site, LONGLONG before, LONGLONG after)
{
	long us = QpcDeltaUs(before, after);
	InterlockedIncrement(&navmesh::g_nmCache.nmPjWaitCount[site]);
	InterlockedExchangeAdd64(&navmesh::g_nmCache.nmPjWaitTotalUs[site], (LONGLONG)us);
	NoteMaxUs(&navmesh::g_nmCache.nmPjWaitMaxUs[site], us);
}

// claimQpc 0 means the caller had no claim time; nothing is recorded.
static inline void NoteClaimAge(bool isMiss, LONGLONG claimQpc, LONGLONG now)
{
	if (!claimQpc) return;
	long us = QpcDeltaUs(claimQpc, now);
	if (isMiss)
	{
		InterlockedIncrement(&navmesh::g_nmCache.nmClaimAgeMissCount);
		InterlockedExchangeAdd64(&navmesh::g_nmCache.nmClaimAgeMissTotalUs, (LONGLONG)us);
		NoteMaxUs(&navmesh::g_nmCache.nmClaimAgeMissMaxUs, us);
		int b = (us < 10000) ? 0 : (us < 100000) ? 1 : (us < 1000000) ? 2 : (us < 5000000) ? 3 : 4;
		InterlockedIncrement(&navmesh::g_nmCache.nmClaimAgeMissBucket[b]);
	}
	else
	{
		InterlockedIncrement(&navmesh::g_nmCache.nmClaimAgeHitCount);
		InterlockedExchangeAdd64(&navmesh::g_nmCache.nmClaimAgeHitTotalUs, (LONGLONG)us);
		NoteMaxUs(&navmesh::g_nmCache.nmClaimAgeHitMaxUs, us);
	}
}
uintptr_t UnloadingZone();
// --------------------------------------------------------------------
// Scratch buffer lazy-init
// --------------------------------------------------------------------
//
// Mirrors orig_dispatchJob (0x3CE030). processJob (0x3C8520) is one of 8 scratch
// consumers and NULL-derefs at +0xA7 without it. Must run before every
// fn_processJobAlt. Safe under processJobCS.

static inline void EnsureGlobalScratchBuffer()
{
	uintptr_t* scratchPtr = (uintptr_t*)(gameBase + RVA_SCRATCH_BUFFER);
	if (!*scratchPtr)
	{
		unsigned int n = *(unsigned int*)(gameBase + RVA_SCRATCH_SIZE);
		if (n == 0) n = 4096;
		*scratchPtr = (uintptr_t)game::g_gameFn.fn_gameNewArr((size_t)n * 8);
	}
}
typedef void (__fastcall *edgeProcess_t)(void* entry);
extern edgeProcess_t orig_edgeProcess;
void NbrSeedFreeTable();   // the neighbour-seed records (below)
enum { NBR_DIR_W = 0, NBR_DIR_E, NBR_DIR_S, NBR_DIR_N, NBR_DIR_COUNT };
// The current type-0 generation on this thread, armed by ProcessNavMeshJob
// around its processJobAlt call and read back after it for the DEV line. POD,
// zero-initialised per thread. The hook records into it only while armed, so a
// call from any other context (none exists today) only counts.
struct NbrSeedJobTls
{
	int           armed;
	int           gridX;
	int           gridY;
	unsigned char cls[NBR_DIR_COUNT];      // NBR_CLASS_*
	int           seeds[NBR_DIR_COUNT];    // the original's return value
	unsigned char standIn[NBR_DIR_COUNT];  // NBR_SI_* (the stand-in inject)
	int           standInSeeds[NBR_DIR_COUNT];
	// Some direction needed a stand-in and its record
	// was not there yet (NBR_SI_LATE). Set by the hook, reset by
	// NbrSeedJobBegin, and read and cleared at the job's L2 decision in
	// ProcessNavMeshJob (NbrSeedJobTakeLate), so it outlives NbrSeedJobEnd.
	int           late;
};
extern __declspec(thread) NbrSeedJobTls t_nbrJob;
// True when this thread's current type-0 generation had a
// direction whose stand-in was late, then clears the flag. Read once, at the
// L2 decision in ProcessNavMeshJob's generate branch (the only place a blob is
// built from a fresh generation); NbrSeedJobBegin also resets it per job.
static inline bool NbrSeedJobTakeLate()
{
	bool late = t_nbrJob.late != 0;
	t_nbrJob.late = 0;
	return late;
}
void FreeFreshSettings(void* wb);   // below; also the failure-path teardown
// Signals "the queue may be non-empty". Every observer of a non-empty queue
// sets it; a worker clears it when the queue holds nothing it can take. See the
// wake protocol note above the worker loop.
static inline void SignalJobAvailable()
{
	if (g_jobEvent)
		SetEvent(g_jobEvent);
}
// Clears the wake event, unless we are shutting down: DllMain signals the event
// once to release every worker, and a worker clearing it there would leave the
// others to wait out the 500 ms timeout before noticing.
static inline void ClearJobAvailable()
{
	if (g_jobEvent && !g_workerShutdown)
		ResetEvent(g_jobEvent);
}
// The content pointer of the job's zone, 0 with no zone. ZoneMaps are never
// freed, so the read is safe whatever the game has unloaded.
static inline uintptr_t JobZoneContent(uintptr_t job)
{
	uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	return zone ? *(volatile uintptr_t*)KLIB_MEMBER(4, zone, ZoneMap_mapContent, OFF_ZONE_CONTENT) : 0;
}
static inline void WorkerBusyEnter(uintptr_t nmg)
{
	t_busyNmg = nmg;
	NoteBusyBridge(BusyBridge(GameBusyBridgeOps(nmg), BUSY_BRIDGE_ENTER, true));
}

static inline void WorkerBusyLeave()
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
void ClaimZoneSet(int slot, uintptr_t zone);

void ClaimZoneClear(int slot);
} // namespace nm_workers_detail
#endif // KENSHI_ZONE_OPT_NM_WORKERS_INTERNAL_H
