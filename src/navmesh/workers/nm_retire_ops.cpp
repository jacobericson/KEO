// nm_retire_ops.cpp - main-thread worker retirement in the last-frame callback.
// bounded logCS only; every wait runs without a lock.
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
namespace nm_workers_detail
{

// The retire's view of the pool: the one handle snapshot and each handle's
// slot. Never a NULL handle (WorkerSlotHandle filters them), since
// WaitForMultipleObjects fails the whole array on one invalid handle.
struct RetireCtx
{
	int    activeCount;
	HANDLE active[NAVMESH_WORKER_COUNT];
	int    slot[NAVMESH_WORKER_COUNT];
};

unsigned RetireOpNowMs(void*)
{
	return (unsigned)(ElapsedSec() * 1000.0);
}

// The larger of the live counter and the snapshot threads still running: the
// counter drops just before a worker's last line and its return, and it also
// covers a thread whose handle can no longer be waited on.
int RetireOpLiveCount(void* ctx)
{
	const RetireCtx* c = (const RetireCtx*)ctx;
	int running = 0;
	for (int i = 0; i < c->activeCount; ++i)
		if (WaitForSingleObject(c->active[i], 0) == WAIT_TIMEOUT)
			++running;
	const long counted = InterlockedCompareExchange(&g_navMeshWorkersLive, 0, 0);
	return counted > running ? (int)counted : running;
}

RetireWait RetireOpWaitSlice(void* ctx, unsigned ms, unsigned long* gle)
{
	const RetireCtx* c = (const RetireCtx*)ctx;
	const DWORD w = WaitForMultipleObjects((DWORD)c->activeCount, c->active, TRUE, ms);
	if (w - WAIT_OBJECT_0 < (DWORD)c->activeCount)
		return RETIRE_WAIT_JOINED;
	if (w == WAIT_TIMEOUT)
		return RETIRE_WAIT_TIMEOUT;
	*gle = GetLastError();   // at once: anything in between would overwrite it
	// A failed wait still gives the workers the slice: poll the live count until
	// the slice has passed or no worker is live.
	const unsigned t0 = RetireOpNowMs(ctx);
	while (RetireOpNowMs(ctx) - t0 < ms && RetireOpLiveCount(ctx) > 0)
		Sleep(50);
	return RETIRE_WAIT_FAILED;
}

long RetireOpStopDrops(void*)
{
	return InterlockedCompareExchange(&g_nmStopDropCount, 0, 0);
}

// "w<slot>:<phase>" for each snapshot thread still running, space-separated.
void RetireOpPhases(void* ctx, char* out, size_t cap)
{
	const RetireCtx* c = (const RetireCtx*)ctx;
	if (cap == 0)
		return;
	out[0] = 0;
	size_t used = 0;
	for (int i = 0; i < c->activeCount; ++i)
	{
		if (WaitForSingleObject(c->active[i], 0) != WAIT_TIMEOUT)
			continue;
		const LONG phase = InterlockedCompareExchange(&g_workerPhase[c->slot[i]], 0, 0);
		const int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "%sw%d:%s",
			used ? " " : "", c->slot[i], NavMeshWorkerPhaseName(phase));
		if (n < 0)
			break;   // truncated, and still terminated
		used += (size_t)n;
	}
}

bool RetireOpLog(void*, const char* line)
{
	return LogMsgBounded(line, RETIRE_LOG_BOUND_MS);
}

void RetireOpLogFallback(void*, const char* line)
{
	LogRetireFallback(line);
}

void RetireOpTerminate(void*, unsigned code)
{
	TerminateProcess(GetCurrentProcess(), code);
	// Reached only if that call failed: this still ends every worker before any
	// detach runs, and the navmesh teardown never starts.
	ExitProcess(code);
}

