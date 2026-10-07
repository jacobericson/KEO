// path_pool_hooks.cpp - timed pass-through path and gate hooks.
// Path-thread passes and any-thread queue detours add no lock, allocation or log.

#include "pathfind/path_pool_internal.h"

namespace path_pool_hooks_detail {
// Sets t_inContentStreamPass or t_inGatePass for the lifetime of a hook call
// and clears it on every exit, including a C++ exception unwinding out of the
// original (a flag left at 1 would attribute every later path-thread search to
// the gate counters, or hide the "outside a pass" warning for good). Used only
// in the two hooks, which have no __try, so a destructor is allowed there.
struct PPTlsFlagScope
{
	int* flag;
	explicit PPTlsFlagScope(int* f) : flag(f) { *flag = 1; }
	~PPTlsFlagScope() { *flag = 0; }
private:
	PPTlsFlagScope(const PPTlsFlagScope&);
	PPTlsFlagScope& operator=(const PPTlsFlagScope&);
};
} // namespace path_pool_hooks_detail
using namespace path_pool_hooks_detail;

namespace path_pool_detail {
PathPassWindow g_ppWindow;
} // namespace path_pool_detail
namespace path_pool_hooks_detail {
union PathPassWindowPodCheck { path_pool_detail::PathPassWindow s; };
static_assert(__alignof(path_pool_detail::PathPassWindow) >= 8, "PathPassWindow must be 8-byte aligned");
} // namespace path_pool_hooks_detail
namespace path_pool_detail {


static const size_t REQ_STAMP_OFFSET    = 0x00;   // never written by the game
static const size_t REQ_PRIORITY_OFFSET = 0x2C;   // 10 NPC, 20 player, 45 mod tier
static const size_t REQ_STATUS_OFFSET   = 0x90;   // -10 sentinel
static const size_t REQ_START_OFFSET    = 0x40;   // start position (3 floats)
static const size_t REQ_GOAL_OFFSET     = 0x50;   // goal position, offset-free once resolved

static const int REQ_STATUS_SENTINEL = -10;

// Path hook_contentStream publishes the current manager by pointer exchange
// every pass. Dequeue/enqueue filters read its aligned pointer on their
// calling thread. Starts NULL, replaced after manager changes; never torn,
// but not a coherent pair with the thread id or a lifetime guarantee.
static void* volatile g_sectionMgrPtr = NULL;

// Set for the duration of hook_contentStream so hook_gatesUpdateCodes (its
// sole caller, IDA-verified) can confirm it is really running inside a pass.
static __declspec(thread) int t_inContentStreamPass = 0;

// Set for the duration of hook_gatesUpdateCodes; PathPoolNoteSearch reads it
// to attribute path-thread searches to the gate counters instead of the
// queue counters.
__declspec(thread) int t_inGatePass = 0;

// QPC at the start of the pass currently in progress on the path thread.
// Single-threaded (only ever written/read by the path thread, from nested
// calls within the same contentStream invocation), so a plain global is
// safe -- no other thread touches it.
static LARGE_INTEGER g_passStartTicks = { 0 };

// "svc" must not include the pass preamble
// (origin shift, drainResults, work items, section adds, the gate pass,
// the request drain), or PathSlow ranks whole passes -- a gate pass can run
// up to 929 ms -- instead of searches. The tightest lower bound the four
// hooks can give for "serve one request began" is the LATEST of: pass
// start, the gate pass's own end (if one ran this pass) and the last
// dequeueWork return this pass (request drain, which immediately
// precedes serve). Both reset to the pass start at the top of every
// pass; hook_gatesUpdateCodes and hook_dequeueWork push them later only if
// they actually run. Plain globals: path-thread-only, same reasoning as
// g_passStartTicks above.
static LARGE_INTEGER g_gatePassEndTicks  = { 0 };
static LARGE_INTEGER g_lastDequeueTicks  = { 0 };


// Latched by PathPoolNoteSearch on the path thread for the last non-gate
// search this pass; hook_enqueueThreadSafe reads it for the PathSlow
// iterations field. Path-thread-only, like the ticks above.
//
// Reset to -1 at the top of every pass (with g_gatePassEndTicks/
// g_lastDequeueTicks), not left holding a stale value across passes.
// On the path thread outside gate passes the only
// caller of findPathFull is findPathFallback (serve step 7); a completion
// that ends at step 3 (status 1, no start face), step 4 (direct csFindPath
// success, status 0) or step 5 (status 2, no goal face) runs no search at
// all this pass, so without the reset PathSlow's iter= would silently show
// another, possibly many-passes-old, request's iteration count. -1 prints
// as "-" (no search ran for this completion).
LONG g_lastPathIterations = -1;

// "direct=" needs to know whether ANY path-thread, non-gate search ran
// during the pass that produced a given completion -- a status-0 completion
// with none is the csFindPath direct path (RunPathRequest step 4), not the
// fallback/full-search path (step 7). Reset alongside g_lastPathIterations;
// incremented by PathPoolNoteSearch.
LONG g_passSearchCount = 0;



} // namespace path_pool_detail
using namespace path_pool_detail;

