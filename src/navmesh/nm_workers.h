// nm_workers.h - Public cross-unit compatibility declarations.

#ifndef KENSHI_ZONE_OPT_NM_WORKERS_H
#define KENSHI_ZONE_OPT_NM_WORKERS_H

#include "navmesh/cache/nm_cache_core.h"
#include "navmesh/cache/nm_disk_cache.h"
#include "navmesh/generation/nm_quality.h"
#include "navmesh/jobs/nm_unload_fence_policy.h"


// Worker pool state
extern volatile long   g_workerShutdown;
extern HANDLE          g_jobEvent;
// The handle of slot i when it belongs to a worker that registered and may
// still be running, or NULL. Returns the handle rather than a bool so callers
// use one snapshot: a worker whose registration fails clears its own slot, and
// re-reading the array after the test can hand back a NULL that has already
// passed it.
HANDLE WorkerSlotHandle(int i);
// Signals shutdown, wakes the workers and waits for them in slices; returns
// only once no worker is live, and at the cap ends the process
// (nm_retire_policy.h). Idempotent. Called from the NavMesh::stop hook, which
// first sets g_navMeshStopSeen (core.h). Once that flag is set no worker starts
// a MISS and no MISS thread blocks on processJobCS. Each slice spent writes a
// line with phases=[w<i>:<phase> ...]; the retired line reports stopDrop=.
void RetireNavMeshWorkers();
extern uintptr_t       g_navMeshGen;
// Sets the workers' wake event when the queue is non-empty (a skipped job may
// be claimable now). No lock held.
void NavMeshWakeWorkersIfQueued();

// Hook functions
char hook_dispatchJob(void* thisNMG);
void hook_nmResultPopulate_diag(void* navData, void* localData, void* result, int param);
void hook_realGenerate(void* workBuffer, void* localData, void* hkaiNavMesh, int param, int timeLowPart);

// Worker infrastructure
// The caller always raises the worker-busy bridge at claim time (both the
// workers and the bg thread) and releases it itself; this function never
// touches it.
// The claim carrier holds the job/type and the original QPC sample (0 =
// unknown). The function re-checks its zone after the processJobCS wait and
// returns with the job untouched if the game unloaded it meanwhile.
struct ClaimedJob;
void ProcessNavMeshJob(void* realNMG, void* workNMG, ClaimedJob* claimed);
DWORD WINAPI NavMeshWorkerProc(LPVOID param);
void CreateNavMeshWorkers();


// --------------------------------------------------------------------
// processJobCS from outside the NavMesh pipeline
// --------------------------------------------------------------------
//
// The save-load reset hook (preload_saveload.cpp) unloads the mod's surviving zones, and
// unloadSingleZone frees zone+0xB8 (terrain collision) and the content. A MISS
// that already passed its post-wait zone re-check is running processJobAlt,
// which reads *(zone+0xB8) (0x3CC99C). Holding processJobCS across the unloads
// lets every MISS in flight finish first; every job claimed meanwhile then
// fails its re-check after the wait. The mod-unload protocol below takes
// it the same way, with a timeout of 0.
//
// Thread rule: never call it from a thread that may already own processJobCS.
// TryEnterCriticalSection recurses, so such a thread would "take" the lock a
// second time, and the owner slot would be zeroed on the inner release while
// the outer hold is still live (the processJobAlt tripwire would then count
// the holder's own calls). That excludes the NavMesh bg thread and the
// workers; any other thread is fine (the main thread, and the save-load reset
// on whatever thread it runs on). It is not a main-thread-only function.
//
// While a timeout > 0 poll runs, the MISS entry points (CloneNMG, missLock)
// stand aside before entering (BackOffForPjPoll, counted as pjYield=), so the
// poll takes the lock at the next release instead of losing to parked
// EnterCriticalSection waiters. A timeout of 0 is exactly one
// TryEnterCriticalSection and asks nothing of them.
//
// NM_PJLOCK_NONE    no lock taken: startPlugin never initialised processJobCS.
// NM_PJLOCK_HELD    taken; the owner-tid bookkeeping is the same as every
//                   other holder's, so the processJobAlt tripwire stays exact.
//                   Release with NavMeshUnlockProcessJob.
// NM_PJLOCK_TIMEOUT not taken within timeoutMs (for 0: the one try failed).
// *waitedMs (optional) receives the time spent trying, in milliseconds.
enum NavMeshPjLockResult
{
	NM_PJLOCK_NONE = 0,
	NM_PJLOCK_HELD,
	NM_PJLOCK_TIMEOUT
};

