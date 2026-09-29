// nm_unload.cpp - main-thread navmesh unload and reset-gate admission.
// Unload and reset-gate state is published under queue lock +152.
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
// The zone the main thread is unloading under the mod-unload protocol
// (NavMeshBeginZoneUnload below), or NULL. Published under the generator's
// queue lock (+152) by NavMeshBeginZoneUnload and cleared atomically by
// a refused Begin or NavMeshEndZoneUnload, without that lock. Every claim
// loop runs under +152 and leaves a job for
// this zone queued. Declared here because NavMeshTryLockProcessJobFor counts
// its failures during an unload (ulSkipPj=).
// Main-thread writer; bg/worker claim loops and process-job entry read the
// pointer through UnloadingZone. Each exchange is an untorn behavior input;
// there is no copied set and a completed unload restores NULL.
static void* volatile g_unloadingZone = NULL;

} // namespace nm_workers_detail

// Claimed-zone slots for the mod-unload protocol. One per worker (slot =
// worker id) and one for the bg thread. A claim writes the job's zone into its
// thread's slot BEFORE it releases the queue lock (+152), in the same locked
// region that unlinks the job and raises the busy bridge, and clears it only
// once the job is completely finished with (served, dropped or regenerated,
// L2 write included). NavMeshBeginZoneUnload publishes g_unloadingZone under
// +152, releases that lock, then reads the slots atomically: every prior claim is
// visible in a slot (the lock orders the write before the read), and every
// claim made after it sees g_unloadingZone and leaves the zone's jobs queued.
// A clear seen late only makes the unload wait a frame (conservative).
namespace nm_workers_detail {
// Worker/bg claims write their own slot with Interlocked pointer exchange;
// main unload and NavMeshZoneClaimed read slots atomically without +152.
// Each slot is an untorn behavior input, not a coherent table snapshot.
// Finish clears its slot; a late clear conservatively delays the unload.
static void* volatile g_claimZone[CLAIM_SLOT_COUNT] = {};

uintptr_t UnloadingZone()
{
	return (uintptr_t)InterlockedCompareExchangePointer(&g_unloadingZone, NULL, NULL);
}
// ClaimZoneSet runs under the caller-held generator queue lock +152, before
// unlink ownership is exposed by releasing it. ClaimZoneClear is called
// when the claimed job finishes; an invalid slot, including -1, is a no-op.
void ClaimZoneSet(int slot, uintptr_t zone)
{
	if (slot >= 0 && slot < CLAIM_SLOT_COUNT)
		InterlockedExchangePointer(&g_claimZone[slot], (void*)zone);
}
void ClaimZoneClear(int slot)
{
	if (slot >= 0 && slot < CLAIM_SLOT_COUNT)
		InterlockedExchangePointer(&g_claimZone[slot], NULL);
}
} // namespace nm_workers_detail

// --------------------------------------------------------------------
// Mod-unload protocol -- nm_workers.h has the contract
// --------------------------------------------------------------------
//
// Why the protocol is sound:
//
// 1. Queued jobs. The walk runs under the generator's queue lock (+152), the
//    lock every enqueue (the game's addJob / TaskQueue::push), the game's own
//    pop (updateBT) and every mod claim loop hold. A zone with a queued job of
//    any type is refused (ulSkipJob=).
// 2. Future claims. g_unloadingZone is published before +152 is released, so
//    every claim loop that takes +152 afterwards reads it and leaves the
//    zone's type 0/1 jobs queued (workers and the bg thread's adjacency scan
//    skip past them; without the scan the bg thread returns 0 on such a head;
//    ulHeld=). Types 2/3/4 and the bad-zone forward
//    need no skip: they reach the original only through
//    CallOrigDispatchLocked, under processJobCS, which the caller holds across
//    the unload; afterwards the original finds the content NULL and drops the
//    job (0x3CE0A7), as vanilla does for its own unloads.
// 3. Claims already made. Every claim writes its zone into its thread's slot
//    and raises workerBusyCount inside the same +152 region that unlinks the
//    job. Acquiring
//    +152 orders those writes before the slot reads below, so a claim made
//    before the walk is always visible; one made after cannot be for this
//    zone (point 2). A zone with a claim in flight is refused (ulSkipClaim=).
// 4. The rest runs under processJobCS (the fence's NavMeshTryLockProcessJobFor
//    with a timeout of 0): no processJobAlt, no type 2/3/4 dispatch and no
//    clone snapshot can run during the unload. Three kinds of work run with
//    processJobCS released: a worker clone's realGenerate, which reads only
//    its own job's work buffer, input geometry and result; the collision
//    build of a HIT or a MISS tail; and a HIT's mesh rebuild outside the
//    late-HIT path. None of them reads zone content, and each belongs to a
//    claimed job, whose zone point 3 refuses.
// 5. The save-load reset's admission gate is published under +152 the same
//    way (NavMeshRaiseResetGateLocked): a claim loop that takes +152 after
//    the raise is ordered after it, and every claim made before it is
//    already in its slot and in workerBusyCount. While the gate is up and no
//    stop is seen, both claim loops take nothing and release +152 at once. A
//    job claimed before the raise waits, holding no lock, before its
//    collision build and before its cache lookup (a worker HIT, whose lookup
//    ran at the claim, before its mesh rebuild), and goes on only if its zone
//    is still loaded with the content it held before the wait.
//
// Never read NMG+232 (NavMeshGenerator::current) and never call
// NavMeshGenerator::hasJob: both are unlocked. Never hold +152 across
// the unload: building state changes reach addJob, which takes it.

