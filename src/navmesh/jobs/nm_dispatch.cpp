// nm_dispatch.cpp - NavMesh bg dispatch and pool decision.
// every original dispatch call holds processJobCS after the queue lock is released.
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
using namespace nm_workers_detail;
// --------------------------------------------------------------------
// orig_dispatchJob under processJobCS
// --------------------------------------------------------------------
//
// dispatchJob_orig's type-3 branch runs processJobAlt on the real generator and
// work buffer, and it frees the global scratch buffer when the queue empties
// (0x3CE2A9). Both are exactly the state processJobCS serializes, so the
// original has to run under it.
//
// measure=true records the hold for the type 2/3/4 path, the one this wrap
// exists for; the bad-zone and bypass forwards are wrapped for correctness but
// are not part of that measurement.
static char CallOrigDispatchUnderPj(void* thisNMG, bool measure)
{
	ProcessJobLock lock;   // held for the whole function, released on return
	if (measure) MissParHolderSet(MP_HOLD_T234);

	LARGE_INTEGER t0, t1;
	if (measure) QueryPerformanceCounter(&t0);

	char r = game::g_hookOrig.orig_dispatchJob(thisNMG);

	if (measure)
	{
		QueryPerformanceCounter(&t1);
		long ms10 = (long)(QPCToMs(t0, t1) * 10.0);
		InterlockedIncrement(&navmesh::g_nmCache.nmT234Count);
		InterlockedExchangeAdd(&navmesh::g_nmCache.nmT234TotalMsTimes10, ms10);
		for (;;)
		{
			long prev = InterlockedCompareExchange(&navmesh::g_nmCache.nmT234MaxMsTimes10, 0, 0);
			if (ms10 <= prev) break;
			if (InterlockedCompareExchange(&navmesh::g_nmCache.nmT234MaxMsTimes10, ms10, prev) == prev) break;
		}
	}

	return r;   // ~ProcessJobLock: same release point as the old trailing Leave
}


// Set once the first-dispatch probes have run against a real work buffer, so
// hook_dispatchJob stops taking processJobCS on every dispatch.
static volatile long g_firstDispatchDone = 0;




// The bg thread stops waiting in this call and returns 0, so threadProc sleeps
// 100 ms as it does on an empty queue. After a slice the reservation stays;
// after a stop it goes, and threadProc then sees threadRunning cleared.
static char NmAdjBgLeave(uintptr_t nmg, NmAdjWaitResult w, LONGLONG* adjSlice)
{
	NmAdjBgWaitDone(adjSlice, w == NMADJ_WAIT_SLICE);
	if (w != NMADJ_WAIT_STOP)
		return 0;
	NmQueueLock queue(nmg);
	NmAdjBgReleaseLocked(queue);
	queue.Release();
	return 0;
}

namespace nm_workers_detail
{

// No pool this session: recorded for the stats line, and said once with its
// cause. The bg thread serves every job alone, as it does before the pool
// exists.
static void RefuseNavMeshWorkers(NmPoolDecision decision)
{
	InterlockedExchange(&navmesh::g_nmCache.g_navMeshPoolRefusal, (long)decision);
	const char* why = decision == NMPOOL_REFUSE_STOPHOOK
		? "NavMesh::stop is not hooked, so nothing would retire them before the game frees the Havok heap"
		: "buildCollision is not hooked, so their collision builds would run beside the bg thread's with no cover";
	char line[256];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "NavMesh workers: none started, workers=%s (%s)",
		NmPoolRefusalToken(decision), why);
	LogMsgDeferrable(line);
}

} // namespace nm_workers_detail
using namespace nm_workers_detail;

