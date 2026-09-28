// nm_clone.cpp - NavMeshGenerator clone and five lock initializers.
// snapshot under processJobCS; never close a copied real semaphore.
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
// Always 0 in shipped code; hook_edgeProcess's nonzero branch is unarmed.
volatile long g_cloneProcessing = 0;
} // namespace nm_workers_detail
// --------------------------------------------------------------------
// NavMeshGenerator clone (352 bytes)
// --------------------------------------------------------------------
//
// Workers need their own NMG so processJobAlt doesn't race on realNMG+256
// (the shared workBuffer pointer). A bare memcpy crashes — the ctor sets up
// five critical sections and two self-referencing queue sentinels that a raw
// copy would corrupt. Protocol, one step per hazard:
//
//   +136/+144: input queue head/sentinel-tail. Tail points at &head (self-ref);
//              after memcpy both slots must reference the CLONE's head, not
//              realNMG's. Clear head and point tail at &clone+136.
//   +184/+192: output queue — same pattern.
//   +72 +104 +152 +200 +272: five critical sections. Byte-state is owned by
//              whoever entered them; reusing would silently share lock
//              ownership with realNMG. fn_queueLockInit (RVA 0x25F350) is the
//              game's CS init routine — call it on each slot.
//   +312..+327: ThreadClass substructure. processJobAlt reads +320 as a
//              pointer three times — zeroing it NULL-derefs at processJobAlt
//              +0xD25. Leave it memcpy'd; the reads are side-effect-free.
//              Do NOT call ThreadClass::init/start/finalize on the clone
//              (would spawn an extra OS thread).
//   +232: current-work-item pointer. Zero so isBusy's +232 path returns false
//         for clone-in-flight jobs; queue-walk path still catches queued jobs.
//   +256: settings pointer (workBuffer). Override with a freshly-constructed
//         settings object owned by the clone.
//
// stoppedOut (may be NULL): set true when NULL is returned
// because NavMesh::stop was seen during the processJobCS wait. The block
// allocated before the wait has been released then (unless the retire had
// already returned), and the caller must drop the job, never fall back to the
// real generator. False on success and on every other failure.
namespace nm_workers_detail {
void* CloneNMG(void* realNMG, bool* stoppedOut = NULL)
{
	if (stoppedOut) *stoppedOut = false;
	if (!realNMG || !fn_queueLockInit || !fn_settingsCtor) return NULL;

	void* clone = HavokTlsAlloc(NMG_STRUCT_SIZE);
	if (!clone) return NULL;

	char* c = (char*)clone;
	void* freshWB = NULL;

	// Every read of the real NMG and its work buffer happens under
	// processJobCS. Two races close here.
	//   - The bg thread's own MISS swaps realNMG+256 to a temporary fresh work
	//     buffer for the duration of processJobAlt, installing and restoring it
	//     inside this lock. Reading the pointer outside could snapshot the
	//     temporary instead of the real one.
	//   - The real work buffer's +520 override array sits at count 4 with
	//     capacity 4, so the game's next append reallocates it. Copying those
	//     entries outside the lock could read a freed buffer.
	// The lock is not held across the clone-local work below (self-reference
	// rebasing, the five lock inits), only across the reads: the guard's scope
	// is exactly this block.
	{
		// pjWait clone=: QPC either side of the guard's construction, so the
		// lock is taken exactly where it was and the owner-tid bookkeeping in
		// EnterProcessJobCS is untouched.
		LONGLONG waitStart = QpcNow();
		NoteWorkerPhase(WPHASE_WAIT_CLONE);
		// Stand aside while a bounded poll wants processJobCS.
		// Nothing is held here. Inside the timed span, so pjWait
		// clone= shows the whole delay; pjYield= says when this was the cause.
		BackOffForPjPoll();
		// A stop-aware wait. The whole loop counts as wait, the bail
		// included, so pjWait clone= stays the time spent trying.
		ProcessJobLock lock(PJ_STOP_AWARE);
		NotePjWait(PJWAIT_CLONE, waitStart, QpcNow());
		if (!lock.held)
		{
			// NavMesh::stop came during the wait: no snapshot, no fresh work
			// buffer. Only the block allocated above is ours, and it is
			// released only while the retire has not returned.
			if (WorkerCleanupBegin())
			{
				HavokTlsFree(clone, NMG_STRUCT_SIZE);
				WorkerCleanupEnd();
			}
			if (stoppedOut) *stoppedOut = true;
			return NULL;
		}
		NoteWorkerPhase(WPHASE_CLONING);
		memcpy(clone, realNMG, NMG_STRUCT_SIZE);
		uintptr_t realWB = *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)realNMG, NavMeshGenerator_settings, 256));
		freshWB = ConstructFreshSettings(realWB);
	}

	if (!freshWB)
	{
		HavokTlsFree(clone, NMG_STRUCT_SIZE);
		InterlockedIncrement(&navmesh::g_nmCache.nmCloneConstructFailCount);
		return NULL;
	}

	// Clone-local from here: nothing below reads the real NMG.
	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_queue_front, 136)) = 0;
	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_queue_back, 144)) = (uintptr_t)(KLIB_MEMBER(4, c, NavMeshGenerator_queue_front, 136));
	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_done_front, 184)) = 0;
	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_done_back, 192)) = (uintptr_t)(KLIB_MEMBER(4, c, NavMeshGenerator_done_front, 184));

	fn_queueLockInit((void*)KLIB_MEMBER(4, c, ThreadClass_runMute, 72));
	fn_queueLockInit((void*)KLIB_MEMBER(4, c, ThreadClass_lockedWhileRunningMute, 104));
	fn_queueLockInit((void*)KLIB_MEMBER(4, c, NavMeshGenerator_queue_mutex, 152));
	fn_queueLockInit((void*)KLIB_MEMBER(4, c, NavMeshGenerator_done_mutex, 200));
	fn_queueLockInit((void*)KLIB_MEMBER(4, c, NavMeshGenerator_taskMutex, 272));

	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_current, 232)) = 0;
	*(uintptr_t*)(KLIB_MEMBER(4, c, NavMeshGenerator_settings, 256)) = (uintptr_t)freshWB;

	InterlockedIncrement(&navmesh::g_nmCache.nmCloneConstructCount);
	return clone;
}
} // namespace nm_workers_detail

