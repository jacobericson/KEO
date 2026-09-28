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
// queue lock (+152) by NavMeshBeginZoneUnload and cleared by
// NavMeshEndZoneUnload. Every claim loop runs under +152 and leaves a job for
// this zone queued. Declared here because NavMeshTryLockProcessJobFor counts
// its failures during an unload (ulSkipPj=).
void* volatile g_unloadingZone = NULL;

} // namespace nm_workers_detail

// Claimed-zone slots for the mod-unload protocol. One per worker (slot =
// worker id) and one for the bg thread. A claim writes the job's zone into its
// thread's slot BEFORE it releases the queue lock (+152), in the same locked
// region that unlinks the job and raises the busy bridge, and clears it only
// once the job is completely finished with (served, dropped or regenerated,
// L2 write included). NavMeshBeginZoneUnload publishes g_unloadingZone under
// +152 and then reads these slots: every claim made before its locked walk is
// visible in a slot (the lock orders the write before the read), and every
// claim made after it sees g_unloadingZone and leaves the zone's jobs queued.
// A clear seen late only makes the unload wait a frame (conservative).
namespace nm_workers_detail {
const int CLAIM_SLOT_BG    = NAVMESH_WORKER_COUNT;
const int CLAIM_SLOT_COUNT = NAVMESH_WORKER_COUNT + 1;
void* volatile g_claimZone[CLAIM_SLOT_COUNT] = {};
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
// 4. The rest runs under processJobCS (the caller's NavMeshTryLockProcessJobFor
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

NavMeshUnloadBegin NavMeshBeginZoneUnload(void* zone)
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
	if (!nmg || !fn_pathBuilderInit || !fn_pathBuilderFinalize || !fn_readerUnlock)
		return NM_UL_REFUSED;   // no dispatch yet: transient
	// One unload at a time: a Begin not yet matched by its End still owns the
	// skip. (The caller is the main thread only, so this is its own bug.)
	if (UnloadingZone())
		return NM_UL_REFUSED;

	char initBuf[16];
	void* initResult = fn_pathBuilderInit(initBuf);
	fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);

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
		fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
		InterlockedIncrement(&navmesh::g_nmCache.nmUlSkipJob);
		return NM_UL_REFUSED;
	}

	// Published under +152: see point 2 above.
	InterlockedExchangePointer(&g_unloadingZone, zone);
	fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));

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

void NavMeshEndZoneUnload()
{
	InterlockedExchangePointer(&g_unloadingZone, NULL);
}

// Takes the generator's queue lock (+152) around the raise and nothing under
// it: the raise is interlocked writes and a ResetEvent. Called on the main
// thread with no mod lock held. Without a generator no claim loop has run, so
// there is nothing to order against and the gate is raised directly.
void NavMeshRaiseResetGateLocked(ZoneResetGate* g)
{
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !fn_pathBuilderInit || !fn_pathBuilderFinalize || !fn_readerUnlock)
	{
		ZoneResetGateRaise(g);
		return;
	}
	char initBuf[16];
	void* initResult = fn_pathBuilderInit(initBuf);
	fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
	// Published under +152: see point 5 above.
	ZoneResetGateRaise(g);
	fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
}

// Main thread, no mod lock held. Takes the generator queue lock (+152) around
// the lower and nothing under it, matching the raise above. Without a
// generator there is no claim loop to order against, so lower directly.
void NavMeshLowerResetGateLocked(ZoneResetGate* g)
{
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !fn_pathBuilderInit || !fn_pathBuilderFinalize || !fn_readerUnlock)
	{
		ZoneResetGateLower(g);
		return;
	}
	char initBuf[16];
	void* initResult = fn_pathBuilderInit(initBuf);
	fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
	ZoneResetGateLower(g);
	fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
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
