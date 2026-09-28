// nm_process_job_lock.cpp - processJobCS ownership and stop-aware entry on NavMesh threads.
// queue +152 is released before processJobCS; processJobCS precedes nmCacheCS.
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
// processJobCS ownership
// --------------------------------------------------------------------
//
// Every Enter/Leave of processJobCS goes through this pair so the thread id of
// the current owner is always known. The tripwire on processJobAlt
// reads it to tell "called under the lock" from "called without it".
//
// The critical section is recursive, but nothing here recurses on it: CloneNMG
// leaves before ProcessNavMeshJob enters, and the first-dispatch latch block
// and the orig_dispatchJob wrap are each a single flat region. A plain owner
// slot is therefore enough. t_pjDepth counts this thread's holds (every
// successful acquire adds one, LeaveProcessJobCS takes one away); the populate
// split releases the lock only when it reads exactly 1, so a hold it cannot
// see through is never dropped.
namespace nm_workers_detail {
volatile long g_processJobOwnerTid = 0;
__declspec(thread) int t_pjDepth = 0;
} // namespace nm_workers_detail

// Wakes a stop-aware waiter the moment processJobCS is released, so a poller
// takes the next release as promptly as a parked EnterCriticalSection waiter
// (CallOrigDispatchLocked, the first-dispatch latch) does. With a plain sleep a
// release would sit for the rest of the poller's sleep, a whole 15.6 ms tick at
// the default timer resolution, and the parked waiters would win nearly every
// release: the MISS threads would lose their turns to type 2/3/4 dispatches and
// the worker/bg MISS share would shift. Auto-reset: a release wakes one poller,
// and the others find the lock free at their next timeout. Signalled only while
// a poller is registered, so an uncontended release costs one interlocked read.
// Created in NavMeshMarkProcessJobLockReady; while NULL the loop sleeps instead.
namespace nm_workers_detail {
HANDLE        g_pjReleaseEvent = NULL;
volatile long g_pjPollers      = 0;
} // namespace nm_workers_detail
// The wait between two tries (plus timer granularity), and so the longest a
// poller takes to notice the stop.
static const DWORD   PJ_STOP_POLL_MS  = 2;



namespace nm_workers_detail {
// processJobCS for a MISS thread, unless NavMesh::stop is seen first. Returns
// true with the lock held (owner tid set exactly as EnterProcessJobCS sets it),
// or false without it once the stop is seen. Never blocks on the lock itself:
// between tries it waits on g_pjReleaseEvent for at most PJ_STOP_POLL_MS. The
// caller holds no lock (the same precondition as BackOffForPjPoll), and must
// not own processJobCS already: TryEnterCriticalSection would recurse.
bool EnterProcessJobCSStopAware()
{
	bool registered = false;
	bool held = false;
	for (;;)
	{
		if (NavMeshStopRequested())
			break;
		if (TryEnterCriticalSection(&processJobCS))
		{
			++t_pjDepth;
			InterlockedExchange(&g_processJobOwnerTid, (long)GetCurrentThreadId());
			MissParHolderSet(MP_HOLD_OTHER);
			held = true;
			break;
		}
		if (!registered)
		{
			// Register, then try again before the first wait: a release that
			// happened before the registration did not signal.
			InterlockedIncrement(&g_pjPollers);
			registered = true;
			continue;
		}
		HANDLE ev = g_pjReleaseEvent;
		if (ev)
			WaitForSingleObject(ev, PJ_STOP_POLL_MS);
		else
			Sleep(1);
	}
	if (registered)
		InterlockedDecrement(&g_pjPollers);
	return held;
}
} // namespace nm_workers_detail

// Only the clone path returns here: the bg swap path takes processJobCS back on
// its own (ReacquireForSwap), and only workers run a clone. Once the stop is
// seen the lock is not taken again: the thread returns into processJobAlt
// without it, and the rest of processJobAlt (revertSettings on the clone's own
// work buffer, the timing log line, the frees of its local geometry arrays)
// touches nothing processJobCS guards and only releases what the job holds,
// which the retire's handshake allows until the retire returns.
// ProcessNavMeshJob then drops the job (MissParLockLost).
int NavMeshReenterAfterGenerate()
{
	if (EnterProcessJobCSStopAware())
	{
		MissParHolderSet(MP_HOLD_WMISS);
		return NM_REENTER_HELD;
	}
	return WorkerCleanupBegin() ? NM_REENTER_STOPPED : NM_REENTER_RETIRED;
}