// The five boost::shared_mutex members of a NavMeshGenerator. CloneNMG
// re-creates all five with fn_queueLockInit, which is boost_shared_mutex__ctor
// (0x25F350): it writes three semaphore handles per lock, at +8 (an anonymous
// semaphore) and +16 / +24 (CreateSemaphoreA). The game closes exactly these
// fifteen — NavMeshGenerator__dtor (0x3C8A60) closes the nine belonging to
// +152, +200 and +272, then tail-calls ThreadClass__dtor (0x25F9C0) for the six
// belonging to +72 and +104 — and nothing ever destroys a clone, so every
// worker MISS leaked fifteen handles.
static const int NMG_LOCK_HANDLE_OFFSETS[3] = { 8, 16, 24 };
static uintptr_t NmgLockAddress(uintptr_t base, int index)
{
 switch (index)
 {
 case 0: return KlibField_ThreadClass_runMute(base);
 case 1: return KlibField_ThreadClass_lockedWhileRunningMute(base);
 case 2: return KlibField_NavMeshGenerator_queue_mutex(base);
 case 3: return KlibField_NavMeshGenerator_done_mutex(base);
 default: return KlibField_NavMeshGenerator_taskMutex(base);
 }
}


// Closes the clone's fifteen. The clone is a memcpy of the real generator, so a
// handle that is still identical to the real one was never re-created and must
// not be closed: closing it would destroy a handle the game is still using.
// That cannot happen on the path CloneNMG takes today, and the comparison is
// fifteen loads, so it stays in as a guard rather than an assumption.
static void CloseClonedNMGLocks(void* clone)
{
	uintptr_t real = g_navMeshGen;
	for (int i = 0; i < 5; ++i)
	{
		for (int h = 0; h < 3; ++h)
		{
			int off = NMG_LOCK_HANDLE_OFFSETS[h];
			HANDLE ch = *(HANDLE*)(NmgLockAddress((uintptr_t)clone, i) + off);
			if (!ch) continue;
			if (real && ch == *(HANDLE*)(NmgLockAddress(real, i) + off))
			{
				InterlockedIncrement(&navmesh::g_nmCache.nmCloneHandleSkipped);
				continue;
			}
			CloseHandle(ch);
			*(HANDLE*)(NmgLockAddress((uintptr_t)clone, i) + off) = NULL;
			InterlockedIncrement(&navmesh::g_nmCache.nmCloneHandleClosed);
		}
	}
}

namespace nm_workers_detail {
void FreeClonedNMG(void* clone)
{
	if (!clone) return;
	// Safe here: processJobAlt has returned, the clone's locks are
	// idle and nothing else holds the clone.
	CloseClonedNMGLocks(clone);
	uintptr_t wb = *(uintptr_t*)(KLIB_MEMBER(4, (char*)clone, NavMeshGenerator_settings, 256));
	FreeFreshSettings((void*)wb);
	HavokTlsFree(clone, NMG_STRUCT_SIZE);
}
} // namespace nm_workers_detail