// Refuses a walk longer than this rather than trust a list that long.
static const int UNLOAD_QUEUE_WALK_CAP = 65536;

// "Can never pass" versus "not now". With caching off (the INI key,
// or hook_manifest.cpp turning it off when the dispatchJob hook failed to install) the
// hook never runs, so no mod code runs on a NavMesh thread at all: no worker,
// no claim-time hash, no lazy hook (InstallNavMeshLazyHooks is reached only
// from hook_dispatchJob, and the only other readers of a zone's content,
// islands.cpp's builder and zone_life.cpp, are main-thread code), so Begin
// succeeds there without publishing anything. g_navMeshGen is set only by hook_dispatchJob: non-zero with
// caching off means the hook ran anyway, and then nothing here is sound.
const char* NavMeshZoneUnloadUnavailable()
{
	if (!navmesh::g_navmeshCfg.cachingEnabled)
		return g_navMeshGen ? "caching off but the dispatchJob hook ran" : NULL;
	return NULL;
}

namespace nm_unload_detail {
enum NavMeshUnloadBegin
{
	NM_UL_REFUSED = 0,   // transient: retry the zone on a later frame
	NM_UL_BEGUN,         // free of mod NavMesh work; NavMeshEndZoneUnload must follow
	NM_UL_UNAVAILABLE    // can never pass in this build or session
};
} // namespace nm_unload_detail
using namespace nm_unload_detail;

static NavMeshUnloadBegin NavMeshBeginZoneUnload(void* zone)
{
	if (!zone)
		return NM_UL_REFUSED;
	if (NavMeshZoneUnloadUnavailable())
		return NM_UL_UNAVAILABLE;
	if (!navmesh::g_navmeshCfg.cachingEnabled)
		return NM_UL_BEGUN;   // no mod NavMesh thread exists: nothing to keep off the zone
	if (!InterlockedCompareExchange(&g_pjLockReady, 0, 0))
		return NM_UL_REFUSED;
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !game::g_gameFn.fn_pathBuilderInit || !game::g_gameFn.fn_pathBuilderFinalize || !game::g_gameFn.fn_readerUnlock)
		return NM_UL_REFUSED;   // no dispatch yet: transient
	// One unload at a time: a Begin not yet matched by its End still owns the
	// skip. (The caller is the main thread only, so this is its own bug.)
	if (UnloadingZone())
		return NM_UL_REFUSED;

	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);

	bool queued = false;
	int walked = 0;
	for (uintptr_t node = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_front, 136));
	     node; node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96)))
	{
		if (++walked > UNLOAD_QUEUE_WALK_CAP ||
		    *(uintptr_t*)KLIB_MEMBER(4, node, NavMeshGenerator__Task_zone, 0) == (uintptr_t)zone)
		{
			queued = true;
			break;
		}
	}

	if (queued)
	{
		game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
		InterlockedIncrement(&navmesh::g_nmCache.nmUlSkipJob);
		return NM_UL_REFUSED;
	}

	// Published under +152: see point 2 above.
	InterlockedExchangePointer(&g_unloadingZone, zone);
	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));

	for (int i = 0; i < CLAIM_SLOT_COUNT; ++i)
	{
		if (InterlockedCompareExchangePointer(&g_claimZone[i], NULL, NULL) == zone)
		{
			InterlockedExchangePointer(&g_unloadingZone, NULL);
			InterlockedIncrement(&navmesh::g_nmCache.nmUlSkipClaim);
			return NM_UL_REFUSED;
		}
	}
	return NM_UL_BEGUN;
}