// =========================================================================
// Hooks
// =========================================================================

char hook_contentStream(void* sectionMgr)
{
	// Unconditional re-latch every pass, not just the first (a
	// compare-exchange against 0 would never update after a recreated path
	// thread, e.g. after a save load that rebuilds the SectionManager). The
	// write is free next to a QueryPerformanceCounter call either way.
	InterlockedExchange((volatile LONG*)&g_pathThreadId, (LONG)GetCurrentThreadId());
	InterlockedExchangePointer(&g_sectionMgrPtr, sectionMgr);

	// Set for the whole call, cleared on return or unwind.
	PPTlsFlagScope passScope(&t_inContentStreamPass);

	// Queue depth at pass start, on the path thread -- a racy main-thread
	// read is not needed since we already sample here every pass.
	if (sectionMgr)
	{
		LONG depth = *(LONG*)(KLIB_MEMBER(5, (char*)sectionMgr, NavMesh_pathRequests_m_size, PATHQ_DEPTH_OFFSET));
		if (depth < 0) depth = 0;
		InterlockedExchangeAdd64(&g_ppWindow.g_depthSum, (LONGLONG)depth);
		InterlockedIncrement(&g_ppWindow.g_depthSamples);
		for (;;)
		{
			LONG cur = g_ppWindow.g_depthMax;
			if (depth <= cur) break;
			if (InterlockedCompareExchange(&g_ppWindow.g_depthMax, depth, cur) == cur) break;
		}
	}

	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	g_passStartTicks = t0;
	// Reset this pass's tightened serve-start trackers to the pass start;
	// hook_gatesUpdateCodes / hook_dequeueWork push them later only if they
	// actually run during this pass.
	g_gatePassEndTicks = t0;
	g_lastDequeueTicks = t0;
	// Reset per pass, not left holding a stale value from a previous pass
	// that may not have run any search at all this pass.
	g_lastPathIterations = -1;
	g_passSearchCount = 0;

	// A fault inside the pass kills this thread with the SectionManager's
	// changeMutex still held exclusively. The guard does not undo that; it
	// records the fault and the lock state before the process reacts to it.
	char served = fixes::g_fixesCfg.navMeshUpdateGuardEnabled
		? NavMeshUpdateGuardCall(sectionMgr, orig_contentStream)
		: orig_contentStream(sectionMgr);

	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);

	InterlockedIncrement(&g_ppWindow.g_passCount);
	LONGLONG passUsThisPass = QpcToUs(t1.QuadPart - t0.QuadPart);
	InterlockedExchangeAdd64(&g_ppWindow.g_passTotalUs, passUsThisPass);
	InterlockedExchangeAdd64(&g_ppWindow.g_busyPassTotalUs, passUsThisPass);

	return served;   // ~passScope clears t_inContentStreamPass
}

void* hook_dequeueWork(void* queueBase)
{
	void* req = orig_dequeueWork(queueBase);

	void* mgr = g_sectionMgrPtr;
	if (req && mgr && queueBase == (void*)KLIB_MEMBER(5, (char*)mgr, NavMesh_characterMessages, PATHQ_INPUT_OFFSET))
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		*(LONGLONG*)((char*)req + REQ_STAMP_OFFSET) = now.QuadPart;
		InterlockedIncrement(&g_ppWindow.g_arrivedCount);
		// The LAST dequeue in the drain loop is the tightest bound this hook
		// can give for "the request drain just finished" -- the step
		// immediately before serve. Plain global, path-thread-only, like
		// g_passStartTicks.
		g_lastDequeueTicks = now;
	}
	return req;
}