// --------------------------------------------------------------------
// processJobCS for the save-load reset
// --------------------------------------------------------------------
//
// nm_workers.h has the contract. The try loop never blocks for longer than
// timeoutMs, so a holder that is itself waiting on the main thread (whatever
// the reason) costs the reset at most the bound, never a hang. On success the
// owner tid is set exactly as EnterProcessJobCS sets it, and the release goes
// through LeaveProcessJobCS, so the processJobAlt tripwire reads the
// same bookkeeping as for every other holder.
//
// Readiness is an explicit flag set by startPlugin right after
// InitNavMeshCacheCS, rather than a guess from the CRITICAL_SECTION's internals.
namespace nm_workers_detail {
volatile long g_pjLockReady = 0;
} // namespace nm_workers_detail

void NavMeshMarkProcessJobLockReady()
{
	// The stop-aware waiters' release signal. Created before the ready flag
	// and before any NavMesh thread exists, so every reader sees it set. Never
	// closed: it lives as long as the process, and a failure only makes the
	// pollers sleep instead (EnterProcessJobCSStopAware).
	if (!g_pjReleaseEvent)
		g_pjReleaseEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	InterlockedExchange(&g_pjLockReady, 1);
}

namespace nm_workers_detail {
// A bounded poll wants processJobCS. A Sleep(1)
// poll almost never wins against the MISS threads: each worker takes the lock
// twice per MISS (CloneNMG, then missLock) and a parked EnterCriticalSection
// waiter re-takes it within microseconds of every release. So while a poll is
// running, the MISS entry points back off before entering (BackOffForPjPoll)
// and the poll gets the next release.
//
// A count rather than a flag, in case two pollers ever overlap (the reset hook
// can run off the main thread). Raised and lowered only by
// NavMeshTryLockProcessJobFor around its polling loop, never for a timeout of 0
// (one TryEnter has nothing to wait for).
static volatile long g_pjPollWanting = 0;


// Backstop only: the poller always lowers g_pjPollWanting within its own
// timeout (the save-load reset uses 10 s). This bounds the back-off if a poller
// ever failed to, so a MISS can never be parked here indefinitely.
static const long PJ_BACKOFF_MAX_US = 15000000;

// The unload priority request (NavMeshRequestPjPriority): QPC value at
// which the raised request expires (0 = none), and the earliest QPC value at
// which a new one may be raised. Written by the main thread only, read by the
// MISS threads in BackOffForPjPoll.
static const LONGLONG PJ_PRIO_TTL_US = 2000000;   // a request lives 2 s
static volatile LONGLONG g_pjPrioUntil     = 0;
static volatile LONGLONG g_pjPrioNextAllow = 0;

static inline bool PjPrioActiveAt(LONGLONG now)
{
	LONGLONG until = InterlockedCompareExchange64(&g_pjPrioUntil, 0, 0);
	return until != 0 && now < until;
}

// Called by a MISS thread right before it would enter processJobCS: CloneNMG's
// snapshot lock and ProcessNavMeshJob's missLock, on the workers and the bg
// thread. Yields while a poll wants the lock (or while the main thread's
// unload priority request is up), then returns and the caller enters as
// before; nothing is skipped or reordered, the MISS only starts later.
//
// It only holds back threads that arrive while the request stands. Threads
// already waiting (missLock and CloneNMG waiters, which poll in
// EnterProcessJobCSStopAware rather than park, and CallOrigDispatchLocked and
// the first-dispatch latch, which park and never back off) still compete for
// the next release, so the poller is not guaranteed that release: it wins the
// first one no other waiter takes. Bounded either way.
//
// Deadlock-free:
//   - the caller holds no lock at either call site: CloneNMG is entered with
//     nothing held (WorkerTryDequeueAny released +152 and nmCacheCS), and
//     missLock's callers hold nothing (hook_dispatchJob released +152 and the
//     first-dispatch latch; the worker loop holds nothing); in particular it
//     never holds processJobCS, which the owner check below enforces anyway,
//     so the poller can always acquire once the current holder leaves;
//   - the poller waits on nothing this thread holds, and it is bounded;
//   - the wait itself is bounded (the poller's timeout, PJ_BACKOFF_MAX_US as
//     a backstop, and worker shutdown).
void BackOffForPjPoll()
{
	// The steady state: one interlocked read each (the request's deadline is 0).
	if (!InterlockedCompareExchange(&g_pjPollWanting, 0, 0)
	    && !InterlockedCompareExchange64(&g_pjPrioUntil, 0, 0))
		return;
	LONGLONG start = QpcNow();
	if (!InterlockedCompareExchange(&g_pjPollWanting, 0, 0) && !PjPrioActiveAt(start))
		return;   // an expired request: nothing to stand aside for
	// Never wait while owning processJobCS: the poller could not get it until
	// this thread let go, so the wait would only burn the poller's timeout.
	if (InterlockedCompareExchange(&g_processJobOwnerTid, 0, 0) == (long)GetCurrentThreadId())
		return;

	InterlockedIncrement(&nmPjYieldCount);
	for (;;)
	{
		LONGLONG now = QpcNow();
		if (!InterlockedCompareExchange(&g_pjPollWanting, 0, 0) && !PjPrioActiveAt(now))
			break;
		// NavMesh::stop as well: the stop-aware wait the caller
		// goes on to then returns at once without the lock.
		if (NavMeshStopRequested())
			break;
		if (QpcDeltaUs(start, now) >= PJ_BACKOFF_MAX_US)
			break;
		Sleep(1);
	}
}
} // namespace nm_workers_detail