namespace nm_dispatch_detail {
static uintptr_t BgFirstDispatchPhase(void* thisNMG)
{
	if (!g_navMeshBgThreadId)
	{
		if (InterlockedCompareExchange((volatile LONG*)&g_navMeshBgThreadId,
		                               (LONG)GetCurrentThreadId(), 0) == 0)
		{
			// First dispatch, on the NavMesh bg thread: fixed buffer and the
			// deferred log, like the lazy hook install just below.
			char ts[64];
			_snprintf_s(ts, sizeof(ts), _TRUNCATE, "NavMesh bg thread TID=%lu",
			            (unsigned long)g_navMeshBgThreadId);
			LogMsgDeferrable(ts);
		}
	}

	uintptr_t nmg = (uintptr_t)thisNMG;

	if (!g_navMeshGen)
		g_navMeshGen = nmg;

	// The lazy hooks install on the first dispatch, where the Havok world is
	// guaranteed live.
	InstallNavMeshLazyHooks();

	// All three dereference realNMG+256, so they race the bg thread's own swap
	// and any worker's clone snapshot and need processJobCS.
	//
	// They must not take it on every dispatch. The bg thread's HIT path is
	// deliberately lock-free so it keeps serving cached meshes while a worker
	// holds processJobCS for a ~1.5 s MISS; an Enter here would block the bg
	// thread for that whole MISS and close exactly the overlap the worker pool
	// exists for. So the block runs once, behind a latch checked without the
	// lock.
	//
	// Running once is enough: the three probes latch internally on their own
	// flags. Nothing here writes the real work buffer; it keeps the game's own
	// settings.
	//
	// The latch is set only when the work buffer was actually present. All
	// three probes re-arm themselves when they find realNMG+256 NULL; leaving
	// the latch clear in that case gives the next dispatch the retry they
	// expect.
	if (!InterlockedCompareExchange(&g_firstDispatchDone, 0, 0))
	{
		ProcessJobLock lock;   // this block only
		if (!InterlockedCompareExchange(&g_firstDispatchDone, 0, 0))
		{
			MissParKeycodeWarmup();
			ProbeNavMeshSettings(nmg);
			VerifyNavMeshSettings(nmg);
			ProbeWorkBufferSize(nmg);
			// Check the real WB's pruning / extra-vertex values
			// against the L2 settings hash, before the first job is served;
			// a difference turns L2 off for the session (nm_quality.h).
			CheckGenerationSettingsKey(nmg);

			if (*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256)))
				InterlockedExchange(&g_firstDispatchDone, 1);
		}
	}

	// Checked lock-free first, so once all five flags read 1 no later dispatch
	// takes processJobCS here again. This always runs past the lazy-hook
	// install above. hook_dispatchJob keeps running after NavMesh::stop, so
	// this skips the retry once stop is seen and takes processJobCS with the
	// stop-aware lock, the same way every other MISS-path wait does rather
	// than blocking on it.
	bool keycodeStopSeen = NavMeshStopRequested();
	if (!keycodeStopSeen && !MissParKeycodesReady())
	{
		ProcessJobLock lock(PJ_STOP_AWARE);
		if (lock.held && !NavMeshStopRequested())
			MissParKeycodeWarmup();
	}

	// The pool is decided once, on the first dispatch that sees the lazy
	// install finished. It starts only with both NavMesh::stop and
	// buildCollision hooked: the first retires the workers before the game
	// frees the Havok heap, the second covers their collision builds. The
	// install above ran on this thread, so both flags are final here.
	{
		static volatile long poolDecided = 0;
		if (!InterlockedCompareExchange(&poolDecided, 0, 0))
		{
			NmPoolDecision d = NmPoolDecide(
				InterlockedCompareExchange(&navmesh::g_nmCache.lazyHooksInstalled, 0, 0) == NM_LAZY_DONE,
				HookRowInstalled(HOOK_NAVMESH_STOP),
				HookRowInstalled(HOOK_BUILD_COLLISION_IMPL));
			if (d != NMPOOL_WAIT && !InterlockedCompareExchange(&poolDecided, 1, 0))
			{
				if (d == NMPOOL_CREATE)
					CreateNavMeshWorkers();
				else
					RefuseNavMeshWorkers(d);
			}
		}
	}
	return nmg;
}

struct BgDispatchCtx
{
	void* thisNMG;
	uintptr_t nmg;
	bool adj;
	uintptr_t job;
	uintptr_t peekZone;
	uintptr_t adjPrev;
	int jobType;
	ClaimedJob* claimed;
	NmQueueLock* queue;