void hook_enqueueThreadSafe(void* queueBase, void** itemPtr)
{
	void* mgr = g_sectionMgrPtr;
	void* req = itemPtr ? *itemPtr : NULL;

	if (req && mgr && queueBase == (void*)KLIB_MEMBER(5, (char*)mgr, NavMesh_completedMessages, PATHQ_RESULT_OFFSET))
	{
		LONG status = *(LONG*)((char*)req + REQ_STATUS_OFFSET);
		LONGLONG stamp = *(LONGLONG*)((char*)req + REQ_STAMP_OFFSET);

		if (status != REQ_STATUS_SENTINEL && stamp != 0)
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);

			LONGLONG waitUs = QpcToUs(now.QuadPart - stamp);
			PPHistAdd(&g_ppWindow.g_waitHist, waitUs);

			// serve-start = the latest of pass start, this pass's
			// gate-pass end and this pass's last dequeue -- excludes the
			// origin shift / drainResults / work items / section adds / gate
			// pass / request drain preamble from "svc", so PathSlow ranks
			// searches, not whole passes (a gate pass alone can run to 929 ms).
			LONGLONG serveStart = g_passStartTicks.QuadPart;
			if (g_gatePassEndTicks.QuadPart > serveStart) serveStart = g_gatePassEndTicks.QuadPart;
			if (g_lastDequeueTicks.QuadPart > serveStart) serveStart = g_lastDequeueTicks.QuadPart;
			LONGLONG svcTicks = now.QuadPart - serveStart;
			if (svcTicks < 0) svcTicks = 0;
			LONGLONG svcUs = QpcToUs(svcTicks);
			PPHistAdd(&g_ppWindow.g_svcHist, svcUs);
			InterlockedExchangeAdd64(&g_ppWindow.g_svcTotalUs, svcUs);

			LONG pri = *(LONG*)((char*)req + REQ_PRIORITY_OFFSET);
			if (pri >= 45)      InterlockedIncrement(&g_ppWindow.g_priTierCount);
			else if (pri == 20) InterlockedIncrement(&g_ppWindow.g_priPlayerCount);
			else if (pri <= 10) InterlockedIncrement(&g_ppWindow.g_priNpcCount);
#ifdef KEO_DEBUG
			PPHistAdd(pri >= 20 ? &g_ppWindow.g_waitHistPlayer : &g_ppWindow.g_waitHistNpc, waitUs);
#endif

			// req+0x90, one counter per raw status.
			if (status >= 0 && status <= 4)
				InterlockedIncrement(&g_ppWindow.g_reqStatusCount[status]);

			// "direct=": a status-0 completion this pass, with
			// no path-thread search having run this pass, took the step-4
			// csFindPath success path rather than the step-7 fallback/full
			// search. g_passSearchCount is path-thread-only, read here on
			// the path thread in the same pass PathPoolNoteSearch (if any)
			// already ran in, so no synchronization is needed.
			if (status == 0 && g_passSearchCount == 0)
				InterlockedIncrement(&g_ppWindow.g_directCount);

			InterlockedIncrement(&g_ppWindow.g_servedCount);

			float* startPos = (float*)((char*)req + REQ_START_OFFSET);
			float* goalPos  = (float*)((char*)req + REQ_GOAL_OFFSET);
			// g_lastPathIterations is path-thread-only (like this pass's timing
			// trackers), so reading it here -- itself on the path
			// thread, in the same pass that just produced this completion
			// -- is safe without synchronization. -1 (reset every pass,
			// see its declaration) means no search ran this pass.
			PPSlowConsider(svcUs, status, pri, g_lastPathIterations,
			               startPos[0], startPos[2], goalPos[0], goalPos[2]);
		}
	}

	orig_enqueueThreadSafe(queueBase, itemPtr);
}

__int64 hook_gatesUpdateCodes(void* gatesObj)
{
	// No LogMsg on the path thread here (reachable if contentStream's own
	// hook failed to install while this one still did). Set the flag only;
	// PathPoolTickMain reports it once from the main thread.
	if (!t_inContentStreamPass)
		InterlockedCompareExchange(&g_gateOutsideStreamWarned, 1, 0);

	bool inTx = isTransitionActive;

	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	GatePassBegin(gatesObj, t0.QuadPart);

	__int64 result;
	{
		PPTlsFlagScope gateScope(&t_inGatePass);   // cleared on return or unwind
		result = orig_gatesUpdateCodes(gatesObj);
	}

	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	GatePassEnd(t1.QuadPart);
	LONGLONG us = QpcToUs(t1.QuadPart - t0.QuadPart);

	// Push the tightened serve-start bound past this gate pass.
	g_gatePassEndTicks = t1;

	InterlockedIncrement(&g_ppWindow.g_gatePassCount);
	InterlockedExchangeAdd64(&g_ppWindow.g_gatePassTotalUs, us);
	// Same 32-bit clamp as PPHistAdd's maximum.
	LONG usClamped = (us > 0x7FFFFFFF) ? (LONG)0x7FFFFFFF : (LONG)us;
	for (;;)
	{
		LONG cur = g_ppWindow.g_gatePassMaxUs;
		if (usClamped <= cur) break;
		if (InterlockedCompareExchange(&g_ppWindow.g_gatePassMaxUs, usClamped, cur) == cur) break;
	}
	if (inTx)
		InterlockedIncrement(&g_ppWindow.g_gatePassInTransition);

	return result;
}
