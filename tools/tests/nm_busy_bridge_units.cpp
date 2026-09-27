// The busy bridge's transitions through the shipped policy, over a stand-in
// generator: a critical section for the queue lock, two words for the count
// and the flag, and scheduling points driven by events. Every wait is on an
// event with a backstop whose expiry fails a check; nothing sleeps.

#include <windows.h>
#include <cstdio>
#include "navmesh/jobs/nm_busy_bridge_policy.h"

#include "check.h"

static const DWORD BACKSTOP_MS = 10000;

struct Bridge
{
	CRITICAL_SECTION lock;
	volatile LONG count;
	volatile LONG flag;
	volatile LONG acquires;      // successful lock acquisitions
	volatile LONG pauseTid;      // the thread whose next decrement pauses; 0 for none
	HANDLE evDecremented;        // set by that thread after its decrement
	HANDLE evResume;             // set by the test to let it go on
	HANDLE evContended;          // set by any thread that finds the lock taken
};

static void BLock(void* c)
{
	Bridge* b = (Bridge*)c;
	if (!TryEnterCriticalSection(&b->lock))
	{
		SetEvent(b->evContended);
		EnterCriticalSection(&b->lock);
	}
	InterlockedIncrement(&b->acquires);
}

static void BUnlock(void* c) { LeaveCriticalSection(&((Bridge*)c)->lock); }
static long BIncrement(void* c) { return InterlockedIncrement(&((Bridge*)c)->count); }

static long BDecrement(void* c)
{
	Bridge* b = (Bridge*)c;
	long v = InterlockedDecrement(&b->count);
	if ((DWORD)InterlockedCompareExchange(&b->pauseTid, 0, 0) == GetCurrentThreadId())
	{
		InterlockedExchange(&b->pauseTid, 0);
		SetEvent(b->evDecremented);
		WaitForSingleObject(b->evResume, BACKSTOP_MS);
	}
	return v;
}

static long BReadCount(void* c) { return InterlockedCompareExchange(&((Bridge*)c)->count, 0, 0); }

static unsigned char BReadFlag(void* c)
{
	return (unsigned char)InterlockedCompareExchange(&((Bridge*)c)->flag, 0, 0);
}

static void BWriteFlag(void* c, unsigned char v) { InterlockedExchange(&((Bridge*)c)->flag, v); }

static void Init(Bridge* b, long count, unsigned char flag)
{
	InitializeCriticalSection(&b->lock);
	b->count = count;
	b->flag = flag;
	b->acquires = 0;
	b->pauseTid = 0;
	b->evDecremented = CreateEvent(NULL, TRUE, FALSE, NULL);
	b->evResume      = CreateEvent(NULL, TRUE, FALSE, NULL);
	b->evContended   = CreateEvent(NULL, TRUE, FALSE, NULL);
}

static void Done(Bridge* b)
{
	CloseHandle(b->evDecremented);
	CloseHandle(b->evResume);
	CloseHandle(b->evContended);
	DeleteCriticalSection(&b->lock);
}

static BusyBridgeOps Ops(Bridge* b)
{
	BusyBridgeOps o;
	o.ctx = b;
	o.lockQueue = BLock;
	o.unlockQueue = BUnlock;
	o.increment = BIncrement;
	o.decrement = BDecrement;
	o.readCount = BReadCount;
	o.readFlag = BReadFlag;
	o.writeFlag = BWriteFlag;
	return o;
}

// A worker finishing its job.
struct LeaveArg { Bridge* b; bool held; };

static DWORD WINAPI LeaveThread(LPVOID p)
{
	LeaveArg* a = (LeaveArg*)p;
	InterlockedExchange(&a->b->pauseTid, (LONG)GetCurrentThreadId());
	a->held = BusyBridgeLeave(Ops(a->b));
	return 0;
}

// A worker claiming a job, inside the claim's own locked region.
struct ClaimArg { Bridge* b; HANDLE evClaimed; bool held; };

static DWORD WINAPI ClaimThread(LPVOID p)
{
	ClaimArg* a = (ClaimArg*)p;
	BusyBridgeOps o = Ops(a->b);
	o.lockQueue(o.ctx);
	a->held = BusyBridge(o, BUSY_BRIDGE_ENTER, true);
	o.unlockQueue(o.ctx);
	SetEvent(a->evClaimed);
	return 0;
}

