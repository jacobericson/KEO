// nm_lazy_hooks.cpp - first-dispatch navmesh hooks and stop flag.
// the stop hook sets its flag before calling the original.
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
// processJobAlt ownership tripwire
// --------------------------------------------------------------------
//
// Every caller of processJobAlt (0x3CBE60) must hold processJobCS: it works on
// the generator's shared work buffer, the global scratch buffer and the seed
// map. This pass-through counts any call from a thread that does not own the
// lock, which covers both the mod's own calls and the game's (orig_dispatchJob
// runs processJobAlt on the real work buffer for type 3).
//
// The hook patches the function entry, so the mod's fn_processJobAlt calls go
// through it too. That is deliberate: the point is to catch a violation
// wherever it comes from.
static processJobAlt_t orig_processJobAltTrip = NULL;

static void hook_processJobAltTrip(void* thisNMG, void* job)
{
	if (InterlockedCompareExchange(&g_processJobOwnerTid, 0, 0) != (long)GetCurrentThreadId())
		InterlockedIncrement(&navmesh::g_nmCache.nmTripCount);
	orig_processJobAltTrip(thisNMG, job);
}





// --------------------------------------------------------------------
// edgeProcess clone-guard
// --------------------------------------------------------------------
//
// finalize (0x3C2300) iterates wb+520 (overrideSettings) entries, calling
// edgeProcess per entry. A fresh WB's zeroed guard slot stops finalize's
// backward pop loop before it reads outside the array; appended entries
// reach normal cleanup. g_cloneProcessing has one zero definition and no
// writer in shipped code. The nonzero skip branch below is unarmed.

namespace nm_workers_detail {
// Always 0 in shipped code; hook_edgeProcess's nonzero branch is unarmed.
static volatile long g_cloneProcessing = 0;
edgeProcess_t orig_edgeProcess = NULL;
} // namespace nm_workers_detail
static volatile long g_edgeProcessLogged = 0;

static void __fastcall hook_edgeProcess(void* entry)
{
	if (InterlockedCompareExchange(&g_edgeProcessLogged, 1, 0) == 0)
		LogMsgDeferrable("edgeProcess hook fired (clone-guard armed)");

	if (InterlockedCompareExchange(&g_cloneProcessing, 0, 0) != 0)
	{
		InterlockedIncrement(&navmesh::g_nmCache.g_edgeProcessArmedCount);
		return;
	}
	InterlockedIncrement(&navmesh::g_nmCache.g_edgeProcessUnarmedCount);
	orig_edgeProcess(entry);
}



typedef void (*navMeshStop_t)(void* navMesh);
static navMeshStop_t orig_navMeshStop = NULL;


static void hook_navMeshStop(void* navMesh)
{
	// First, before the retire: from here on the game is tearing the navmesh
	// system down, and the crash recorder tags any record written after this
	// point (core.h). Set before the worker wait, because a fault during the
	// retire's slices belongs to the teardown too; set here, it also keeps the
	// DEV exit capture from arming when the retire ends the process at its cap.
	InterlockedExchange(&g_navMeshStopSeen, 1);
	// Threads queued for a generation slot see the stop now rather than at
	// their next poll, and skip the generation.
	MissParShutdownWake();
	RetireNavMeshWorkers();
	orig_navMeshStop(navMesh);
	// The stand-in records are plain malloc memory, read by the
	// hook and the prefetch on the NavMesh threads. After the original the
	// game's own NavMesh threads are joined, and the retire returned with no
	// worker live, so this test holds; it stays as the backstop.
	if (InterlockedCompareExchange(&navmesh::g_nmCache.g_navMeshWorkersLive, 0, 0) == 0)
		NbrSeedFreeTable();
}

// --------------------------------------------------------------------
// Lazy hook install (first dispatch)
// --------------------------------------------------------------------
//
// Hooks that need the Havok world live, installed on the first dispatchJob
// rather than from startPlugin: the edgeProcess clone-guard, the populate
// pass-through that reads each generation's input triangle count, and the
// processJobAlt ownership tripwire.
//
// The four hkFreeListAllocator wrappers that used to be installed here are
// gone. The Havok heap is per thread in front and locked behind
// its own critical section already, so the wrappers protected nothing — and one
// of them, RVA 0xBCCCB0, is the allocator's destructor, not garbageCollect, so
// wrapping it was a defect in its own right.
//
// A failed install logs once. Most only lose a counter; the collision
// builders and the neighbour-seed hook refuse their mode, and hook_dispatchJob
// refuses the worker pool without the NavMesh::stop or the buildCollision hook.
//
// Runs on the NavMesh bg thread, so every line (these, and the build gate's
// own lines from VerifyPrologue) goes through LogMsgDeferrable: no CRT strings
// or LogMsg here; the main thread writes them within a frame, in order.

