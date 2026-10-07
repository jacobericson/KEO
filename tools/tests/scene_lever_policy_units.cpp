// Host tests for the scene switches' rules: the three idle tests over fake
// scene managers, the fire/wait pairing, the call-site stub bytes, the rel32
// reach, the site windows, the instance upload decision, the render-queue
// flag's writes and the PE header read.

#include <cstdio>
#include <cstring>
#include "render/scene_lever_policy.h"

#include "check.h"

// A scene manager: 0x4C00 bytes, zero-initialised and 8-byte aligned.
static unsigned long long g_sm[0x4C00 / 8];

// Two fake vtables: slot VT_BATCH_CULL holds the address under test.
static uintptr_t g_vtA[16];
static uintptr_t g_vtB[16];

static const uintptr_t HW_CULL  = 0x18012FBA0ULL;
static const uintptr_t VTF_CULL = 0x180130E90ULL;
static const uintptr_t BASE_CULL = 0x1800274C0ULL;
// An override in some other module: no list names it.
static const uintptr_t OTHER_CULL = 0x7FF8123456A0ULL;

static unsigned char* Sm() { return (unsigned char*)g_sm; }

static void Put(void* base, size_t off, uintptr_t v)
{
	memcpy((unsigned char*)base + off, &v, sizeof(v));
}

static void ResetSm()
{
	memset(g_sm, 0, sizeof(g_sm));
}

// ---- r8 -----------------------------------------------------------------

// Three visible lists of up to four objects each.
static uintptr_t g_objs[3][4][2];      // each object: { vtable, pad }
static uintptr_t g_data[3][4];
static uintptr_t g_lists[3][3];        // { data, size, capacity }

static void SetVt(uintptr_t* vt, uintptr_t slot)
{
	memset(vt, 0, 16 * sizeof(uintptr_t));
	vt[VT_BATCH_CULL / 8] = slot;
}

// Fake code and import entries for the thunk test. The readers serve only
// these two buffers and fail anywhere else, as an unreadable address would.
static unsigned char g_code[64];
static uintptr_t g_iat[2];
static int g_reads = 0;

static bool InFake(uintptr_t addr, size_t len)
{
	const uintptr_t c = (uintptr_t)g_code, i = (uintptr_t)g_iat;
	return (addr >= c && addr + len <= c + sizeof(g_code))
	    || (addr >= i && addr + len <= i + sizeof(g_iat));
}

static bool FakeReadBytes(uintptr_t addr, unsigned char* out, size_t len)
{
	++g_reads;
	if (!InFake(addr, len))
		return false;
	memcpy(out, (const void*)addr, len);
	return true;
}

static bool FakeReadPtr(uintptr_t addr, uintptr_t* out)
{
	++g_reads;
	if (!InFake(addr, sizeof(*out)))
		return false;
	memcpy(out, (const void*)addr, sizeof(*out));
	return true;
}

static const CullReaders g_rd = { &FakeReadBytes, &FakeReadPtr };
static CullThunkCache g_cache;

static void ResetCache()
{
	memset(&g_cache, 0, sizeof(g_cache));
	g_reads = 0;
}

// A jmp [rip+disp32] at g_code + at, with a REX.W prefix when rex, aimed at
// import entry `entry`, which holds target. Returns the thunk's address.
static uintptr_t PutThunk(size_t at, bool rex, int entry, uintptr_t target)
{
	memset(g_code, 0xCC, sizeof(g_code));
	g_iat[entry] = target;
	unsigned char* t = g_code + at;
	size_t n = 0;
	if (rex)
		t[n++] = 0x48;
	t[n++] = 0xFF;
	t[n++] = 0x25;
	const int32_t disp = (int32_t)((intptr_t)&g_iat[entry] - (intptr_t)(t + n + 4));
	memcpy(t + n, &disp, sizeof(disp));
	return (uintptr_t)t;
}

static bool R8(uintptr_t base)
{
	return ForkR8Idle(Sm(), base, &g_cache, &g_rd);
}

// Lists 0..n-1, each with `per` objects on vtable A.
static void BuildR8(size_t n, size_t per)
{
	ResetSm();
	memset(g_objs, 0, sizeof(g_objs));
	memset(g_data, 0, sizeof(g_data));
	memset(g_lists, 0, sizeof(g_lists));
	for (size_t i = 0; i < n; ++i)
	{
		for (size_t k = 0; k < per; ++k)
		{
			g_objs[i][k][0] = (uintptr_t)g_vtA;
			g_data[i][k] = (uintptr_t)g_objs[i][k];
		}
		g_lists[i][0] = (uintptr_t)g_data[i];
		g_lists[i][1] = per;
		g_lists[i][2] = 4;
	}
	Put(Sm(), SM_VISIBLE_LISTS, (uintptr_t)g_lists);
	Put(Sm(), SM_VISIBLE_LIST_COUNT, n);
}

