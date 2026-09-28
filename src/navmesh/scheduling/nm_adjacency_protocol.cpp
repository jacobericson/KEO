// nm_adjacency_protocol.cpp - navmesh adjacency claim and wait protocol.
// Locked scan paths use caller-held generator queue +152 and take no lock beneath it;
// the bg wait takes no lock and checks stop, while unpin takes +152 itself.
#include "navmesh/scheduling/nm_adjacency_internal.h"
#include <string.h>
namespace nm_adjacency_detail {
static const size_t OFF_NMG_THREAD_RUNNING = 0x108;
static const LONGLONG kAgeAfterMs = 100;   // a worker-skipped job is reserved after this
static const DWORD    kBgStopPollMs = 10;  // longest wait before the stop flags are read again
static const LONGLONG kBgSliceMs  = 40;    // then the bg thread idles in threadProc's sleep
} // namespace nm_adjacency_detail
using namespace nm_adjacency_detail;
static void NoteDeferral(LONGLONG waited);
// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

void NmAdjWorkerScanBeginLocked(NmAdjScan* s)
{
	NmAdjScanBegin(s, &g_reg, s_mode == MODE_ENFORCE, QpcNow());
}

NmAdjOffer NmAdjWorkerOfferLocked(NmAdjScan* s, uintptr_t task, bool eligible, NmJobDesc* descOut)
{
	if (task == (uintptr_t)g_reg.pin)
		eligible = false;
	if (!eligible)
	{
		NmAdjScanObserve(s, task, false);
		return NMADJ_OFFER_PASS;
	}
	NmAdjDescribeTask(task, descOut);
	return NmAdjScanOffer(s, &g_reg, task, descOut, true);
}

void NmAdjWorkerObserveLocked(NmAdjScan* s, uintptr_t task, bool eligible)
{
	NmAdjScanObserve(s, task, eligible && task != (uintptr_t)g_reg.pin);
}

bool NmAdjWorkerScanEndLocked(NmAdjScan* s, int workerId, const NmJobDesc* takenDesc, bool canReserve)
{
	const bool took = s->took;
	const bool conflicting = s->tookConflicting;
	const unsigned __int64 taken = s->taken;
	NmAdjScanResult r;
	WBegin();
	NmAdjScanEnd(s, &g_reg, workerId, takenDesc, kAgeAfterMs * s_qpf / 1000, canReserve, &r);
	WEnd();

	if (s->skips)
		InterlockedExchangeAdd(&s_skips, s->skips);
	if (r.reservedAge) InterlockedIncrement(&s_resAge);
	if (r.droppedAge)  InterlockedIncrement(&s_resDrop);
	if (r.full)        InterlockedIncrement(&s_full);
	if (!took || r.refused)
		return !r.refused;

	if (conflicting) InterlockedIncrement(&s_would);
	if (s->skips)    InterlockedIncrement(&s_skipClaims);
	if (r.deferredTaken)
		NoteDeferral(r.deferredFor);
	InterlockedIncrement(&s_claims);
	t_own = r.claimIdx;
	t_ownTask = r.claimIdx >= 0 ? taken : 0;
	return true;
}

void NmAdjAfterScan(const NmAdjScan* s)
{
	// A dropped or claimed age reservation unblocks others.
	if (s->ageRes >= 0)
		Wake();
}

// ---------------------------------------------------------------------------
// bg thread
// ---------------------------------------------------------------------------

static void NoteDeferral(LONGLONG waited)
{
	InterlockedIncrement(&s_deferred);
	NoteMax64(&s_deferMaxUs, QpcToUs(waited));
}

void NmAdjBgScanBeginLocked(NmAdjBgScan* s)
{
	NmAdjBgScanBegin(s, &g_reg, s_mode == MODE_ENFORCE, QpcNow());
}

bool NmAdjBgOfferLocked(NmAdjBgScan* s, uintptr_t task, bool eligible, bool bgOnly)
{
	NmJobDesc d;
	if (!NmAdjBgScanWants(s, bgOnly))
		return NmAdjBgScanOffer(s, &g_reg, task, &d, false, bgOnly);   // not described, not read
	if (eligible)
		NmAdjDescribeTask(task, &d);
	else
		memset(&d, 0, sizeof(d));
	return NmAdjBgScanOffer(s, &g_reg, task, &d, eligible, bgOnly);
}