namespace nm_workers_detail {
void InstallNavMeshLazyHooks()
{
	if (InterlockedCompareExchange(&navmesh::g_nmCache.lazyHooksInstalled, 1, 0) != 0)
		return;

	if (HookInstallRow(HOOK_EDGE_PROCESS, (void*)hook_edgeProcess, (void**)&orig_edgeProcess,
	                   NULL, true) == NULL)
		LogMsgDeferrable("edgeProcess clone-guard: installed");
	else
		LogMsgDeferrable("edgeProcess clone-guard: install FAILED");

	// Pass-through on NavMeshResult__populate, purely to read each generation's
	// input triangle count for the zero-face rule. Installed here rather than in
	// startPlugin so it shares the first-dispatch timing of the guard above.
	if (HookInstallRow(HOOK_NM_RESULT_POPULATE, (void*)hook_nmResultPopulate_diag,
	                   (void**)&game::g_hookOrig.orig_nmResultPopulate, NULL, true) == NULL)
		LogMsgDeferrable("populate hook: installed (input triangle counts)");
	else
		LogMsgDeferrable("populate hook: install FAILED (tri counts unavailable)");

	// Ownership tripwire on processJobAlt. Its prologue is
	// `mov rax, rsp` / `push rbp` / `push rsi` = exactly 5 relocatable bytes at
	// instruction boundaries, with no RIP-relative operand, so it is hookable.
	if (HookInstallRow(HOOK_PROCESS_JOB_ALT, (void*)hook_processJobAltTrip,
	                   (void**)&orig_processJobAltTrip, NULL, true) == NULL)
	{
		InterlockedExchange(&navmesh::g_nmCache.nmTripInstalled, 1);
		LogMsgDeferrable("processJobAlt tripwire: installed");
	}
	else
	{
		LogMsgDeferrable("processJobAlt tripwire: install FAILED (trip= reads off)");
	}

	// Retire the workers before the game tears the NavMesh down. Its prologue
	// is `test rcx, rcx` + a short `jz` into the body, so the 5 bytes contain a
	// relative branch that the trampoline has to relocate rather than copy —
	// the one hook in this file where that is true, and the reason the marker
	// below matters more here than elsewhere. A failed install logs once, and
	// the worker pool is then refused for the session.
	if (HookInstallRow(HOOK_NAVMESH_STOP, (void*)hook_navMeshStop, (void**)&orig_navMeshStop,
	                   NULL, true) == NULL)
	{
		InterlockedExchange(&g_navMeshStopHookInstalled, 1);
		LogMsgDeferrable("NavMesh::stop hook: installed (worker retirement)");
		ExitCaptureNoteStopHookOutcome(true);
	}
	else
	{
		LogMsgDeferrable("NavMesh::stop hook: install FAILED (worker pool refused)");
		ExitCaptureNoteStopHookOutcome(false);
	}

	// Collision builders and the narrow build-lock hooks.
	InstallBuildLockHooks();

	// The neighbour-seed pass-through. Here, before any
	// generation of the session and before the workers exist, so nothing runs
	// the target while it is patched. Installing it is diagnostic: a mismatch (or
	// a failed AddHook) turns the feature off for the session and reverts the
	// L2 settings hash to the value without the stand-in marker (NmNbrSeedStandInActive).
	if (NmNbrSeedHookWanted())
	{
		if (fn_navMeshGetSector
		    && HookInstallRow(HOOK_NMG_GET_SEED_POINTS_ADJ, (void*)hook_getSeedPointsAdj,
		                      (void**)&orig_getSeedPointsAdj, NULL, true) == NULL)
		{
			InterlockedExchange(&navmesh::g_nmCache.g_nbrSeedHookState, 1);
			char line[128];
			_snprintf_s(line, sizeof(line), _TRUNCATE,
				"neighbour-seed hook: installed (NMNBRSEED_STEP %d)", (int)2);
			LogMsgDeferrable(line);
			// Before any L2 access of the session: a refusal here reverts the
			// settings hash (NmNbrSeedStandInActive) before a file is read.
			if (!NbrCheckStandInCallees())
			{
				InterlockedExchange(&navmesh::g_nmCache.g_nbrSeedStandInRefused, 1);
				LogMsgDeferrable("neighbour seeds: stand-in REFUSED (a callee check failed); "
				                 "instrumentation only, L2 settings hash without nbrseed1");
			}
		}
		else
		{
			InterlockedExchange(&navmesh::g_nmCache.g_nbrSeedHookState, 2);
			LogMsgDeferrable("neighbour-seed hook: install FAILED (neighbour seeds off for the session"
			                 "; L2 settings hash without nbrseed1"
			                 ")");
		}
		// The DEV "L2 format:" line at startup
		// prints the hash for the install about to happen; this is the one the
		// session's L2 reads and writes use, now that the outcome is known.
		{
			char hl[128];
			_snprintf_s(hl, sizeof(hl), _TRUNCATE,
				"neighbour seeds: L2 settingsHash=%08x in effect (nbrSeed=%s)",
				L2SettingsHash(), NmNbrSeedStandInActive() ? "on" : "off");
			LogMsgDeferrable(hl);
		}
	}

	InterlockedExchange(&navmesh::g_nmCache.lazyHooksInstalled, 2);
}
} // namespace nm_workers_detail
