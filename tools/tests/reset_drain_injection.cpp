// Drives the save-load reset's shipped fence sequence and admission gate with
// a released generation held past the drain deadline. The navmesh threads are
// Win32 threads, processJobCS is a critical section, and each zone is a
// ZoneMap page that is never freed holding the address of a content page that
// the native unload decommits. The run shows:
//
//   1. the drain runs out its whole budget, and the lock is given the floor;
//   2. the lock is held, the native unload still runs with the gate up, and
//      only the claimed survivor is left loaded;
//   3. the generation re-enters processJobCS only after the fence releases it,
//      and the release comes before the drain's block is lowered;
//   4. a claim and two collision builds made during the reset are held until
//      the gate comes down: the build whose zone was unloaded drops its job,
//      the build whose zone kept its content builds, the claim is admitted;
//   5. control: the unloaded zone's content page faults when read.
//
// Every scheduling point is an event set by the thread that reached it; a
// handshake that runs out is a failure, never a pass. The fence's drain and
// lock operations count the milliseconds they report per poll instead of
// sleeping, so nothing waits on real time: the gate's own wait ends when its
// event is set.
//
// Links src/zone/reset/zone_reset_sequence.cpp, zone_reset_gate.cpp and
// zone_reset_fence.cpp unmodified. Kept out of build_tests.bat because its
// control reads a decommitted page on purpose.

#include <windows.h>
#include <cstdio>
#include "zone/reset/zone_reset_gate.h"
#include "zone/reset/zone_reset_sequence.h"

#include "check.h"

