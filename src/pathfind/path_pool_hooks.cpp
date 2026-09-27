// path_pool_hooks.cpp - timed pass-through path and gate hooks.
// Path and AI back threads, and the main thread gate pass; detours add no lock, allocation or log.

#include "pathfind/path_pool_internal.h"

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
		InterlockedExchangeAdd64(&g_depthSum, (LONGLONG)depth);
		InterlockedIncrement(&g_depthSamples);
		for (;;)
		{
			LONG cur = g_depthMax;
			if (depth <= cur) break;
			if (InterlockedCompareExchange(&g_depthMax, depth, cur) == cur) break;
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
	char served = navMeshUpdateGuardEnabled
		? NavMeshUpdateGuardCall(sectionMgr, orig_contentStream)
		: orig_contentStream(sectionMgr);

	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);

	InterlockedIncrement(&g_passCount);
	LONGLONG passUsThisPass = QpcToUs(t1.QuadPart - t0.QuadPart);
	InterlockedExchangeAdd64(&g_passTotalUs, passUsThisPass);
	InterlockedExchangeAdd64(&g_busyPassTotalUs, passUsThisPass);

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
		InterlockedIncrement(&g_arrivedCount);
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
			PPHistAdd(&g_waitHist, waitUs);

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
			PPHistAdd(&g_svcHist, svcUs);
			InterlockedExchangeAdd64(&g_svcTotalUs, svcUs);

			LONG pri = *(LONG*)((char*)req + REQ_PRIORITY_OFFSET);
			if (pri >= 45)      InterlockedIncrement(&g_priTierCount);
			else if (pri == 20) InterlockedIncrement(&g_priPlayerCount);
			else if (pri <= 10) InterlockedIncrement(&g_priNpcCount);

			// req+0x90, one counter per raw status.
			if (status >= 0 && status <= 4)
				InterlockedIncrement(&g_reqStatusCount[status]);

			// "direct=": a status-0 completion this pass, with
			// no path-thread search having run this pass, took the step-4
			// csFindPath success path rather than the step-7 fallback/full
			// search. g_passSearchCount is path-thread-only, read here on
			// the path thread in the same pass PathPoolNoteSearch (if any)
			// already ran in, so no synchronization is needed.
			if (status == 0 && g_passSearchCount == 0)
				InterlockedIncrement(&g_directCount);

			InterlockedIncrement(&g_servedCount);

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

	InterlockedIncrement(&g_gatePassCount);
	InterlockedExchangeAdd64(&g_gatePassTotalUs, us);
	// Same 32-bit clamp as PPHistAdd's maximum.
	LONG usClamped = (us > 0x7FFFFFFF) ? (LONG)0x7FFFFFFF : (LONG)us;
	for (;;)
	{
		LONG cur = g_gatePassMaxUs;
		if (usClamped <= cur) break;
		if (InterlockedCompareExchange(&g_gatePassMaxUs, usClamped, cur) == cur) break;
	}
	if (inTx)
		InterlockedIncrement(&g_gatePassInTransition);

	return result;
}