	bool Pick(char* result);
	char ProcessPicked();
};

// Returns true with *queue held (+152); ProcessPicked releases it on every path.
bool BgDispatchCtx::Pick(char* result)
{
	// Multi-consumer dequeue: workers may free jobs concurrently, so peek
	// under the queue lock.
	if (!*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)))
	{
		ClearBusyBridgeIfIdle(nmg, NULL);
		// The bg-only age's clock: an idle bg thread has looked, and seen nothing.
		NmAdjBgLookedUnlocked();
		*result = 0;
		return false;
	}

	// A job whose stitches would touch a job still in flight or undrained is
	// reserved and passed over for a later one; with nothing runnable the bg
	// thread waits here, holding no lock, then decides again.
	const bool adj = NmAdjActive();
	LONGLONG adjSlice = 0;
	adjRetry:
	queue->Acquire(nmg);

	job = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136));
	if (!job)
	{
		if (adj)
		{
			NmAdjBgReleaseLocked(*queue);
			NmAdjBgWaitDone(&adjSlice, false);
		}
		// Still under +152 here, so the read-and-clear is atomic against a
		// worker's WorkerBusyEnter without taking the lock a second time.
		ClearBusyBridgeIfIdle(nmg, queue);
		queue->Release();
		*result = 0;
		return false;
	}

	// A save-load reset is running: no claim and no forward, so nothing reaches
	// processJobCS. No pop and no pin; threadProc sleeps 100 ms and calls again.
	if (ZoneResetGateUp(&g_zoneResetGate) && ZoneResetAdmit(true, NavMeshStopRequested(), ZONE_RESET_SITE_CLAIM) == ZONE_RESET_DEFER_RESET)
	{
		if (adj)
			NmAdjBgWaitDone(&adjSlice, false);
		queue->Release();
		ZoneResetGateNoteDeferred(&g_zoneResetGate, ZONE_RESET_SITE_CLAIM);
		*result = 0;
		return false;
	}

	peekZone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	if (!peekZone || !*(uintptr_t*)KLIB_MEMBER(4, peekZone, ZoneMap_mapContent, 0))
	{
		// Dropped by the original without a stitch, so it registers nothing;
		// pinned so the original pops this node and no other. A reservation
		// held by another task stays.
		if (adj)
		{
			NmAdjBgForwardLocked(*queue, job);
			NmAdjBgWaitDone(&adjSlice, false);
		}
		queue->Release();
		// Wake the workers once this forward has consumed the head. A worker
		// that found the queue all-ineligible cleared the event, and nothing
		// else re-sets it on this path, so eligible jobs queued behind the head
		// would sit until the 500 ms backstop expired.
		{
			char r = CallOrigDispatchUnderPj(thisNMG, false);
			if (adj)
				NmAdjBgUnpin();
			if (*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)))
				SignalJobAvailable();
			*result = r;
			return false;
		}
	}

	// The job's predecessor when the scan picks a type 0/1 job behind the head.
	adjPrev = 0;
	if (adj)
	{
		NmAdjBgScan scan;
		NmAdjBgScanBeginLocked(*queue, &scan);
		const uintptr_t unloading = UnloadingZone();
		bool headHeld = false;
		for (uintptr_t node = job; node; node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96)))
		{
			int t = *(int*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_flags, 88)) & 7;
			uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, node, NavMeshGenerator__Task_zone, 0);
			// A type 0/1 job of the zone being unloaded stays queued, as
			// for the workers; types 2/3/4 reach the original under processJobCS.
			bool held = (t == 0 || t == 1) && zone && zone == unloading;
			if (held && node == job)
				headHeld = true;
			bool eligible = zone && *(uintptr_t*)KLIB_MEMBER(4, zone, ZoneMap_mapContent, 0) && !held;
			if (NmAdjBgOfferLocked(*queue, &scan, node, eligible, t != 0 && t != 1))
				break;
		}
		if (headHeld)
			InterlockedIncrement(&navmesh::g_nmCache.nmUlHeld);
		uintptr_t pick = 0;
		NmAdjBgDecision decision = NmAdjBgScanEndLocked(*queue, &scan, &pick, adjSlice == 0);
		if (decision != NMADJ_BG_CLAIM)
		{
			queue->Release();
			// Workers may take what the bg thread cannot. Once per episode:
			// every release wakes them again (Wake in nm_adjacency.cpp).
			if (!adjSlice)
				SignalJobAvailable();
			if (decision == NMADJ_BG_NONE)
			{
				NmAdjBgWaitDone(&adjSlice, false);
				*result = 0;
				return false;
			}
			NmAdjWaitResult w = NmAdjBgWait(nmg, &adjSlice);
			if (w == NMADJ_WAIT_RETRY)
				goto adjRetry;
			*result = NmAdjBgLeave(nmg, w, &adjSlice);
			return false;
		}
		NmAdjBgWaitDone(&adjSlice, false);

		for (uintptr_t node = job; node && node != pick; node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96)))
			adjPrev = node;
		int pickType = *(int*)(KLIB_MEMBER(4, pick, NavMeshGenerator__Task_flags, 88)) & 7;
		if (adjPrev && pickType != 0 && pickType != 1)
		{
			// The original pops the front, so a bg-only pick moves there (it is
			// pinned). The rest keeps its order; the prioritizer reorders this
			// queue by zone tier anyway.
			uintptr_t after = *(uintptr_t*)(KLIB_MEMBER(4, pick, NavMeshGenerator__Task_next, 96));
			*(uintptr_t*)(KLIB_MEMBER(4, adjPrev, NavMeshGenerator__Task_next, 96)) = after;
			if (!after)
				*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_back, 144)) = KLIB_MEMBER(4, adjPrev, NavMeshGenerator__Task_next, 96);
			*(uintptr_t*)(KLIB_MEMBER(4, pick, NavMeshGenerator__Task_next, 96)) = job;
			*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)) = pick;
			adjPrev = 0;
		}
		job = pick;
		peekZone = *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0);
	}

	jobType = *(int*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_flags, 88)) & 7;
	this->adj = adj;
	return true;
}

