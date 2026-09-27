// Drives the save-load reset's shipped fence sequence and admission gate with
// processJobCS held past the lock deadline. The navmesh threads are Win32
// threads, processJobCS is a critical section, and each zone is a ZoneMap page
// that is never freed holding the address of a content page that the native
// unload decommits. The run shows:
//
//   1. the drain finds nothing released, so the lock is given the whole
//      budget, and it times out on it;
//   2. the native unload runs anyway, nothing is released that was not held,
//      the drain's block is lowered once, and only the claimed survivor is
//      left loaded;
//   3. a claim and a claimed job's cache lookup made during the reset are held
//      until the gate comes down: the lookup finds its zone unloaded, the
//      claim is admitted;
//   4. the accepted residual: a MISS claimed before the gate rose, waiting for
//      the lock and consulting no gate, takes it once the holder leaves and
//      reads a zone the native unload frees next. That thread is harness code;
//      its fault shows the shape the fence leaves open, not a shipped check.
//
// Every scheduling point is an event set by the thread that reached it; a
// handshake that runs out is a failure, never a pass. The fence's drain and
// lock operations count the milliseconds they report per poll instead of
// sleeping, so nothing waits on real time: the gate's own wait ends when its
// event is set.
//
// Links src/zone/reset/zone_reset_sequence.cpp, zone_reset_gate.cpp and
// zone_reset_fence.cpp unmodified. Kept out of build_tests.bat because it
// reads a decommitted page on purpose.

#include <windows.h>
#include <cstdio>
#include "zone/reset/zone_reset_gate.h"
#include "zone/reset/zone_reset_sequence.h"

#include "check.h"

namespace reset_lock_injection_detail
{
	const DWORD    HANDSHAKE_MS   = 10000;
	const unsigned RESET_TOTAL_MS = 10000;
	const unsigned RESET_FLOOR_MS = 500;
	const unsigned DRAIN_POLL_MS  = 2;
	const SIZE_T   PAGE           = 4096;

	volatile LONG g_handshakeTimeouts = 0;

	// Waits for `h`; a wait that runs out is counted.
	bool Handshake(HANDLE h)
	{
		if (WaitForSingleObject(h, HANDSHAKE_MS) == WAIT_OBJECT_0)
			return true;
		InterlockedIncrement(&g_handshakeTimeouts);
		return false;
	}

	HANDLE NewEvent()
	{
		return CreateEvent(NULL, FALSE, FALSE, NULL);
	}

	// ---- Zones ----------------------------------------------------------------

	struct Zone
	{
		void** map;       // the ZoneMap page, never freed; slot 0 is the content pointer
		void*  content;   // the content page as it was loaded
	};