static void TestR8()
{
	SetVt(g_vtA, BASE_CULL);
	ResetSm();
	Check(R8(BASE_CULL), "r8: no visible list is idle");

	SetVt(g_vtA, BASE_CULL);
	BuildR8(3, 4);
	Check(R8(BASE_CULL), "r8: lists of plain objects are idle");

	SetVt(g_vtB, HW_CULL);
	BuildR8(3, 4);
	g_objs[1][2][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8: an InstanceBatchHW makes the fork run");

	SetVt(g_vtB, VTF_CULL);
	BuildR8(3, 4);
	g_objs[0][1][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8: an InstanceBatchHW_VTF makes the fork run");

	SetVt(g_vtB, OTHER_CULL);
	BuildR8(3, 4);
	g_objs[1][0][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8: an override from an unknown module makes the fork run");

	BuildR8(3, 4);
	Check(!R8(0), "r8: no base address makes the fork run");

	SetVt(g_vtB, HW_CULL);
	BuildR8(3, 4);
	g_objs[2][3][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8: a batch in the last list makes the fork run");

	BuildR8(3, 4);
	g_data[1][0] = 0;
	Check(!R8(BASE_CULL), "r8: a null object makes the fork run");

	BuildR8(3, 4);
	g_objs[2][0][0] = 0;
	Check(!R8(BASE_CULL), "r8: a null vtable makes the fork run");

	BuildR8(3, 4);
	Put(Sm(), SM_VISIBLE_LIST_COUNT, FORK_MAX_LISTS + 1);
	Check(!R8(BASE_CULL), "r8: too many lists make the fork run");

	// Lists 0 and 1 empty with null data pointers: passed over, idle; then a
	// batch in list 2 is still found behind them.
	SetVt(g_vtB, HW_CULL);
	BuildR8(3, 4);
	g_lists[0][0] = 0;
	g_lists[0][1] = 0;
	g_lists[1][0] = 0;
	g_lists[1][1] = 0;
	const bool emptyIdle = R8(BASE_CULL);
	g_objs[2][1][0] = (uintptr_t)g_vtB;
	Check(emptyIdle && !R8(BASE_CULL), "r8: empty lists are skipped over");
}

// An object class from another module reaches the base through that module's
// import thunk, and counts as the base.
static void TestR8Thunk()
{
	ResetCache();
	SetVt(g_vtB, PutThunk(8, false, 0, BASE_CULL));
	BuildR8(3, 4);
	g_objs[1][2][0] = (uintptr_t)g_vtB;
	Check(R8(BASE_CULL), "r8 thunk: an FF 25 thunk whose import entry holds the base is idle");

	ResetCache();
	SetVt(g_vtB, PutThunk(16, true, 1, BASE_CULL));
	BuildR8(3, 4);
	g_objs[0][0][0] = (uintptr_t)g_vtB;
	Check(R8(BASE_CULL), "r8 thunk: a 48 FF 25 thunk to the base is idle");

	ResetCache();
	SetVt(g_vtB, PutThunk(8, false, 0, OTHER_CULL));
	BuildR8(3, 4);
	g_objs[2][3][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8 thunk: a thunk whose import entry holds another address makes the fork run");
	Check(g_cache.count == 0, "r8 thunk: a thunk to another address is not cached");

	// E9 rel32: a jump of another form, even one that lands on an entry
	// holding the base, is not read as a thunk.
	ResetCache();
	PutThunk(8, false, 0, BASE_CULL);
	g_code[8] = 0xE9;
	SetVt(g_vtB, (uintptr_t)(g_code + 8));
	BuildR8(3, 4);
	g_objs[1][1][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8 thunk: an E9 rel32 jump makes the fork run");

	ResetCache();
	SetVt(g_vtB, 0);
	BuildR8(3, 4);
	g_objs[1][1][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL) && g_reads == 0, "r8 thunk: a NULL slot makes the fork run, unread");

	// An import entry outside readable memory: the read fails, the fork runs.
	ResetCache();
	PutThunk(8, false, 0, BASE_CULL);
	const int32_t farDisp = 0x10000000;
	memcpy(g_code + 10, &farDisp, sizeof(farDisp));
	SetVt(g_vtB, (uintptr_t)(g_code + 8));
	BuildR8(3, 4);
	g_objs[0][3][0] = (uintptr_t)g_vtB;
	Check(!R8(BASE_CULL), "r8 thunk: an unreadable import entry makes the fork run");

	// A confirmed thunk is cached; later walks answer without a read.
	ResetCache();
	const uintptr_t thunk = PutThunk(8, false, 0, BASE_CULL);
	SetVt(g_vtB, thunk);
	BuildR8(3, 4);
	g_objs[2][0][0] = (uintptr_t)g_vtB;
	const bool first = R8(BASE_CULL);
	g_reads = 0;
	memset(g_code, 0xCC, sizeof(g_code));
	Check(first && g_cache.count == 1 && R8(BASE_CULL) && g_reads == 0,
	      "r8 thunk: a cached thunk is idle without re-reading");

	// A full cache still answers by reading and keeps its entries.
	ResetCache();
	g_cache.count = CULL_THUNK_CACHE;
	for (size_t i = 0; i < CULL_THUNK_CACHE; ++i)
		g_cache.slot[i] = 0x1000 + i;
	SetVt(g_vtB, PutThunk(24, false, 1, BASE_CULL));
	BuildR8(3, 4);
	g_objs[1][3][0] = (uintptr_t)g_vtB;
	Check(R8(BASE_CULL) && g_reads > 0 && g_cache.count == CULL_THUNK_CACHE && g_cache.slot[0] == 0x1000,
	      "r8 thunk: a full cache still reads and keeps its entries");

	// No readers: only the equality is tested.
	ResetCache();
	SetVt(g_vtB, PutThunk(8, false, 0, BASE_CULL));
	BuildR8(3, 4);
	g_objs[1][2][0] = (uintptr_t)g_vtB;
	Check(!ForkR8Idle(Sm(), BASE_CULL, &g_cache, NULL), "r8 thunk: with no readers a thunk makes the fork run");
}

// ---- r1 -----------------------------------------------------------------

// A manager points at its list head; an empty list's head points at itself.
static uintptr_t g_heads[4];
static uintptr_t g_node;
static uintptr_t g_skelMgrs[4][1];
static uintptr_t g_skelArr[4];

static void BuildR1(size_t n)
{
	ResetSm();
	for (size_t i = 0; i < n; ++i)
	{
		g_heads[i] = (uintptr_t)&g_heads[i];
		g_skelMgrs[i][0] = (uintptr_t)&g_heads[i];
		g_skelArr[i] = (uintptr_t)g_skelMgrs[i];
	}
	Put(Sm(), SM_SKEL_MGRS_BEGIN, (uintptr_t)g_skelArr);
	Put(Sm(), SM_SKEL_MGRS_END, (uintptr_t)(g_skelArr + n));
}

static void TestR1()
{
	ResetSm();
	Check(ForkR1Idle(Sm()), "r1: no manager is idle");

	BuildR1(3);
	Check(ForkR1Idle(Sm()), "r1: managers with empty lists are idle");

	BuildR1(3);
	g_node = (uintptr_t)&g_heads[1];
	g_heads[1] = (uintptr_t)&g_node;
	Check(!ForkR1Idle(Sm()), "r1: one non-empty list makes the fork run");

	BuildR1(3);
	g_skelArr[2] = 0;
	Check(!ForkR1Idle(Sm()), "r1: a null manager makes the fork run");

	BuildR1(3);
	Put(Sm(), SM_SKEL_MGRS_BEGIN, (uintptr_t)(g_skelArr + 3));
	Put(Sm(), SM_SKEL_MGRS_END, (uintptr_t)g_skelArr);
	Check(!ForkR1Idle(Sm()), "r1: an inverted range makes the fork run");
}

// ---- r7 -----------------------------------------------------------------

static unsigned long long g_instMgrs[3][16];   // 128 bytes each
static uintptr_t g_instArr[3];

static void BuildR7(size_t n)
{
	ResetSm();
	memset(g_instMgrs, 0, sizeof(g_instMgrs));
	for (size_t i = 0; i < n; ++i)
	{
		// Both lists empty: begin == end, at a non-null address.
		const uintptr_t empty = (uintptr_t)&g_instMgrs[i][15];
		Put(g_instMgrs[i], IM_DYNAMIC_BEGIN, empty);
		Put(g_instMgrs[i], IM_DYNAMIC_END, empty);
		Put(g_instMgrs[i], IM_DIRTY_BEGIN, empty);
		Put(g_instMgrs[i], IM_DIRTY_END, empty);
		g_instArr[i] = (uintptr_t)g_instMgrs[i];
	}
	Put(Sm(), SM_INST_MGRS_BEGIN, (uintptr_t)g_instArr);
	Put(Sm(), SM_INST_MGRS_END, (uintptr_t)(g_instArr + n));
}

static void TestR7()
{
	ResetSm();
	Check(ForkR7Idle(Sm()), "r7: no manager is idle");

	BuildR7(3);
	Check(ForkR7Idle(Sm()), "r7: managers without batches are idle");

	BuildR7(3);
	Put(g_instMgrs[1], IM_DYNAMIC_END, (uintptr_t)&g_instMgrs[1][15] + 8);
	Check(!ForkR7Idle(Sm()), "r7: a dynamic batch makes the fork run");

	BuildR7(3);
	Put(g_instMgrs[2], IM_DIRTY_END, (uintptr_t)&g_instMgrs[2][15] + 8);
	Check(!ForkR7Idle(Sm()), "r7: a dirty static batch makes the fork run");

	BuildR7(3);
	g_instArr[0] = 0;
	Check(!ForkR7Idle(Sm()), "r7: a null manager makes the fork run");
}

// ---- pair ---------------------------------------------------------------

static void TestPair()
{
	ForkPairState s = { true, false };
	Check(ForkFire(&s, false, true, true) == FORK_CALL && !s.pending
	      && ForkWait(&s, true) == FORK_CALL, "pair: off calls");

	ForkPairState half = { false, false };
	Check(ForkFire(&half, true, true, true) == FORK_CALL && !half.pending, "pair: a pair not live never skips");

	s.live = true; s.pending = false;
	Check(ForkFire(&s, true, false, true) == FORK_CALL && !s.pending, "pair: off the main thread calls");

	s.live = true; s.pending = false;
	Check(ForkFire(&s, true, true, true) == FORK_SKIP && s.pending,
	      "pair: an idle fire skips and leaves its wait pending");
	Check(ForkWait(&s, true) == FORK_SKIP && !s.pending,
	      "pair: the wait of a skipped fire returns without the barrier");

	s.live = true; s.pending = false;
	Check(ForkFire(&s, true, true, false) == FORK_CALL && !s.pending && ForkWait(&s, true) == FORK_CALL,
	      "pair: a busy fire and its wait both call");

	s.live = true; s.pending = true;
	Check(ForkFire(&s, true, true, true) == FORK_CALL_ODD && !s.pending,
	      "pair: a fire that finds pending set calls and clears it");

	s.live = true; s.pending = true;
	Check(ForkWait(&s, false) == FORK_CALL && s.pending, "pair: a wait off the main thread never consumes pending");
}

// ---- stub ---------------------------------------------------------------

static bool StubIs(ForkReg reg, const unsigned char prefix[3])
{
	unsigned char out[32];
	memset(out, 0xCC, sizeof(out));
	const size_t n = BuildForkStub(out, sizeof(out), reg, 3, 0x1122334455667788ULL);
	unsigned char want[21] = { 0, 0, 0, 0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x48, 0xB8,
	                           0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0 };
	memcpy(want, prefix, 3);
	return n == FORK_STUB_BYTES && memcmp(out, want, sizeof(want)) == 0 && out[21] == 0xCC;
}

static void TestStub()
{
	static const unsigned char rsi[3] = { 0x48, 0x89, 0xF2 };
	static const unsigned char r12[3] = { 0x4C, 0x89, 0xE2 };
	static const unsigned char rdi[3] = { 0x48, 0x89, 0xFA };
	Check(StubIs(FORK_REG_RSI, rsi), "stub: rsi form");
	Check(StubIs(FORK_REG_R12, r12), "stub: r12 form");
	Check(StubIs(FORK_REG_RDI, rdi), "stub: rdi form");

	unsigned char shortBuf[20];
	memset(shortBuf, 0xCC, sizeof(shortBuf));
	bool untouched = true;
	const size_t n = BuildForkStub(shortBuf, sizeof(shortBuf), FORK_REG_RSI, 3, 0x1122334455667788ULL);
	for (size_t i = 0; i < sizeof(shortBuf); ++i)
		untouched = untouched && shortBuf[i] == 0xCC;
	Check(n == 0 && untouched, "stub: a short buffer writes nothing");
}

// ---- rel32 --------------------------------------------------------------

static void TestRel32()
{
	int32_t rel = 0;
	Check(Rel32To(0x180001000ULL, 0x180001100ULL, &rel) && rel == 0x100, "rel32: a near target reaches");
	rel = 0;
	Check(Rel32To(0x180000000ULL, 0x180000000ULL + 0x7FFFFFFFULL, &rel) && rel == 0x7FFFFFFF,
	      "rel32: 2 GiB minus one forward reaches");
	Check(!Rel32To(0x180000000ULL, 0x180000000ULL + 0x80000000ULL, &rel), "rel32: beyond 2 GiB does not");
	rel = 0;
	Check(Rel32To(0x180002000ULL, 0x180001000ULL, &rel) && rel == -0x1000, "rel32: backward reaches");
}

// ---- site ---------------------------------------------------------------

static void TestSite()
{
	static const unsigned char r8Fire[12] = { 0x48,0x8B,0x8E,0x20,0x4B,0x00,0x00,0xE8,0xAA,0x2B,0x12,0x00 };
	static const unsigned char r7Fire[25] = { 0x48,0x8B,0xF9,0xC7,0x81,0x18,0x4B,0x00,0x00,0x07,0x00,0x00,0x00,
	                                          0x48,0x8B,0x89,0x20,0x4B,0x00,0x00,0xE8,0x66,0x4F,0x11,0x00 };
	const unsigned long long sync = 0x1803DFF40ULL;
	Check(ForkSiteMatches(r8Fire, r8Fire, sizeof(r8Fire), 0x1802BD391ULL, sync),
	      "site: the request-8 fire window matches");
	Check(ForkSiteMatches(r7Fire, r7Fire, sizeof(r7Fire), 0x1802CAFD5ULL, sync),
	      "site: the request-7 fire window matches");

	unsigned char changed[12];
	memcpy(changed, r8Fire, sizeof(changed));
	changed[3] = 0x28;
	Check(!ForkSiteMatches(changed, r8Fire, sizeof(r8Fire), 0x1802BD391ULL, sync), "site: one changed byte refuses");
	Check(!ForkSiteMatches(r8Fire, r8Fire, sizeof(r8Fire), 0x1802BD391ULL, sync + 0x10),
	      "site: a different target refuses");
}

// ---- inst ---------------------------------------------------------------

static unsigned long long g_batch[0x400 / 8];

static void BuildBatch(int threaded, size_t culled, bool withMgr)
{
	ResetSm();
	memset(g_batch, 0, sizeof(g_batch));
	memcpy(Sm() + SM_THREADED_INSTANCING, &threaded, sizeof(threaded));
	Put(g_batch, BATCH_SCENE_MANAGER, withMgr ? (uintptr_t)Sm() : 0);
	Put(g_batch, BATCH_CULLED_COUNT, culled);
}

static void TestInst()
{
	const unsigned char* b = (const unsigned char*)g_batch;
	BuildBatch(1, 0, true);
	Check(InstUploadDecide(true, b) == INST_EMPTY, "inst: an empty threaded batch answers empty");
	BuildBatch(1, 5, true);
	Check(InstUploadDecide(true, b) == INST_RUN, "inst: a batch with instances runs");
	BuildBatch(0, 0, true);
	Check(InstUploadDecide(true, b) == INST_UNTHREADED, "inst: single-thread instancing runs");
	BuildBatch(1, 0, false);
	Check(InstUploadDecide(true, b) == INST_RUN && InstUploadDecide(true, NULL) == INST_RUN,
	      "inst: no scene manager runs");
	BuildBatch(1, 0, true);
	Check(InstUploadDecide(false, b) == INST_OFF_MAIN, "inst: off the main thread runs");
}

// ---- rq -----------------------------------------------------------------

static void TestRq()
{
	Check(RqClearStep(1, 0) == RQ_SET_ON, "rq: switching on writes on");
	Check(RqClearStep(1, 1) == RQ_NONE, "rq: on again writes nothing");
	Check(RqClearStep(0, 1) == RQ_RESTORE, "rq: switching off restores");
	Check(RqClearStep(0, 0) == RQ_NONE, "rq: off at start writes nothing");
	Check(RqClearStep(7, 0) == RQ_NONE && RqClearStep(7, 1) == RQ_RESTORE, "rq: an unknown mode reads as off");
}

int main()
{
	TestR8();
	TestR8Thunk();
	TestR1();
	TestR7();
	TestPair();
	TestStub();
	TestRel32();
	TestSite();
	TestInst();
	TestRq();
	return CheckExit("scene_lever_policy_units");
}