NmAdjBgDecision NmAdjBgScanEndLocked(NmAdjBgScan* s, uintptr_t* pick, bool episodeStart)
{
	NmAdjBgResult r;
	WBegin();
	NmAdjBgScanEnd(s, &g_reg, &r);
	WEnd();
	*pick = 0;

	if (r.droppedRes) InterlockedIncrement(&s_resDrop);
	if (r.skipped)    InterlockedIncrement(&s_bgSkip);
	if (r.passedFull && episodeStart) InterlockedIncrement(&s_bgPassFull);
	if (r.bgOnlyWaited)
	{
		InterlockedIncrement(&s_bgoClaims);
		NoteMax64(&s_bgoMaxUs, QpcToUs(r.bgOnlyWait));
	}
	if (r.decision == NMADJ_BG_FULL)
	{
		InterlockedIncrement(&s_full);
		if (s_mode == MODE_ENFORCE)
			return NMADJ_BG_WAIT;
	}
	else if (r.decision != NMADJ_BG_CLAIM)
		return r.decision;

	if (r.conflicted) InterlockedIncrement(&s_would);
	if (r.deferred)   NoteDeferral(r.deferredFor);
	InterlockedIncrement(&s_claims);
	t_own = r.claimIdx;
	t_ownTask = r.claimIdx >= 0 ? r.pick : 0;
	*pick = (uintptr_t)r.pick;
	return NMADJ_BG_CLAIM;
}

void NmAdjBgForwardLocked(uintptr_t head)
{
	WBegin();
	NmAdjBgForward(&g_reg, head);
	NmAdjBgLooked(&g_reg, QpcNow());
	WEnd();
}

void NmAdjBgReleaseLocked()
{
	WBegin();
	NmAdjBgRelease(&g_reg);
	NmAdjBgLooked(&g_reg, QpcNow());
	WEnd();
}

void NmAdjBgLookedUnlocked()
{
	if (s_mode == MODE_OFF)
		return;
	// bgLastLook and bgLooked belong to the bg thread alone (nm_adjacency_policy.h).
	InterlockedExchange64((volatile LONG64*)&g_reg.bgLastLook, QpcNow());
	g_reg.bgLooked = true;
}

void NmAdjBgUnpin()
{
	uintptr_t nmg = g_navMeshGen;
	if (!nmg || !g_reg.pin)
		return;
	LockQueue(nmg);
	WBegin();
	g_reg.pin = 0;
	WEnd();
	UnlockQueue(nmg);
}

// NavMesh::stop seen, or the workers told to shut down.
static bool StopSeen()
{
	return InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0
	    || InterlockedCompareExchange(&g_workerShutdown, 0, 0) != 0;
}

// Rescans (RETRY) only when the event says a claim was published, drained or
// dropped, plus once when the slice runs out; nothing else can make a blocked
// queue runnable except a new job, which waits for the slice or the next
// dispatch.
static __declspec(thread) bool t_sliceRescanDone = false;

NmAdjWaitResult NmAdjBgWait(uintptr_t nmg, LONGLONG* sliceStart)
{
	if (!*sliceStart)
	{
		*sliceStart = QpcNow();
		t_sliceRescanDone = false;
	}
	const LONGLONG slice = kBgSliceMs * s_qpf / 1000;
	for (;;)
	{
		if (StopSeen() || !*(volatile unsigned char*)(nmg + OFF_NMG_THREAD_RUNNING))
			return NMADJ_WAIT_STOP;
		const LONGLONG left = slice - (QpcNow() - *sliceStart);
		if (left <= 0)
		{
			if (t_sliceRescanDone)
				return NMADJ_WAIT_SLICE;
			t_sliceRescanDone = true;
			return NMADJ_WAIT_RETRY;
		}
		DWORD ms = (DWORD)(left * 1000 / s_qpf) + 1;
		if (ms > kBgStopPollMs)
			ms = kBgStopPollMs;
		if (WaitForSingleObject(g_adjEvent, ms) == WAIT_OBJECT_0)
		{
			if (StopSeen() || !*(volatile unsigned char*)(nmg + OFF_NMG_THREAD_RUNNING))
				return NMADJ_WAIT_STOP;
			return NMADJ_WAIT_RETRY;
		}
	}
}

void NmAdjBgWaitDone(LONGLONG* sliceStart, bool idle)
{
	if (!*sliceStart)
		return;
	LONG64 us = QpcToUs(QpcNow() - *sliceStart);
	*sliceStart = 0;
	InterlockedIncrement(&s_bgWaits);
	InterlockedExchangeAdd64(&s_bgWaitUs, us);
	NoteMax64(&s_bgWaitMaxUs, us);
	if (idle)
		InterlockedIncrement(&s_bgIdle);
	static const LONG64 edges[7] = { 1000, 2000, 5000, 10000, 20000, 50000, 200000 };
	int b = 7;
	for (int i = 0; i < 7; ++i)
		if (us < edges[i]) { b = i; break; }
	InterlockedIncrement(&s_bgWaitHist[b]);
}
