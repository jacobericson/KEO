#include <cstdio>
#include <windows.h>
#include "navmesh/generation/misspar_math.h"
#include "navmesh/scheduling/nm_inflight.h"

#include "check.h"

static InflightKey Key(unsigned n)
{
	InflightKey k;
	for (int i = 0; i < 6; ++i) k.v[i] = n * 7 + (unsigned)i;
	return k;
}

static bool StopNever()  { return false; }
static bool StopAlways() { return true; }

struct WaitArgs { InflightKey k; volatile LONG done; InflightResult r; int slot; };

static DWORD WINAPI WaitThread(LPVOID p)
{
	WaitArgs* a = (WaitArgs*)p;
	a->r = InflightRegisterOrWait(a->k, 5000, &StopNever, &a->slot);
	InterlockedExchange(&a->done, 1);
	return 0;
}

static char WB_REAL, WB_FRESH_A, WB_FRESH_B, WB_CLONE;

static MissParReleaseInputs Base(int armKind)
{
	MissParReleaseInputs in;
	in.armKind = armKind;
	in.splitEnabled = true;
	in.bgSplitEnabled = true;
	in.slotsReady = true;
	in.keycodesReady = true;
	in.stopSeen = false;
	in.holderIsBg = (armKind == MP_ARM_SWAP);
	in.pjDepth = 1;
	in.swapOutstanding = (armKind == MP_ARM_SWAP) ? 1 : 0;
	in.wb = (armKind == MP_ARM_SWAP) ? &WB_FRESH_A : &WB_CLONE;
	in.installedWb = (armKind == MP_ARM_SWAP) ? &WB_FRESH_A : &WB_REAL;
	in.canonicalWb = &WB_REAL;
	return in;
}

static void ClassifyTests()
{
	MissParReleaseInputs in = Base(MP_ARM_CLONE);
	Check(MissParClassifyRelease(in) == MP_REL_CLONE, "clone releases");
	in.wb = in.installedWb;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "clone on the installed buffer never releases");

	in = Base(MP_ARM_SERIAL);
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "a vanilla dispatch never releases");

	in = Base(MP_ARM_SWAP);
	Check(MissParClassifyRelease(in) == MP_REL_SWAP, "swap releases");
	in.bgSplitEnabled = false;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "navmeshMissSplitBg off");
	in = Base(MP_ARM_SWAP); in.canonicalWb = 0;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "swap without a proven canonical buffer");
	in = Base(MP_ARM_SWAP); in.wb = in.installedWb = in.canonicalWb;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "swap on the real buffer (no fresh one installed)");
	in = Base(MP_ARM_SWAP); in.swapOutstanding = 2;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "two swap runs in flight");
	in = Base(MP_ARM_SWAP); in.swapOutstanding = 0;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "swap arm bookkeeping lost the count");
	in = Base(MP_ARM_SWAP); in.holderIsBg = false;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "a worker on the swap path never releases");
	in = Base(MP_ARM_CLONE); in.holderIsBg = true;
	Check(MissParClassifyRelease(in) == MP_REL_CLONE, "a clone does not care which thread it is on");
	in = Base(MP_ARM_SWAP); in.installedWb = &WB_FRESH_B;
	Check(MissParClassifyRelease(in) == MP_REL_NONE, "someone else's buffer is installed");

	// The common refusals apply to both kinds.
	for (int k = MP_ARM_CLONE; k <= MP_ARM_SWAP; ++k)
	{
		in = Base(k); in.splitEnabled = false;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "navmeshMissSplit off");
		in = Base(k); in.slotsReady = false;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "no generation slots");
		in = Base(k); in.keycodesReady = false;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "keycodes not warm");
		in = Base(k); in.stopSeen = true;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "stop seen");
		in = Base(k); in.pjDepth = 2;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "nested hold");
		in = Base(k); in.pjDepth = 0;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "no hold");
		in = Base(k); in.wb = 0;
		Check(MissParClassifyRelease(in) == MP_REL_NONE, "no work buffer");
	}
}

static void CanonTests()
{
	MissParCanonState st;
	MissParCanonInit(st);
	Check(MissParCanonConfirmed(st, 3) == 0, "nothing observed yet");
	MissParCanonObserve(st, &WB_REAL, 3);
	MissParCanonObserve(st, &WB_REAL, 3);
	Check(MissParCanonConfirmed(st, 3) == 0, "two of three agreeing");
	MissParCanonObserve(st, &WB_REAL, 3);
	Check(MissParCanonConfirmed(st, 3) == &WB_REAL, "three agreeing proves it");

	// A disagreement before the proof just restarts the count.
	MissParCanonInit(st);
	MissParCanonObserve(st, &WB_REAL, 3);
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	Check(MissParCanonConfirmed(st, 3) == &WB_FRESH_A, "a restart still proves");

	// After the proof a disagreement disables it for good.
	MissParCanonObserve(st, &WB_REAL, 3);
	Check(MissParCanonConfirmed(st, 3) == 0 && st.disabled, "a proven pointer that changes disables");
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	MissParCanonObserve(st, &WB_FRESH_A, 3);
	Check(MissParCanonConfirmed(st, 3) == 0, "disabled stays disabled");

	// A null observation is ignored.
	MissParCanonInit(st);
	MissParCanonObserve(st, 0, 3);
	Check(st.cand == 0 && st.agree == 0, "null observation ignored");
}