// The interleaving that loses the flag: a release takes the count to 0, a new
// claim comes before the release writes the flag, then the release writes it.
static void ModelLeaveAgainstClaim()
{
	Bridge b;
	Init(&b, 1, 1);
	LeaveArg la = { &b, true };
	ClaimArg ca = { &b, CreateEvent(NULL, TRUE, FALSE, NULL), true };

	HANDLE ta = CreateThread(NULL, 0, LeaveThread, &la, 0, NULL);
	bool decremented = WaitForSingleObject(b.evDecremented, BACKSTOP_MS) == WAIT_OBJECT_0;
	HANDLE tb = CreateThread(NULL, 0, ClaimThread, &ca, 0, NULL);
	HANDLE first[2] = { ca.evClaimed, b.evContended };
	DWORD which = WaitForMultipleObjects(2, first, FALSE, BACKSTOP_MS);
	SetEvent(b.evResume);
	HANDLE both[2] = { ta, tb };
	bool joined = WaitForMultipleObjects(2, both, TRUE, BACKSTOP_MS) == WAIT_OBJECT_0;

	Check(decremented, "model: the releasing thread reached its decrement");
	Check(which == WAIT_OBJECT_0 + 1, "model: a claim made after the decrement waits for the release's lock");
	Check(joined, "model: both threads finish");
	Check(BReadCount(&b) == 1 && BReadFlag(&b) == 1,
	      "model: a release never clears the flag over a claim made after its decrement");
	Check(la.held && ca.held, "model: neither transition ends with the flag 0 while the count is not 0");

	CloseHandle(ta);
	CloseHandle(tb);
	CloseHandle(ca.evClaimed);
	Done(&b);
}

static void EnterAndLeave()
{
	Bridge b;
	Init(&b, 0, 0);
	BusyBridgeOps o = Ops(&b);
	o.lockQueue(o.ctx);
	bool entered = BusyBridge(o, BUSY_BRIDGE_ENTER, true);
	o.unlockQueue(o.ctx);
	Check(entered && BReadCount(&b) == 1 && BReadFlag(&b) == 1, "enter: count 1, flag 1");
	Check(b.acquires == 1, "enter: under the caller's lock it takes none of its own");
	bool left = BusyBridgeLeave(o);
	Check(left && BReadCount(&b) == 0 && BReadFlag(&b) == 0, "release: the last claim clears the flag");
	Check(b.acquires == 2, "release: takes the queue lock exactly once");
	Done(&b);
}

static void TwoClaims()
{
	Bridge b;
	Init(&b, 0, 0);
	BusyBridgeOps o = Ops(&b);
	BusyBridge(o, BUSY_BRIDGE_ENTER, false);
	BusyBridge(o, BUSY_BRIDGE_ENTER, false);
	Check(b.acquires == 2, "enter without the caller's lock takes it");
	BusyBridgeLeave(o);
	Check(BReadCount(&b) == 1 && BReadFlag(&b) == 1, "release: a claim still in flight keeps the flag");
	BusyBridgeLeave(o);
	Check(BReadCount(&b) == 0 && BReadFlag(&b) == 0, "release: the second release clears it");
	Done(&b);
}

static void ClearIfIdle()
{
	Bridge b;
	Init(&b, 0, 0);
	BusyBridgeOps o = Ops(&b);
	Check(BusyBridge(o, BUSY_BRIDGE_CLEAR_IF_IDLE, false) && b.acquires == 0,
	      "idle clear: a flag already 0 costs no lock");
	BWriteFlag(&b, 1);   // the original's own set, with nothing claimed
	Check(BusyBridge(o, BUSY_BRIDGE_CLEAR_IF_IDLE, false) && BReadFlag(&b) == 0 && b.acquires == 1,
	      "idle clear: a flag the original left at 1 is cleared under the lock");
	InterlockedExchange(&b.count, 1);
	BWriteFlag(&b, 1);
	Check(BusyBridge(o, BUSY_BRIDGE_CLEAR_IF_IDLE, false) && BReadFlag(&b) == 1,
	      "idle clear: a live claim keeps the flag");
	InterlockedExchange(&b.count, 0);
	o.lockQueue(o.ctx);
	long before = b.acquires;
	bool held = BusyBridge(o, BUSY_BRIDGE_CLEAR_IF_IDLE, true);
	o.unlockQueue(o.ctx);
	Check(held && BReadFlag(&b) == 0 && b.acquires == before,
	      "idle clear: under the caller's lock it clears in place");
	Done(&b);
}

static void InvariantReported()
{
	Bridge b;
	Init(&b, 1, 0);   // a state no transition may leave behind
	BusyBridgeOps o = Ops(&b);
	o.lockQueue(o.ctx);
	bool held = BusyBridge(o, BUSY_BRIDGE_CLEAR_IF_IDLE, true);
	o.unlockQueue(o.ctx);
	Check(!held, "invariant: the flag 0 with a claim in flight is reported");
	Done(&b);
}

int main()
{
	ModelLeaveAgainstClaim();
	EnterAndLeave();
	TwoClaims();
	ClearIfIdle();
	InvariantReported();
	return CheckExit("nm_busy_bridge_units");
}
