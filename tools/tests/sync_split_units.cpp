// Host tests for the Ogre barrier probes' pure rules (audit_sync_split.h).

#include "../../profiler/audit_sync_split.h"

using namespace syncsplit;

#include "check.h"

static void Kinds()
{
	Check(KindOf(0) == SK_FIRE, "index 0 is the fire");
	Check(KindOf(1) == SK_WAIT, "index 1 is the wait");
	Check(KindOf(2) == SK_ODD && KindOf(-1) == SK_ODD, "any other index is odd");
}

static void Blocking()
{
	Check(WouldBlock(14, 16), "a caller with one other thread still to arrive blocks");
	Check(!WouldBlock(15, 16), "the last of sixteen to arrive does not block");
	Check(!WouldBlock(0, 0), "a barrier with no threads never blocks");
}

static void Buckets()
{
	Check(RequestBucket(0) == 0 && RequestBucket(12) == 12, "requests 0 to 12 keep their bucket");
	Check(RequestBucket(13) == 13 && RequestBucket(-1) == 13, "anything else goes to bucket 13");
	Check(TicksToUs(25000, 10000000) == 2500, "ticks convert to microseconds");
	Check(TicksToUs(5, 0) == 0, "a zero frequency converts to 0");
}

static void Reduction()
{
	// Four events over three slots, so a mean over slots (4.67) differs from one over events (3.5).
	WorkerFrame w[3] = { { 10, 6, 2 }, { 0, 0, 0 }, { 4, 4, 2 } };
	Reduced r = Reduce(w, 3);
	Check(r.sum == 14 && r.max == 6 && r.n == 4, "the reduction sums the sums and counts, and keeps the largest max");
	Check(r.mean > 3.49 && r.mean < 3.51, "the mean is over events, not slots");
	Reduced e = Reduce(w + 1, 1);
	Check(e.sum == 0 && e.max == 0 && e.n == 0 && e.mean == 0.0, "an idle slot reduces to zeros");
}

static void Locks()
{
	Check(IsReadOnlyLock(2), "lock option 2 is a read-only source lock");
	Check(!IsReadOnlyLock(1) && !IsReadOnlyLock(0), "discard and normal locks are destinations");
}

static void Qwords()
{
	// The worker loop's top wait: FF 15 disp32 at byte 1 of the qword 18 FF 15 51 9B 2E 00 EB.
	const unsigned long long top = 0xEB002E9B5115FF18ULL;
	Check(InOneQword(0x2CD5D9, 6), "a six-byte call at byte 1 of a qword fits in it");
	Check(InOneQword(0x2CD5DA, 6), "byte 2 is the last start a six-byte call fits from");
	Check(!InOneQword(0x2CD5DB, 6) && !InOneQword(0x2CD60F, 6), "a six-byte call that crosses into the next qword is refused");
	Check(!InOneQword(0x2CD5D8, 9) && !InOneQword(0x2CD5D8, 0), "no write is longer than a qword or empty");
	Check(CallQword(top, 1, 0x11223344) == 0xEB9011223344E818ULL, "the call becomes E8 rel32 90 and the bytes around it are kept");
	Check(CallQword(top, 1, -1) == 0xEB90FFFFFFFFE818ULL, "a negative displacement stays inside the call");
	Check(CallQword(top, 3, 0) == top, "a call that does not fit leaves the qword as it was");
}

int main()
{
	Kinds();
	Blocking();
	Buckets();
	Reduction();
	Locks();
	Qwords();
	return CheckExit("sync_split_units");
}