RetireOps RetireRealOps(RetireCtx* ctx)
{
	RetireOps ops;
	ops.ctx         = ctx;
	ops.nowMs       = RetireOpNowMs;
	ops.liveCount   = RetireOpLiveCount;
	ops.waitSlice   = RetireOpWaitSlice;
	ops.stopDrops   = RetireOpStopDrops;
	ops.phases      = RetireOpPhases;
	ops.log         = RetireOpLog;
	ops.logFallback = RetireOpLogFallback;
	ops.terminate   = RetireOpTerminate;
	return ops;
}

} // namespace nm_workers_detail
using namespace nm_workers_detail;

// Retires the worker pool before the game tears the NavMesh down: returns only
// once no worker is live; at the cap it ends the process. NavMesh::stop
// (0x3AAE90) clears +0x1C8, joins the game's own NavMesh threads without a
// bound, deletes the generator and shuts the Havok memory system down, so a
// worker still running there would read freed state.
//
// The wait runs in slices (nm_retire_policy.h): one line per slice spent, each
// reading joined=HANG from the report threshold, and at the cap the final
// record, then the process ends with no exception dispatched (no crash record,
// no teardown). Main thread, inside the game's last frame callback. The one
// lock is logCS, taken bounded through LogMsgBounded (pendingLogCS under it
// for the deferred lines, which the workers' "exiting" lines need: no frame
// follows to flush them); after one refused take every later line goes to the
// fallback file. Nothing waits under a lock: the slices, the failed-wait poll
// and the drain below hold none.
void RetireNavMeshWorkers()
{
	if (InterlockedExchange(&g_workerShutdown, 1) != 0)
		return;   // already retired

	// Manual-reset: one set releases every waiter and stays signalled, and the
	// workers' clear path skips the reset while shutdown is set.
	if (g_jobEvent)
		SetEvent(g_jobEvent);

	RetireCtx ctx;
	ctx.activeCount = 0;
	for (int i = 0; i < NAVMESH_WORKER_COUNT; ++i)
	{
		// One snapshot per slot, and never a NULL into the array.
		HANDLE h = WorkerSlotHandle(i);
		if (h)
		{
			ctx.active[ctx.activeCount] = h;
			ctx.slot[ctx.activeCount] = i;
			++ctx.activeCount;
		}
	}

	const RetireOps ops = RetireRealOps(&ctx);
	const RetireResult r = RetireRun(ops);

	// No worker is live, and a worker's last release ends before its live count
	// drops, so no worker release is in flight; WorkerCleanupBegin refuses from
	// here on, as the backstop. A release that began before this line takes the
	// queue lock for its byte write and then frees; every holder of that lock
	// keeps it for a short region and none waits on a worker, so after a retire
	// that had to wait it gets a short bounded wait of its own (cleanupLeft= on
	// the line reports one still in flight).
	InterlockedExchange(&g_retireReturned, 1);
	long cleanupLeft = 0;
	if (r.slices > 0 || r.anyWaitFailed)
	{
		const DWORD WORKER_CLEANUP_DRAIN_MS = 500;
		double c0 = ElapsedSec();
		for (;;)
		{
			cleanupLeft = InterlockedCompareExchange(&g_workerCleanupInFlight, 0, 0);
			if (cleanupLeft <= 0 || (ElapsedSec() - c0) * 1000.0 >= WORKER_CLEANUP_DRAIN_MS)
				break;
			Sleep(1);
		}
	}

	RetireSummary s;
	s.activeCount   = ctx.activeCount;
	s.waitMs        = r.waitMs;
	s.anyWaitFailed = r.anyWaitFailed;
	s.lastGle       = r.lastGle;
	s.live          = (int)InterlockedCompareExchange(&g_navMeshWorkersLive, 0, 0);
	// Jobs dropped because the stop was seen (after the claim, after
	// CloneNMG, after missLock; workers and the bg thread). 0 at an idle quit.
	s.stopDrop      = InterlockedCompareExchange(&g_nmStopDropCount, 0, 0);
	s.cleanupLeft   = cleanupLeft;
	char line[RETIRE_LINE_CHARS];
	RetireFormatRetiredLine(line, sizeof(line), s);
	bool logStuck = r.logStuck;
	RetireEmit(ops, &logStuck, line);
}