static void InflightTests()
{
	InflightInit();
	InflightInit();   // idempotent

	InflightKey k = Key(1);
	int slotA = -1;
	Check(InflightRegisterOrWait(k, 5000, &StopNever, &slotA) == INFLIGHT_OWNER && slotA >= 0, "first registration owns the key");

	// A second thread on the same key waits for the owner's release.
	WaitArgs a;
	a.k = k; a.done = 0; a.r = INFLIGHT_FULL; a.slot = 99;
	HANDLE h = CreateThread(NULL, 0, WaitThread, &a, 0, NULL);
	Sleep(200);
	Check(InterlockedCompareExchange(&a.done, 0, 0) == 0, "waiter blocks while the owner holds the key");
	InflightRelease(slotA);
	WaitForSingleObject(h, 5000);
	CloseHandle(h);
	Check(a.done == 1 && a.r == INFLIGHT_WAITED && a.slot == -1, "waiter returns WAITED after the release");

	// The key is free again: the next caller owns it.
	int slotB = -1;
	Check(InflightRegisterOrWait(k, 5000, &StopNever, &slotB) == INFLIGHT_OWNER && slotB >= 0, "third call owns the key");
	InflightRelease(slotB);

	// Eight distinct keys fill the table; the ninth is FULL.
	int slots[9];
	for (int i = 0; i < 8; ++i)
		Check(InflightRegisterOrWait(Key(100 + i), 5000, &StopNever, &slots[i]) == INFLIGHT_OWNER, "eight distinct keys own");
	Check(InflightRegisterOrWait(Key(200), 5000, &StopNever, &slots[8]) == INFLIGHT_FULL && slots[8] == -1, "ninth distinct key is FULL");
	for (int i = 0; i < 8; ++i)
		InflightRelease(slots[i]);

	// A waiter whose stop callback reads true gives up at once.
	int slotC = -1, slotD = 99;
	Check(InflightRegisterOrWait(Key(300), 5000, &StopNever, &slotC) == INFLIGHT_OWNER, "stop test owner");
	DWORD t0 = GetTickCount();
	InflightResult r = InflightRegisterOrWait(Key(300), 5000, &StopAlways, &slotD);
	DWORD dt = GetTickCount() - t0;
	Check(r == INFLIGHT_STOPPED && slotD == -1 && dt < 100, "stopped waiter returns STOPPED within 100 ms");
	InflightRelease(slotC);

	// A bounded wait on a key nobody releases times out.
	int slotE = -1, slotF = 99;
	InflightRegisterOrWait(Key(400), 5000, &StopNever, &slotE);
	Check(InflightRegisterOrWait(Key(400), 150, &StopNever, &slotF) == INFLIGHT_TIMEOUT && slotF == -1, "unreleased key times out");
	InflightRelease(slotE);

	InflightRelease(-1);   // no-ops
	InflightRelease(8);
}

int main()
{
	// FNV-1a 64 of the empty input is the offset basis.
	Check(MissParFnv64(0, 0) == 0xCBF29CE484222325ULL, "empty");
	unsigned char a[3] = { 1, 2, 3 }, b[3] = { 1, 2, 4 };
	MissParSpan sa = { a, 3 }, sb = { b, 3 };
	Check(MissParFnv64(&sa, 1) != MissParFnv64(&sb, 1), "one byte differs");
	// The length is hashed: {1,2},{3} differs from {1},{2,3}.
	MissParSpan s1[2] = { { a, 2 }, { a + 2, 1 } }, s2[2] = { { a, 1 }, { a + 1, 2 } };
	Check(MissParFnv64(s1, 2) != MissParFnv64(s2, 2), "span boundaries");

	MissParInterval serial[3] = { { 0, 10 }, { 10, 20 }, { 25, 30 } };
	__int64 sum = 0;
	Check(MissParUnion(serial, 3, &sum) == 25 && sum == 25, "serial: union == sum");
	MissParInterval over[3] = { { 20, 30 }, { 0, 10 }, { 5, 25 } };
	Check(MissParUnion(over, 3, &sum) == 30 && sum == 40, "overlapping, unsorted");
	Check(MissParUnion(over, 0, &sum) == 0 && sum == 0, "none");

	Check(MissParGenConcurrency(0, 16, 4) == 2, "auto, 16 cpus");
	Check(MissParGenConcurrency(0, 4, 4) == 1, "auto, 4 cpus");
	Check(MissParGenConcurrency(0, 1, 4) == 1, "auto, 1 cpu");
	Check(MissParGenConcurrency(3, 16, 4) == 3, "explicit 3");
	Check(MissParGenConcurrency(9, 16, 4) == 4, "explicit above the cap");

	// The startup line: states configured vs. resolved, says why (auto vs.
	// explicit), and flags an auto/explicit cap that governs nothing this
	// session because navmeshMissSplit is off.
	{
		std::string autoMsg = MissParGenConcurrencyMessage(0, 2, 16, false);
		Check(autoMsg == "NavMesh gen concurrency: configured=0 resolved cap=2 (auto from 16 cpus)",
		      "auto message text");
		std::string explicitMsg = MissParGenConcurrencyMessage(4, 4, 16, false);
		Check(explicitMsg == "NavMesh gen concurrency: configured=4 resolved cap=4 (explicit)",
		      "explicit message text");
		std::string inertMsg = MissParGenConcurrencyMessage(4, 4, 16, true);
		Check(inertMsg.find("(explicit)") != std::string::npos &&
		      inertMsg.find("inert") != std::string::npos &&
		      inertMsg.find("navmeshMissSplit is off") != std::string::npos,
		      "split-off message names the cap and says it is inert");
	}

	ClassifyTests();
	CanonTests();
	InflightTests();

	return CheckExit("misspar_units");
}