	void MakeZone(Zone* z)
	{
		z->map     = (void**)VirtualAlloc(NULL, PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		z->content = VirtualAlloc(NULL, PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		*(volatile int*)z->content = 1;
		InterlockedExchangePointer((PVOID volatile*)z->map, z->content);
	}

	// The zone's content pointer now; another thread may clear it.
	void* ContentNow(const Zone* z)
	{
		return InterlockedCompareExchangePointer((PVOID volatile*)z->map, NULL, NULL);
	}

	// The native unload of one zone: the page goes, then the pointer.
	void UnloadZone(Zone* z)
	{
		VirtualFree(z->content, PAGE, MEM_DECOMMIT);
		InterlockedExchangePointer((PVOID volatile*)z->map, NULL);
	}

	// True when reading the page faulted.
	bool ReadFaults(const void* page)
	{
		__try
		{
			volatile int sink = *(const volatile int*)page;
			(void)sink;
			return false;
		}
		__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
		{
			return true;
		}
	}

	// ---- Shared state -----------------------------------------------------------

	ZoneResetGate    g_gate;
	CRITICAL_SECTION g_pj;
	volatile LONG    g_live   = 0;   // generations released from g_pj and not yet back

	Zone g_n0;   // freed first by the native unload; the lookup P is on it
	Zone g_n1;   // freed second; the MISS M is on it
	Zone g_s0;   // a survivor a claimed job holds
	Zone g_s1;   // a survivor nothing claims

	HANDLE g_holderIn, g_holderRelease;
	HANDLE g_missGo, g_missWaiting, g_missChecked, g_zoneFreed, g_missDone;
	HANDLE g_hitGo, g_hitDone;
	HANDLE g_claimGo, g_claimRefused, g_claimWake, g_claimAdmitted;

	bool StopNever()
	{
		return false;
	}

	void WakeClaimer()
	{
		SetEvent(g_claimWake);
	}

	// ---- The holder of g_pj ------------------------------------------------------------

	// A MISS generating under the lock: the drain blocked its release.
	DWORD WINAPI Holder(void*)
	{
		EnterCriticalSection(&g_pj);
		SetEvent(g_holderIn);
		Handshake(g_holderRelease);
		LeaveCriticalSection(&g_pj);
		return 0;
	}

	// ---- The MISS waiting for the lock ----------------------------------------------------

	void* g_missSaw     = NULL;
	bool  g_missFaulted = false;

	DWORD WINAPI Miss(void*)
	{
		if (Handshake(g_missGo))
		{
			SetEvent(g_missWaiting);
			EnterCriticalSection(&g_pj);
			g_missSaw = ContentNow(&g_n1);
			SetEvent(g_missChecked);
			Handshake(g_zoneFreed);
			if (g_missSaw)
				g_missFaulted = ReadFaults(g_missSaw);
			LeaveCriticalSection(&g_pj);
		}
		SetEvent(g_missDone);
		return 0;
	}

	// ---- A claimed job reaching its cache lookup -------------------------------------------

	ZoneResetWait g_hitWait    = ZONE_RESET_WAIT_NONE;
	bool          g_hitLooked  = false;
	bool          g_hitDropped = false;

	DWORD WINAPI Lookup(void*)
	{
		if (Handshake(g_hitGo))
		{
			void* before = ContentNow(&g_n0);
			g_hitWait = ZoneResetGateWait(&g_gate, ZONE_RESET_SITE_HIT, &StopNever);
			if (g_hitWait == ZONE_RESET_WAIT_WAITED && !ZoneResetContentKept(before, ContentNow(&g_n0)))
				g_hitDropped = true;
			else if (g_hitWait != ZONE_RESET_WAIT_STOPPED)
				g_hitLooked = true;
		}
		SetEvent(g_hitDone);
		return 0;
	}

	// ---- A claim attempted during the reset -------------------------------------------

	int  g_claimRefusals = 0;
	bool g_claimTaken    = false;

	DWORD WINAPI ClaimThread(void*)
	{
		if (Handshake(g_claimGo))
		{
			for (;;)
			{
				if (ZoneResetGateUp(&g_gate) &&
				    ZoneResetAdmit(true, false, ZONE_RESET_SITE_CLAIM) == ZONE_RESET_DEFER_RESET)
				{
					ZoneResetGateNoteDeferred(&g_gate, ZONE_RESET_SITE_CLAIM);
					if (++g_claimRefusals == 1)
						SetEvent(g_claimRefused);
					if (!Handshake(g_claimWake))
						break;
					continue;
				}
				g_claimTaken = true;
				break;
			}
		}
		SetEvent(g_claimAdmitted);
		return 0;
	}

	// ---- The fence's operations -----------------------------------------------------------

	struct FenceLog
	{
		unsigned      lockBudget;
		ZoneResetLock lockAnswer;
		int           unloadCalls;
		ZoneResetLock lockAtUnload;
		int           unlockCalls;
		int           drainEndCalls;
		bool          kept[2];
		bool          unloaded[2];
	};

	FenceLog g_log;

	Zone* Survivor(int i)
	{
		return i == 0 ? &g_s0 : &g_s1;
	}

	bool DrainBegin(void*, unsigned timeoutMs, unsigned* waitedMs)
	{
		unsigned waited = 0;
		for (;;)
		{
			if (InterlockedCompareExchange(&g_live, 0, 0) == 0)
			{
				*waitedMs = waited;
				return true;
			}
			if (waited >= timeoutMs)
			{
				*waitedMs = waited;
				return false;
			}
			waited   += DRAIN_POLL_MS;
		}
	}

	void DrainEnd(void*)
	{
		g_log.drainEndCalls++;
	}

	ZoneResetLock Lock(void*, unsigned timeoutMs, unsigned* waitedMs)
	{
		g_log.lockBudget = timeoutMs;
		unsigned waited = 0;
		for (;;)
		{
			if (TryEnterCriticalSection(&g_pj))
			{
				*waitedMs = waited;
				g_log.lockAnswer = ZONE_RESET_LOCK_HELD;
				return ZONE_RESET_LOCK_HELD;
			}
			if (waited >= timeoutMs)
			{
				*waitedMs = waited;
				g_log.lockAnswer = ZONE_RESET_LOCK_TIMEOUT;
				return ZONE_RESET_LOCK_TIMEOUT;
			}
			waited   += 1;
		}
	}

	void Unlock(void*)
	{
		g_log.unlockCalls++;
		LeaveCriticalSection(&g_pj);
	}

	// Frees N0 and lets the holder go, so the waiting MISS takes the lock and
	// reads N1 before N1 is freed.
	void NativeUnload(void*)
	{
		g_log.unloadCalls++;
		g_log.lockAtUnload = g_log.lockAnswer;
		UnloadZone(&g_n0);
		SetEvent(g_holderRelease);
		Handshake(g_missChecked);
		UnloadZone(&g_n1);
		SetEvent(g_zoneFreed);
		Handshake(g_missDone);
	}

	int CollectSurvivors(void*)
	{
		return 2;
	}

	// Stands for a claimed job's hold on S0.
	bool SurvivorClaimed(void*, int i)
	{
		return i == 0;
	}

	void UnloadSurvivor(void*, int i)
	{
		g_log.unloaded[i] = true;
		UnloadZone(Survivor(i));
	}

	void KeepSurvivor(void*, int i)
	{
		g_log.kept[i] = true;
	}
}
using namespace reset_lock_injection_detail;

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	InitializeCriticalSection(&g_pj);
	g_holderIn      = NewEvent();
	g_holderRelease = NewEvent();
	g_missGo        = NewEvent();
	g_missWaiting   = NewEvent();
	g_missChecked   = NewEvent();
	g_zoneFreed     = NewEvent();
	g_missDone      = NewEvent();
	g_hitGo         = NewEvent();
	g_hitDone       = NewEvent();
	g_claimGo       = NewEvent();
	g_claimRefused  = NewEvent();
	g_claimWake     = NewEvent();
	g_claimAdmitted = NewEvent();
	g_gate.parked   = NewEvent();
	ZoneResetGateInit(&g_gate);

	MakeZone(&g_n0);
	MakeZone(&g_n1);
	MakeZone(&g_s0);
	MakeZone(&g_s1);

	HANDLE threads[4];
	threads[0] = CreateThread(NULL, 0, &Holder, NULL, 0, NULL);
	threads[1] = CreateThread(NULL, 0, &Miss, NULL, 0, NULL);
	threads[2] = CreateThread(NULL, 0, &Lookup, NULL, 0, NULL);
	threads[3] = CreateThread(NULL, 0, &ClaimThread, NULL, 0, NULL);

	ZoneResetFenceOps ops = { NULL, &DrainBegin, &DrainEnd, &Lock, &Unlock, &NativeUnload,
	                          &CollectSurvivors, &SurvivorClaimed, &UnloadSurvivor, &KeepSurvivor };
	ZoneResetFenceOutcome out;
	long deferredAtLine = -1;
	long deferredClaim  = -1;
	long deferredHit    = -1;

	Handshake(g_holderIn);
	{
		ZoneResetGateScope scope(&g_gate, NULL, &WakeClaimer);

		SetEvent(g_hitGo);
		Handshake(g_gate.parked);
		SetEvent(g_claimGo);
		Handshake(g_claimRefused);
		SetEvent(g_missGo);
		Handshake(g_missWaiting);

		ZoneResetRunFence(&ops, RESET_TOTAL_MS, RESET_FLOOR_MS, true, &out);

		// The reset line is built here, before the lower.
		deferredAtLine = ZoneResetGateDeferredTotal(&g_gate);
		deferredClaim  = ZoneResetGateDeferredAt(&g_gate, ZONE_RESET_SITE_CLAIM);
		deferredHit    = ZoneResetGateDeferredAt(&g_gate, ZONE_RESET_SITE_HIT);
	}

	// missDone was taken by the native unload; the MISS is joined below.
	Handshake(g_hitDone);
	Handshake(g_claimAdmitted);
	for (int i = 0; i < 4; ++i)
		Handshake(threads[i]);

	Check(out.drained && out.drainMs == 0,
	      "t2: the drain found nothing released and waited for nothing");
	Check(out.lock == ZONE_RESET_LOCK_TIMEOUT && g_log.lockBudget == RESET_TOTAL_MS &&
	      out.lockWaitMs == RESET_TOTAL_MS && !out.fenceComplete,
	      "t2: the lock timed out on the whole budget the drain left");
	Check(g_log.unloadCalls == 1 && g_log.lockAtUnload == ZONE_RESET_LOCK_TIMEOUT,
	      "t2: the native unload ran anyway after the lock timed out");
	Check(g_log.unlockCalls == 0,
	      "t2: nothing was released that was not held");
	Check(g_log.drainEndCalls == 1,
	      "t2: the drain's block was lowered once");
	Check(out.survivors == 2 && out.kept == 1 && g_log.kept[0] && !g_log.kept[1] &&
	      !g_log.unloaded[0] && g_log.unloaded[1] &&
	      ContentNow(&g_s0) != NULL && ContentNow(&g_s1) == NULL,
	      "t2: the claimed survivor was left loaded and the unclaimed one unloaded");
	Check(g_claimRefusals == 1 && g_claimTaken,
	      "t2: a claim during the reset was refused, then taken after the gate came down");
	Check(g_hitWait == ZONE_RESET_WAIT_WAITED && g_hitDropped && !g_hitLooked,
	      "t2: a claimed job's lookup waited the reset out, then found its zone unloaded");
	Check(deferredAtLine == 2 && deferredClaim == 1 && deferredHit == 1,
	      "t2: the line reads admitDeferred=2 (one claim, one lookup)");
	Check(g_missSaw != NULL && g_missFaulted,
	      "t2: accepted residual: a MISS claimed before the gate rose took the lock after the timeout and read a zone the native unload then freed");
	Check(InterlockedCompareExchange(&g_handshakeTimeouts, 0, 0) == 0,
	      "t2: every handshake completed within 10 s");

	if (out.lock == ZONE_RESET_LOCK_TIMEOUT && g_log.unloadCalls == 1)
		std::printf("t2: lock timed out (pjWaitMs=%u), native unload ran anyway, "
		            "%d claimed survivor left loaded, admitDeferred=%ld\n",
		            out.lockWaitMs, out.kept, deferredAtLine);
	else
		std::printf("t2: fence outcome: lock=%d pjWaitMs=%u nativeUnloads=%d\n",
		            (int)out.lock, out.lockWaitMs, g_log.unloadCalls);
	if (g_missSaw != NULL && g_missFaulted)
		std::printf("t2: accepted residual reproduced: a MISS admitted before the gate rose read a zone "
		            "freed after the lock timed out (fault taken)\n");
	else
		std::printf("t2: accepted residual not reproduced (missSaw=%d faulted=%d)\n",
		            g_missSaw != NULL ? 1 : 0, g_missFaulted ? 1 : 0);

	return CheckExit("reset_lock_injection");
}