// --------------------------------------------------------------------
// Mod-unload protocol: the fence below is the API; its order is nm_unload_fence_policy.h's.
// --------------------------------------------------------------------
//
// The mod unloading one of its own zones during play (ZoneManager::
// deactivateZoneMap) frees the zone's content and terrain collision while the
// NavMesh bg thread and the workers may be reading them: the claim-time
// building hash reads the content with no lock, and processJobAlt reads the
// terrain under processJobCS. The fence keeps every mod NavMesh thread off
// the zone for the duration. Main thread only, holding no mod lock, in this
// order:
//
//   NavMeshUnloadFence fence;
//   NmFenceResult r = fence.TryBegin(zone);
//   if (NmFenceProceeds(r))
//   {
//       ... ZoneManager::deactivateZoneMap(zm, zone, save) ...
//       fence.Release();
//   }
//   // REFUSED_*, DEFER_PJ: nothing unloaded; retry the zone on a later
//   // frame (after DEFER_PJ the priority is already asked). UNAVAILABLE:
//   // never retry (NavMeshZoneUnloadUnavailable says why).
//
// With caching on, TryBegin refuses until processJobCS exists, so
// NM_FENCE_NO_LOCK follows only the caching-off begin, which publishes nothing.
//
// const char* NavMeshZoneUnloadUnavailable()
//   NULL when TryBegin can succeed in this build and session (now or later);
//   otherwise a static reason: "caching off but the dispatchJob hook ran". With caching off (the INI key, or the dispatchJob
//   hook failed to install) and the hook never run, no mod code runs on a
//   NavMesh thread at all, so it answers NULL and TryBegin begins at once
//   without publishing anything (vanilla's own exposure only). Main
//   thread.
//
// NmFenceResult NavMeshUnloadFence::TryBegin(void* zone)
//   NM_FENCE_UNAVAILABLE whenever NavMeshZoneUnloadUnavailable is non-NULL.
//   Otherwise takes the generator's queue lock (NMG+152, the primitives
//   PrioritizeNavMeshQueue uses), walks the job queue (NMG+136 via Task::next)
//   and refuses, releasing the lock, if any job of any type names `zone`
//   (NM_FENCE_REFUSED_JOB, counted ulSkipJob=). Otherwise publishes the zone
//   as "being unloaded" before releasing the lock: from then on no worker and
//   no bg claim takes a job for it (they stay queued; ulHeld= counts claim
//   attempts that left one). Then refuses if any worker or the bg thread
//   still has a job for the zone in flight (its claimed-zone slot, written
//   under the same lock at claim and cleared when the job is completely
//   done), clearing the publication first (NM_FENCE_REFUSED_CLAIM,
//   ulSkipClaim=). NM_FENCE_REFUSED (transient) when: zone is NULL,
//   processJobCS is not initialised, no dispatch has run yet (no generator
//   known), or an earlier publication has not been ended. Once begun, the
//   zone stays free of mod NavMesh work until Release, and TryBegin makes one
//   NavMeshTryLockProcessJobFor(0, ...): NM_FENCE_HELD when it takes
//   processJobCS; when it fails, TryBegin ends the publication and calls
//   NavMeshRequestPjPriority before returning NM_FENCE_DEFER_PJ. Never
//   holds the queue lock on return, and never reads NMG+232 or calls
//   NavMeshGenerator::hasJob (both unlocked). The +152 lock is a blocking
//   timed_lock, held only for queue operations (the same as
//   PrioritizeNavMeshQueue).
//
// NavMeshTryLockProcessJobFor(0, ...)
//   Exactly one TryEnterCriticalSection. Fails while any MISS, clone
//   snapshot or type 2/3/4 dispatch holds processJobCS (a MISS holds it
//   2-10 s on a cold cache); a failure during an unload counts ulSkipPj=.
//   A success withdraws the unload priority request below (ulPrioWin= when
//   it was still up).
//
// bool NavMeshRequestPjPriority()
//   Main thread only, never blocks. Raises a "an unload wants
//   processJobCS" request after a pj deferral: while it stands, the MISS entry
//   points (CloneNMG, missLock) stand aside before entering exactly as for a
//   bounded poll (pjYield=), so the caller's next zero-wait try wins the first
//   release no already-parked waiter takes. The request expires 2 s after it
//   was raised and is never extended; after an expiry no new one can be
//   raised for another 2 s, so MISSes keep at least half of any cold stretch.
//   A successful zero-wait try clears it. Returns true when this call raised
//   a new request (counted ulPrio=).
// bool NavMeshPjPriorityActive()
//   The request is up now (raised and not expired). Main thread.
//
// void NavMeshUnloadFence::Release()
//   After NM_FENCE_HELD or NM_FENCE_NO_LOCK: releases processJobCS when
//   held, then clears the publication. After any other result, or a second
//   time, it releases nothing.
//
// bool NavMeshWorkersIdle()
//   Interlocked read of workerBusyCount == 0, safe from the main thread.
//   Every worker and bg claim raises the count
//   under the queue lock before releasing it and lowers it only when the job
//   is done, so true means no NavMesh job was claimed and unfinished at the
//   instant of the read (it can change right after; pair it with the
//   publication above for anything stronger). It is not needed by the protocol above,
//   whose per-zone test is exact.