bool NavMeshRequestPjPriority()
{
	LONGLONG now = QpcNow();
	if (PjPrioActiveAt(now))
		return false;                                     // already up: never extended
	if (now < InterlockedCompareExchange64(&g_pjPrioNextAllow, 0, 0))
		return false;                                     // cool-down after an expiry
	InterlockedExchange64(&g_pjPrioNextAllow, now + QpcFromUs(2 * PJ_PRIO_TTL_US));
	InterlockedExchange64(&g_pjPrioUntil, now + QpcFromUs(PJ_PRIO_TTL_US));
	InterlockedIncrement(&nmUlPrio);
	return true;
}

bool NavMeshPjPriorityActive()
{
	return PjPrioActiveAt(QpcNow());
}

NavMeshPjLockResult NavMeshTryLockProcessJobFor(DWORD timeoutMs, DWORD* waitedMs)
{
	if (waitedMs) *waitedMs = 0;
	if (!InterlockedCompareExchange(&g_pjLockReady, 0, 0))
		return NM_PJLOCK_NONE;

	// Timeout 0: exactly one TryEnterCriticalSection, no back-off request.
	if (timeoutMs == 0)
	{
		if (TryEnterCriticalSection(&processJobCS))
		{
			++t_pjDepth;
			InterlockedExchange(&g_processJobOwnerTid, (long)GetCurrentThreadId());
			MissParHolderSet(MP_HOLD_OTHER);
			// The unload priority request has done its job. Won while
			// still up (not expired) counts ulPrioWin=; either way it is
			// withdrawn and the next deferral may raise a fresh one at once.
			if (PjPrioActiveAt(QpcNow()))
				InterlockedIncrement(&nmUlPrioWin);
			InterlockedExchange64(&g_pjPrioUntil, 0);
			InterlockedExchange64(&g_pjPrioNextAllow, 0);
			return NM_PJLOCK_HELD;
		}
		// A MISS (or a type 2/3/4 dispatch) holds it. During a mod unload that
		// defers the unload to a later frame (ulSkipPj=).
		if (UnloadingZone())
			InterlockedIncrement(&nmUlSkipPj);
		return NM_PJLOCK_TIMEOUT;
	}

	// Ask the MISS threads to stand aside for the duration of the poll. The
	// request is withdrawn on both exits; once the lock is held they simply
	// block in EnterCriticalSection as they would behind any holder.
	InterlockedIncrement(&g_pjPollWanting);

	LONGLONG start = QpcNow();
	for (;;)
	{
		if (TryEnterCriticalSection(&processJobCS))
		{
			++t_pjDepth;
			InterlockedExchange(&g_processJobOwnerTid, (long)GetCurrentThreadId());
			MissParHolderSet(MP_HOLD_OTHER);
			InterlockedDecrement(&g_pjPollWanting);
			if (waitedMs) *waitedMs = (DWORD)(QpcDeltaUs(start, QpcNow()) / 1000);
			return NM_PJLOCK_HELD;
		}
		DWORD elapsedMs = (DWORD)(QpcDeltaUs(start, QpcNow()) / 1000);
		if (elapsedMs >= timeoutMs)
		{
			InterlockedDecrement(&g_pjPollWanting);
			if (UnloadingZone())
				InterlockedIncrement(&nmUlSkipPj);
			if (waitedMs) *waitedMs = elapsedMs;
			return NM_PJLOCK_TIMEOUT;
		}
		Sleep(1);
	}
}

void NavMeshUnlockProcessJob()
{
	LeaveProcessJobCS();
}

// The populate split's view of processJobCS (nm_cache_core.cpp).
int  NavMeshProcessJobDepth()      { return t_pjDepth; }
void NavMeshLeaveForGenerate()     { LeaveProcessJobCS(); }
bool NavMeshStopSeen()             { return NavMeshStopRequested(); }