static void NavMeshEndZoneUnload()
{
	InterlockedExchangePointer(&g_unloadingZone, NULL);
}

// The fence's operations, bound to this protocol. Main thread.
static int  FenceBegin(void* zone)
{
	NavMeshUnloadBegin b = NavMeshBeginZoneUnload(zone);
	return b == NM_UL_BEGUN ? NM_FENCE_BEGIN_OK : b == NM_UL_UNAVAILABLE ? NM_FENCE_BEGIN_UNAVAILABLE : NM_FENCE_BEGIN_REFUSED;
}
static long FenceSkipJob(void*)   { return InterlockedCompareExchange(&navmesh::g_nmCache.nmUlSkipJob, 0, 0); }
static long FenceSkipClaim(void*) { return InterlockedCompareExchange(&navmesh::g_nmCache.nmUlSkipClaim, 0, 0); }
static int  FenceTryPj(void*)
{
	NavMeshPjLockResult pj = NavMeshTryLockProcessJobFor(0, NULL);
	return pj == NM_PJLOCK_HELD ? NM_FENCE_PJ_HELD : pj == NM_PJLOCK_TIMEOUT ? NM_FENCE_PJ_TIMEOUT : NM_FENCE_PJ_NONE;
}
static void FenceUnlockPj(void*)  { NavMeshUnlockProcessJob(); }
static void FenceEnd(void*)       { NavMeshEndZoneUnload(); }
static void FencePriority(void*)  { NavMeshRequestPjPriority(); }

static NmFenceOps FenceOps(void* zone)
{
	NmFenceOps ops = { zone, &FenceBegin, &FenceSkipJob, &FenceSkipClaim, &FenceTryPj,
	                   &FenceUnlockPj, &FenceEnd, &FencePriority };
	return ops;
}

NmFenceResult NavMeshUnloadFence::TryBegin(void* zone)
{
	result = NmFenceTryBegin(FenceOps(zone));
	return result;
}

void NavMeshUnloadFence::Release()
{
	NmFenceRelease(&result, FenceOps(NULL));
}

// Takes the generator's queue lock (+152) around the raise and nothing under
// it: the raise is interlocked writes and a ResetEvent. Called on the main
// thread with no mod lock held. Without a generator no claim loop has run, so
// there is nothing to order against and the gate is raised directly.
void NavMeshRaiseResetGateLocked(ZoneResetGate* g)
{
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !game::g_gameFn.fn_pathBuilderInit || !game::g_gameFn.fn_pathBuilderFinalize || !game::g_gameFn.fn_readerUnlock)
	{
		ZoneResetGateRaise(g);
		return;
	}
	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
	// Published under +152: see point 5 above.
	ZoneResetGateRaise(g);
	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
}

// Main thread, no mod lock held. Takes the generator queue lock (+152) around
// the lower and nothing under it, matching the raise above. Without a
// generator there is no claim loop to order against, so lower directly.
void NavMeshLowerResetGateLocked(ZoneResetGate* g)
{
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !game::g_gameFn.fn_pathBuilderInit || !game::g_gameFn.fn_pathBuilderFinalize || !game::g_gameFn.fn_readerUnlock)
	{
		ZoneResetGateLower(g);
		return;
	}
	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
	ZoneResetGateLower(g);
	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
}

bool NavMeshWorkersIdle()
{
	return InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0) == 0;
}

bool NavMeshZoneClaimed(void* zone)
{
	for (int i = 0; i < CLAIM_SLOT_COUNT; ++i)
		if (zone && InterlockedCompareExchangePointer(&g_claimZone[i], NULL, NULL) == zone)
			return true;
	return false;
}