namespace reset_drain_injection_detail
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
	CRITICAL_SECTION g_queue;  // models generator queue +152, distinct from processJobCS
	volatile LONG    g_live     = 0;   // generations released from g_pj and not yet back
	volatile LONG    g_released = 0;   // set by the fence's unlock, under g_pj

	Zone g_n;    // freed by the native unload; the collision build B is on it
	Zone g_s0;   // a survivor the build K has claimed
	Zone g_s1;   // a survivor nothing claims

	HANDLE g_genReleased, g_genFinish, g_genReacquiring;
	HANDLE g_buildGo, g_keepGo, g_buildDone, g_keepDone;
	HANDLE g_claimGo, g_claimRefused, g_claimWake, g_claimAdmitted;
	HANDLE g_crossSnap, g_crossRelease, g_crossDone;
	LONG g_crossRaises = -1;
	ZoneResetWait g_crossWait = ZONE_RESET_WAIT_NONE;
	bool g_crossDownAtClaim = false;
	bool g_crossDownAfter = false;
	bool g_crossSawLowerReturn = false;
	volatile LONG g_lowerReturned = 0;

	bool StopNever()
	{
		return false;
	}

	void WakeClaimer()
	{
		SetEvent(g_claimWake);
	}

	void RaiseUnderQueue(ZoneResetGate* g)
	{
		EnterCriticalSection(&g_queue);
		ZoneResetGateRaise(g);
		LeaveCriticalSection(&g_queue);
	}

	void LowerUnderQueue(ZoneResetGate* g)
	{
		EnterCriticalSection(&g_queue);
		ZoneResetGateLower(g);
		LeaveCriticalSection(&g_queue);
		InterlockedExchange(&g_lowerReturned, 1);
	}

	// Claims before the reset, then crosses a complete raise/lower before its
	// first build wait. The release event is set only after the scope has exited.
	DWORD WINAPI CrossingJob(void*)
	{
		EnterCriticalSection(&g_queue);
		g_crossDownAtClaim = !ZoneResetGateUp(&g_gate);
		g_crossRaises = ZoneResetGateRaises(&g_gate);
		LeaveCriticalSection(&g_queue);
		SetEvent(g_crossSnap);
		if (Handshake(g_crossRelease))
		{
			g_crossSawLowerReturn = InterlockedCompareExchange(&g_lowerReturned, 0, 0) == 1;
			g_crossWait = ZoneResetGateWaitSince(&g_gate, ZONE_RESET_SITE_BUILD,
				&StopNever, g_crossRaises);
			g_crossDownAfter = !ZoneResetGateUp(&g_gate);
		}
		SetEvent(g_crossDone);
		return 0;
	}

	// ---- The released generation --------------------------------------------------

	LONG g_genSawReleased = -1;

	DWORD WINAPI Generation(void*)
	{
		InterlockedIncrement(&g_live);
		SetEvent(g_genReleased);
		Handshake(g_genFinish);
		SetEvent(g_genReacquiring);
		EnterCriticalSection(&g_pj);
		g_genSawReleased = InterlockedCompareExchange(&g_released, 0, 0);
		InterlockedDecrement(&g_live);
		LeaveCriticalSection(&g_pj);
		return 0;
	}

	// ---- A claimed job reaching its collision build ---------------------------------

	struct BuildJob
	{
		Zone*         zone;
		HANDLE        go;
		HANDLE        done;
		ZoneResetWait wait;
		bool          built;
		bool          dropped;
	};

	DWORD WINAPI CollisionBuild(void* p)
	{
		BuildJob* j = (BuildJob*)p;
		if (Handshake(j->go))
		{
			void* before = ContentNow(j->zone);
			j->wait = ZoneResetGateWait(&g_gate, ZONE_RESET_SITE_BUILD, &StopNever);
			if (j->wait == ZONE_RESET_WAIT_WAITED && !ZoneResetContentKept(before, ContentNow(j->zone)))
				j->dropped = true;
			else if (j->wait != ZONE_RESET_WAIT_STOPPED)
				j->built = true;
		}
		SetEvent(j->done);
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
				EnterCriticalSection(&g_queue);
				bool refused = ZoneResetGateUp(&g_gate) &&
					ZoneResetAdmit(true, false, ZONE_RESET_SITE_CLAIM) == ZONE_RESET_DEFER_RESET;
				if (refused)
					ZoneResetGateNoteDeferred(&g_gate, ZONE_RESET_SITE_CLAIM);
				else
					(void)ZoneResetGateRaises(&g_gate);  // accepted claim snapshot under +152
				LeaveCriticalSection(&g_queue);
				if (refused)
				{
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
		unsigned lockBudget;
		int      unloadCalls;
		bool     gateUpAtUnload;
		int      unlockCalls;
		int      drainEndCalls;
		int      seq;
		int      unlockSeq;
		int      drainEndSeq;
		bool     kept[2];
		bool     unloaded[2];
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
		g_log.drainEndSeq = ++g_log.seq;
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
				return ZONE_RESET_LOCK_HELD;
			}
			if (waited >= timeoutMs)
			{
				*waitedMs = waited;
				return ZONE_RESET_LOCK_TIMEOUT;
			}
			waited   += 1;
		}
	}

	void Unlock(void*)
	{
		InterlockedExchange(&g_released, 1);
		g_log.unlockCalls++;
		g_log.unlockSeq = ++g_log.seq;
		LeaveCriticalSection(&g_pj);
	}

	// Frees N, then lets the generation finish and try for g_pj.
	void NativeUnload(void*)
	{
		g_log.unloadCalls++;
		g_log.gateUpAtUnload = ZoneResetGateUp(&g_gate);
		UnloadZone(&g_n);
		SetEvent(g_genFinish);
		Handshake(g_genReacquiring);
	}

	int CollectSurvivors(void*)
	{
		return 2;
	}

	// Stands for the build K's claim on S0.
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
using namespace reset_drain_injection_detail;

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	InitializeCriticalSection(&g_pj);
	InitializeCriticalSection(&g_queue);
	g_genReleased    = NewEvent();
	g_genFinish      = NewEvent();
	g_genReacquiring = NewEvent();
	g_buildGo        = NewEvent();
	g_keepGo         = NewEvent();
	g_buildDone      = NewEvent();
	g_keepDone       = NewEvent();
	g_claimGo        = NewEvent();
	g_claimRefused   = NewEvent();
	g_claimWake      = NewEvent();
	g_claimAdmitted  = NewEvent();
	g_crossSnap      = NewEvent();
	g_crossRelease   = NewEvent();
	g_crossDone      = NewEvent();
	g_gate.parked    = NewEvent();
	ZoneResetGateInit(&g_gate);

	MakeZone(&g_n);
	MakeZone(&g_s0);
	MakeZone(&g_s1);
	const void* nPage = g_n.content;

	BuildJob b = { &g_n,  g_buildGo, g_buildDone, ZONE_RESET_WAIT_NONE, false, false };
	BuildJob k = { &g_s0, g_keepGo,  g_keepDone,  ZONE_RESET_WAIT_NONE, false, false };

	HANDLE threads[5];
	threads[0] = CreateThread(NULL, 0, &Generation, NULL, 0, NULL);
	threads[1] = CreateThread(NULL, 0, &CollisionBuild, &b, 0, NULL);
	threads[2] = CreateThread(NULL, 0, &CollisionBuild, &k, 0, NULL);
	threads[3] = CreateThread(NULL, 0, &ClaimThread, NULL, 0, NULL);
	threads[4] = CreateThread(NULL, 0, &CrossingJob, NULL, 0, NULL);

	ZoneResetFenceOps ops = { NULL, &DrainBegin, &DrainEnd, &Lock, &Unlock, &NativeUnload,
	                          &CollectSurvivors, &SurvivorClaimed, &UnloadSurvivor, &KeepSurvivor };
	ZoneResetFenceOutcome out;
	long deferredAtLine = -1;
	long deferredClaim  = -1;
	long deferredBuild  = -1;

	Handshake(g_genReleased);
	Handshake(g_crossSnap);   // the crossing job's snapshot precedes this reset's raise
	{
		ZoneResetGateScope scope(&g_gate, &RaiseUnderQueue, &WakeClaimer, &LowerUnderQueue);

		SetEvent(g_buildGo);
		Handshake(g_gate.parked);
		SetEvent(g_keepGo);
		Handshake(g_gate.parked);
		SetEvent(g_claimGo);
		Handshake(g_claimRefused);

		ZoneResetRunFence(&ops, RESET_TOTAL_MS, RESET_FLOOR_MS, true, &out);

		// The reset line is built here, before the lower.
		deferredAtLine = ZoneResetGateDeferredTotal(&g_gate);
		deferredClaim  = ZoneResetGateDeferredAt(&g_gate, ZONE_RESET_SITE_CLAIM);
		deferredBuild  = ZoneResetGateDeferredAt(&g_gate, ZONE_RESET_SITE_BUILD);
	}
	SetEvent(g_crossRelease);   // only after the lower callback returned
	bool gateDown = !ZoneResetGateUp(&g_gate);
	bool overSet  = WaitForSingleObject(g_gate.over, 0) == WAIT_OBJECT_0;

	Handshake(g_buildDone);
	Handshake(g_keepDone);
	Handshake(g_claimAdmitted);
	Handshake(g_crossDone);
	for (int i = 0; i < 5; ++i)
		Handshake(threads[i]);

	bool controlFaulted = ReadFaults(nPage);

	Check(!out.drained && out.drainMs == RESET_TOTAL_MS,
	      "t1: the drain timed out and reported the whole budget");
	Check(g_log.lockBudget == RESET_FLOOR_MS,
	      "t1: the lock was given the floor, not a second full budget");
	Check(out.lock == ZONE_RESET_LOCK_HELD && !out.fenceComplete,
	      "t1: the lock was held, so only the drain left the fence incomplete");
	Check(g_log.unloadCalls == 1 && g_log.gateUpAtUnload && !out.fenceComplete,
	      "t1: the native unload ran with the gate up and the fence incomplete");
	Check(out.survivors == 2 && out.kept == 1 && g_log.kept[0] && !g_log.kept[1] &&
	      !g_log.unloaded[0] && g_log.unloaded[1] &&
	      ContentNow(&g_s0) != NULL && ContentNow(&g_s1) == NULL,
	      "t1: the claimed survivor was left loaded and the unclaimed one unloaded");
	Check(g_genSawReleased == 1 && InterlockedCompareExchange(&g_live, 0, 0) == 0,
	      "t1: the released generation took the lock only after the fence released it");
	Check(g_log.unlockCalls == 1 && g_log.drainEndCalls == 1 && g_log.unlockSeq > 0 &&
	      g_log.drainEndSeq > g_log.unlockSeq,
	      "t1: the lock was released before the drain's block was lowered");
	Check(g_claimRefusals == 1 && g_claimTaken,
	      "t1: a claim during the reset was refused, then taken after the gate came down");
	Check(b.wait == ZONE_RESET_WAIT_WAITED && b.dropped && !b.built,
	      "t1: the collision build waited the reset out, then found its zone unloaded and dropped the job");
	Check(k.wait == ZONE_RESET_WAIT_WAITED && k.built && !k.dropped,
	      "t1: a collision build whose zone kept its content through the reset waited, then built");
	Check(deferredAtLine == 3 && deferredClaim == 1 && deferredBuild == 2,
	      "t1: the line reads admitDeferred=3 (one claim, two builds)");
	Check(gateDown && overSet,
	      "t1: the gate is down and its event set after the scope");
	Check(g_crossDownAtClaim && g_crossRaises == 0 && g_crossSawLowerReturn
	      && g_crossDownAfter && g_crossWait == ZONE_RESET_WAIT_WAITED
	      && deferredBuild == 2,
	      "t1: a job claimed under the queue lock before a whole reset revalidates after lower");
	Check(controlFaulted,
	      "t1: control: the unloaded zone's content page faults when read");
	Check(InterlockedCompareExchange(&g_handshakeTimeouts, 0, 0) == 0,
	      "t1: every handshake completed within 10 s");

	if (!out.drained && out.lock == ZONE_RESET_LOCK_HELD && g_log.unloadCalls == 1)
		std::printf("t1: drain timed out (drainMs=%u), lock held on a %u ms budget, native unload ran, "
		            "%d claimed survivor left loaded, admitDeferred=%ld\n",
		            out.drainMs, g_log.lockBudget, out.kept, deferredAtLine);
	else
		std::printf("t1: fence outcome: drained=%d drainMs=%u lock=%d lockBudget=%u nativeUnloads=%d\n",
		            out.drained ? 1 : 0, out.drainMs, (int)out.lock, g_log.lockBudget, g_log.unloadCalls);
	if (controlFaulted)
		std::printf("t1: control: the unloaded zone's content page faulted when read\n");
	else
		std::printf("t1: control: the unloaded zone's content page read without a fault\n");

	return CheckExit("reset_drain_injection");
}