// Entered with *queue held (+152); every path releases it before anything that takes processJobCS.
char BgDispatchCtx::ProcessPicked()
{
	if (jobType != 0 && jobType != 1)
	{
		// No consumer can take this head across the unlock: workers only ever
		// pop type 0/1 heads, and appends go to the tail while the head is
		// non-NULL. Without the adjacency registry the main thread's
		// PrioritizeNavMeshQueue could still put a different job at the head
		// and the original would re-pop that one; with it the head is pinned
		// (the prioritizer leaves a pinned head in place), because the job
		// registered by the scan above must be the job the original processes.
		queue->Release();
		InterlockedIncrement(&navmesh::g_nmCache.nmJobCount);
		InterlockedIncrement(&navmesh::g_nmCache.nmCacheSkipCount);
		// Same wake as the bad-zone forward: a stitching job at the head is
		// exactly the case that made workers clear the event and sleep on a
		// queue that still held type 0/1 work behind it.
		{
			char r = CallOrigDispatchUnderPj(thisNMG, true);
			// The original pushed the task (or deleted it) before returning.
			if (adj)
			{
				NmAdjOwnFinished();
				NmAdjBgUnpin();
			}
			if (*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)))
				SignalJobAvailable();
			return r;
		}
	}

	uintptr_t nextJob = *(uintptr_t*)(KLIB_MEMBER(4, job, NavMeshGenerator__Task_next, 96));

	// Mod-unload protocol: a type 0/1 head for the zone the main thread is
	// unloading stays queued. Without the adjacency scan (which passes over
	// such a job) the bg thread pops only the head, so it cannot skip past it;
	// it returns 0, which makes threadProc (0x3CE2F0) sleep 100 ms instead of
	// re-calling at once and spinning on +152 for the whole unload. Once the
	// unload has NULLed the zone's content, the bad-zone forward above hands
	// the job to the original, which drops it (0x3CE0A7). Workers can scan past
	// this head, so wake them if anything is behind it.
	if (peekZone == UnloadingZone())
	{
		queue->Release();
		InterlockedIncrement(&navmesh::g_nmCache.nmUlHeld);
		if (nextJob)
			SignalJobAvailable();
		return 0;
	}

	InterlockedIncrement(&navmesh::g_nmCache.nmJobCount);

	if (adjPrev)
	{
		// A type 0/1 pick behind the head: unlink exactly this node, as a
		// worker does.
		*(uintptr_t*)(KLIB_MEMBER(4, adjPrev, NavMeshGenerator__Task_next, 96)) = nextJob;
		if (!nextJob)
			*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_back, 144)) = KLIB_MEMBER(4, adjPrev, NavMeshGenerator__Task_next, 96);
	}
	else
	{
		*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)) = nextJob;
		if (!nextJob)
			*(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_back, 144)) = KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136);
	}

	// The bg thread's claim markers, raised BEFORE the unlock exactly as a
	// worker's are (WorkerTryDequeueAny): the busy bridge (so workerBusyCount
	// covers this job from the unlink, which is what NavMeshWorkersIdle reads)
	// and the claimed-zone slot (so NavMeshUnloadFence::TryBegin sees this zone as in
	// flight), so the job is never invisible to either check. Released once, after
	// ProcessNavMeshJob returns (its L2 write included, as for a worker), or on
	// the vanilla content-check return below.
	ClaimedJobBeginLocked(*queue, claimed, job, jobType, peekZone, CLAIM_SLOT_BG);

	queue->Release();

	// Claim time: the bg thread took ownership of `job` in the unlink just
	// above (either arm). Stored in ClaimedJob for ProcessNavMeshJob.
	claimed->claimQpc = QpcNow();

	// Vanilla's own claim-time check (dispatchJob_orig 0x3CE030:
	// `if (!**job) return 1;`), unchanged. ProcessNavMeshJob re-checks after
	// its processJobCS wait.
	if (!*(uintptr_t*)KLIB_MEMBER(4, *(uintptr_t*)KLIB_MEMBER(4, job, NavMeshGenerator__Task_zone, 0), ZoneMap_mapContent, 0))
	{
		ClaimedJobFinish(claimed, CJ_FINISH_BG_CONTENT_LOST, adj);
		return 1;
	}

	// Signal workers that a new job is available.
	if (g_jobEvent && *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136)))
		SetEvent(g_jobEvent);

	// HITs process concurrently with workers; MISSes serialize via processJobCS.
	// Returns 1 on every path, including ProcessNavMeshJob's drop of a job whose
	// zone was unloaded during the processJobCS wait: the value vanilla returns
	// for a dropped job. That drop also takes every type 0/1 MISS once
	// NavMesh::stop is seen: its missLock wait gives up without the
	// lock, so the bg thread starts no new MISS of the mod's own during the
	// worker retire. HITs and the type 2/3/4 / bad-zone forwards are unchanged.
	// The bridge was raised at the unlink above; ClaimedJobFinish below releases it.
	ProcessNavMeshJob(thisNMG, thisNMG, claimed);
	ClaimedJobFinish(claimed, CJ_FINISH_BG_PIPELINE_RETURN, adj);
	return 1;
}

} // namespace nm_dispatch_detail

using namespace nm_dispatch_detail;

// NavMesh bg thread entry. Runs the step-3 probes, then dequeues
// one job and processes it (HIT via cache reconstruction, MISS via
// ProcessNavMeshJob's swap-settings path). At step >= 4, workers also dequeue
// from this queue, so the peek runs under the queue lock.
char hook_dispatchJob(void* thisNMG)
{
	uintptr_t nmg = BgFirstDispatchPhase(thisNMG);
	ClaimedJob claimed;
	BgDispatchCtx context;
	context.thisNMG = thisNMG;
	context.nmg = nmg;
	context.claimed = &claimed;
	NmQueueLock queueLock;
	context.queue = &queueLock;
	char result = 0;
	if (!context.Pick(&result)) return result;
	return context.ProcessPicked();
}