NavMeshPjLockResult NavMeshTryLockProcessJobFor(DWORD timeoutMs, DWORD* waitedMs);
// Only after NM_PJLOCK_HELD.
void NavMeshUnlockProcessJob();
// Called by startPlugin right after InitNavMeshCacheCS: processJobCS exists
// from here on. Before it, NavMeshTryLockProcessJobFor answers NM_PJLOCK_NONE.
void NavMeshMarkProcessJobLockReady();
const char* NavMeshZoneUnloadUnavailable();
// Raises the save-load reset's admission gate inside one hold of the queue
// lock (+152), as NavMeshUnloadFence::TryBegin publishes its zone; directly when no
// dispatch has run yet. Main thread, no mod lock held.
struct ZoneResetGate;
void NavMeshRaiseResetGateUnderQueueLock(ZoneResetGate* g);
// Lowers the gate under the same queue lock (+152), directly when no
// generator exists. Main thread, no mod lock held.
void NavMeshLowerResetGateUnderQueueLock(ZoneResetGate* g);
bool NavMeshRequestPjPriority();
bool NavMeshPjPriorityActive();
bool NavMeshWorkersIdle();
// For the populate split (nm_misspar.cpp), on the thread that holds
// processJobCS: its hold count, a release of its single hold, and the
// re-acquire that follows. NavMeshStopSeen: NavMesh::stop seen or the workers
// told to shut down; any thread.
int  NavMeshProcessJobDepth();
void NavMeshLeaveForGenerate();
// The re-acquire waits like every other MISS-path wait: until the lock is won
// or the stop is seen.
//   HELD     the lock is held again, exactly as before the release.
//   STOPPED  the stop was seen first; the lock is not held, and the retire's
//            cleanup handshake has begun: the thread may only release what
//            the job holds, and ProcessNavMeshJob ends the handshake after
//            processJobAlt returns.
//   RETIRED  the stop was seen and the retire has already returned: nothing
//            that touches Havok memory may run on this thread again. A
//            backstop: the retire returns only once no worker is live.
enum { NM_REENTER_HELD = 0, NM_REENTER_STOPPED, NM_REENTER_RETIRED };
int  NavMeshReenterAfterGenerate();
bool NavMeshStopSeen();
// True while a worker or the bg thread has a job claimed for this zone (the
// claim covers the whole job, generation included).
bool NavMeshZoneClaimed(void* zone);

// The mod-unload fence. Main thread only, holding no mod lock. TryBegin
// publishes the zone under the queue lock (+152), refusing it while a job for
// it is queued or claimed, then tries processJobCS once with no wait; when it
// is busy, it ends the publication and asks the MISS entry points to stand
// aside before returning NM_FENCE_DEFER_PJ. After NM_FENCE_HELD or
// NM_FENCE_NO_LOCK the caller unloads, then calls Release, which unlocks
// processJobCS when held and then ends the publication. The destructor
// releases only what the caller did not, on a C++ unwind.
struct NavMeshUnloadFence
{
	NmFenceResult result;

	NavMeshUnloadFence() : result(NM_FENCE_RELEASED) {}
	~NavMeshUnloadFence() { Release(); }
	NmFenceResult TryBegin(void* zone);
	void Release();

private:
	NavMeshUnloadFence(const NavMeshUnloadFence&);
	NavMeshUnloadFence& operator=(const NavMeshUnloadFence&);
};


#endif // KENSHI_ZONE_OPT_NM_WORKERS_H
