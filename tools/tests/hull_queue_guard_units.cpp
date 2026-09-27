// Host tests for the hull destroy-queue duplicate policy: every judge arm,
// the recorded-batch state machine, act/observe, a raced drop, the table
// bounds, caller packing, and a simulated game (main-thread pushes, the
// updateUT flush, the physics thread's delete loop and an allocator that
// reuses freed addresses) run with and without the guard.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "fixes/physx/hull_queue_guard_policy.h"

#include "check.h"

static const unsigned kSlots = 1u << 12;
static HqgSlot g_slots[kSlots];

struct Lists
{
	uintptr_t main[64];  unsigned nMain;
	uintptr_t back[64];  unsigned nBack;
	uintptr_t make[64];  unsigned nMake;
};

static HqgLists View(Lists* l)
{
	HqgLists v;
	v.destroyMain = l->main;       v.destroyMainCount = l->nMain;
	v.destroyBack = l->back;       v.destroyBackCount = l->nBack;
	v.makeMain = l->make;          v.makeMainCount = l->nMake;
	return v;
}

static bool PlainNull(void* ctx, unsigned index, uintptr_t expected)
{
	uintptr_t* data = (uintptr_t*)ctx;
	if (data[index] != expected)
		return false;
	data[index] = 0;
	return true;
}

static bool NeverNull(void*, unsigned, uintptr_t) { return false; }

// The flush: main onto back, then the record the shim takes after it.
static void Flush(HqgState* s, Lists* l)
{
	for (unsigned i = 0; i < l->nMain; ++i)
		l->back[l->nBack++] = l->main[i];
	l->nMain = 0;
	l->nMake = 0;
	HqgRecord(s, l->back, l->nBack);
}

// The physics thread's delete loop: the buffer is left as it was.
static void Consume(Lists* l) { l->nBack = 0; }

static HqgResult Judge(HqgState* s, Lists* l, bool act, HqgFinding* out = 0, unsigned cap = 0)
{
	HqgLists v = View(l);
	return HqgJudge(s, &v, act, &PlainNull, l->main, out, cap);
}

static void Fresh(HqgState* s, Lists* l)
{
	memset(l, 0, sizeof(*l));
	HqgInit(s, g_slots, kSlots);
}

static void TestEmptyAndDistinct()
{
	HqgState s; Lists l; Fresh(&s, &l);
	HqgResult r = Judge(&s, &l, true);
	Check(r.outcome == HQG_OUT_EMPTY, "empty main list is EMPTY");

	l.main[0] = 0x1000; l.main[1] = 0x2000; l.main[2] = 0x3000; l.nMain = 3;
	r = Judge(&s, &l, true);
	Check(r.outcome == HQG_OUT_JUDGED, "distinct judged");
	Check(r.scanned == 3 && r.dups == 0 && r.nulled == 0, "distinct hulls pass");
	Check(l.main[0] == 0x1000 && l.main[1] == 0x2000 && l.main[2] == 0x3000, "distinct untouched");
}

static void TestInList()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x1000; l.main[1] = 0x2000; l.main[2] = 0x1000; l.main[3] = 0x1000; l.nMain = 4;
	HqgFinding f[4];
	HqgResult r = Judge(&s, &l, true, f, 4);
	Check(r.dups == 2 && r.inList == 2 && r.nulled == 2, "double push in one list rejected");
	Check(l.main[0] == 0x1000 && l.main[2] == 0 && l.main[3] == 0, "first copy kept, later copies dropped");
	Check(r.findings == 2 && f[0].index == 2 && f[0].kind == HQG_DUP_IN_LIST, "finding names the second copy");
}

static void TestNullEntriesIgnored()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0; l.main[1] = 0x1000; l.main[2] = 0; l.nMain = 3;
	HqgResult r = Judge(&s, &l, true);
	Check(r.scanned == 1 && r.dups == 0, "NULL entries neither scanned nor duplicates");
}

// The recorded ScythePhysicsT route: queued before the flush, queued again
// after it, the batch deletes it, and the next flush would hand the physics
// thread the freed address.
static void TestStraddleFreed()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x5000; l.main[1] = 0x6000; l.nMain = 2;
	Judge(&s, &l, true);
	Flush(&s, &l);
	l.main[l.nMain++] = 0x5000;     // the object's own update queues it again
	Consume(&l);                    // the batch deletes 0x5000 and 0x6000
	HqgFinding f[2];
	HqgResult r = Judge(&s, &l, true, f, 2);
	Check(r.outcome == HQG_OUT_JUDGED && r.batchCount == 2, "deleted batch recognised");
	Check(r.freed == 1 && r.nulled == 1 && l.main[0] == 0, "push of a deleted, not re-made address rejected");
	Check(r.findings == 1 && f[0].kind == HQG_DUP_FREED && f[0].ptr == 0x5000, "finding is freed");
}

// A new object at the address the batch freed is registered through
// hullsToMake before it can be queued: kept.
static void TestPushAfterDrainRemade()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x5000; l.nMain = 1;
	Judge(&s, &l, true);
	Flush(&s, &l);
	Consume(&l);
	l.make[l.nMake++] = 0x5000;     // a new object at the reused address
	l.main[l.nMain++] = 0x5000;     // and its own destroy, the same frame
	HqgResult r = Judge(&s, &l, true);
	Check(r.dups == 0 && r.keptRemade == 1 && l.main[0] == 0x5000, "push after drain of a re-made address accepted");

	// The same new object queued twice is still a duplicate.
	l.main[l.nMain++] = 0x5000;
	r = Judge(&s, &l, true);
	Check(r.keptRemade == 1 && r.inList == 1 && l.main[0] == 0x5000 && l.main[1] == 0, "re-made address keeps one copy");
}

// Once a later batch has gone by, the address is no longer in the recorded
// batch, and a push of it is accepted without a make.
static void TestPushAfterDrainLater()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x5000; l.nMain = 1;
	Judge(&s, &l, true);
	Flush(&s, &l);
	Consume(&l);
	Judge(&s, &l, true);            // an empty frame
	Flush(&s, &l);                  // an empty flush: the record is now empty
	Consume(&l);
	l.main[l.nMain++] = 0x5000;
	HqgResult r = Judge(&s, &l, true);
	Check(r.dups == 0 && l.main[0] == 0x5000 && r.batchCount == 0, "push after an empty flush accepted");
}

// The physics thread has not run since the flush: the back list is still
// queued, and a second push of one of its entries is a duplicate.
static void TestPending()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x7000; l.nMain = 1;
	Judge(&s, &l, true);
	Flush(&s, &l);
	l.main[l.nMain++] = 0x7000;
	l.make[l.nMake++] = 0x7000;     // a make cannot excuse a still-queued object
	HqgResult r = Judge(&s, &l, true);
	Check(r.pending == 1 && r.nulled == 1 && l.main[0] == 0 && r.pendingCount == 1, "pending duplicate rejected");
}

static void TestNoRecordPending()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.back[0] = 0x7000; l.nBack = 1;          // before any recorded flush
	l.main[0] = 0x7000; l.main[1] = 0x8000; l.nMain = 2;
	HqgResult r = Judge(&s, &l, true);
	Check(r.pending == 1 && l.main[0] == 0 && l.main[1] == 0x8000, "unrecorded back list counts as pending");
}

static void TestStateMismatch()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x5000; l.nMain = 1;
	Judge(&s, &l, true);
	Flush(&s, &l);                  // record: back count 1
	l.back[l.nBack++] = 0x9000;     // appended outside the recorded flush
	l.main[l.nMain++] = 0x5000;
	HqgResult r = Judge(&s, &l, true);
	Check(r.outcome == HQG_OUT_SKIP_STATE && r.nulled == 0 && l.main[0] == 0x5000, "count mismatch skipped, list untouched");

	HqgState s2; Lists l2; Fresh(&s2, &l2);
	uintptr_t other[4] = { 0x5000, 0, 0, 0 };
	l2.main[0] = 0x5000; l2.nMain = 1;
	Judge(&s2, &l2, true);
	Flush(&s2, &l2);
	Consume(&l2);
	HqgRecord(&s2, other, 1);       // the buffer moved since the record
	l2.main[l2.nMain++] = 0x5000;
	r = Judge(&s2, &l2, true);
	Check(r.outcome == HQG_OUT_SKIP_STATE && l2.main[0] == 0x5000, "buffer mismatch skipped");

	// A new physics object: the old record is forgotten, so a deleted batch
	// from the old one is never read as this one's.
	HqgState s3; Lists l3; Fresh(&s3, &l3);
	l3.main[0] = 0x5000; l3.nMain = 1;
	Judge(&s3, &l3, true);
	Flush(&s3, &l3);
	Consume(&l3);
	HqgForget(&s3);
	l3.main[l3.nMain++] = 0x5000;
	r = Judge(&s3, &l3, true);
	Check(r.outcome == HQG_OUT_JUDGED && r.batchCount == 0 && r.dups == 0 && l3.main[0] == 0x5000,
	      "a forgotten record knows no deleted batch");
}

static void TestObserve()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x5000; l.nMain = 1;
	Judge(&s, &l, false);
	Flush(&s, &l);
	l.main[l.nMain++] = 0x5000;
	Consume(&l);
	l.main[l.nMain++] = 0x5000;
	HqgResult r = Judge(&s, &l, false);
	Check(r.dups == 2 && r.freed == 1 && r.inList == 1 && r.nulled == 0, "observe counts both");
	Check(l.main[0] == 0x5000 && l.main[1] == 0x5000, "observe writes nothing");
}

static void TestRaced()
{
	HqgState s; Lists l; Fresh(&s, &l);
	l.main[0] = 0x1000; l.main[1] = 0x1000; l.nMain = 2;
	HqgLists v = View(&l);
	HqgResult r = HqgJudge(&s, &v, true, &NeverNull, 0, 0, 0);
	Check(r.outcome == HQG_OUT_RACED && r.nulled == 0 && r.dups == 1, "a refused drop stops the frame as raced");
}

static void TestBounds()
{
	HqgState s; Lists l; Fresh(&s, &l);
	HqgSlot small[8];
	HqgInit(&s, small, 8);
	for (unsigned i = 0; i < 5; ++i)
		l.main[i] = 0x1000 + i * 0x100;
	l.nMain = 5;
	HqgResult r = Judge(&s, &l, true);
	Check(r.outcome == HQG_OUT_SKIP_OVERFLOW, "more than half the table is overflow");
	l.nMain = 4;
	r = Judge(&s, &l, true);
	Check(r.outcome == HQG_OUT_JUDGED, "half the table is judged");

	HqgLists v = View(&l);
	v.destroyMainCount = HQG_MAX_LIST + 1;
	r = HqgJudge(&s, &v, true, &PlainNull, l.main, 0, 0);
	Check(r.outcome == HQG_OUT_SKIP_LIST, "unbelievable count skipped");
	v = View(&l);
	v.destroyBack = 0; v.destroyBackCount = 1;
	r = HqgJudge(&s, &v, true, &PlainNull, l.main, 0, 0);
	Check(r.outcome == HQG_OUT_SKIP_LIST, "count without a buffer skipped");
}

static void TestEpochWrap()
{
	HqgState s; Lists l; Fresh(&s, &l);
	s.epoch = 0xFFFFFFFFu;
	l.main[0] = 0x1000; l.main[1] = 0x1000; l.nMain = 2;
	HqgResult r = Judge(&s, &l, true);
	Check(s.epoch == 1 && r.inList == 1, "epoch wraps without losing a duplicate");
	l.main[0] = 0x1000; l.main[1] = 0x2000; l.nMain = 2;
	r = Judge(&s, &l, true);
	Check(r.dups == 0, "no stale marks after the wrap");
}

static void TestCallers()
{
	unsigned __int64 w = HqgPushCallers(0, 0x531798);
	Check(HqgLastCaller(w) == 0x531798 && HqgPrevCaller(w) == 0, "first caller");
	w = HqgPushCallers(w, 0x7D493E);
	Check(HqgLastCaller(w) == 0x7D493E && HqgPrevCaller(w) == 0x531798, "both callers kept");
	Check(strcmp(HqgDupName(HQG_DUP_FREED), "freed") == 0 && HqgDupDrops(HQG_DUP_PENDING) &&
	      !HqgDupDrops(HQG_DUP_NONE), "names and drop rule");
}

// ---- Simulation ----------------------------------------------------------------
//
// A LIFO free list reuses freed addresses at once, as the heap's low-
// fragmentation buckets do. Objects are created through hullsToMake, and
// destroyed by one legitimate push, sometimes followed by a second push:
// in the same frame, inside updateUT after its flush (the recorded route),
// or while the physics thread runs. The physics thread deletes the back list;
// a delete of a dead address is a double free, and a delete of a live object
// nobody queued is a live delete.

struct Sim
{
	static const int N = 48;
	bool     alive[N];
	bool     doomed[N];    // its destroy was legitimately queued
	int      freeList[N];
	int      nFree;
	int      doubleFrees;
	int      liveDeletes;
	Lists    l;
	HqgState s;
	uintptr_t lateA[16]; int nLateA;   // queued again inside updateUT, after the flush
	uintptr_t lateB[16]; int nLateB;   // queued again while the physics thread runs
};

static uintptr_t Addr(int i) { return 0x10000 + (uintptr_t)i * 0x40; }
static int Index(uintptr_t a) { return (int)((a - 0x10000) / 0x40); }

static void SimInit(Sim* m)
{
	memset(m, 0, sizeof(*m));
	HqgInit(&m->s, g_slots, kSlots);
	for (int i = Sim::N - 1; i >= 0; --i)
		m->freeList[m->nFree++] = i;
}

static void SimJudge(Sim* m, bool guard)
{
	HqgLists v = View(&m->l);
	HqgJudge(&m->s, &v, guard, &PlainNull, m->l.main, 0, 0);
}

static void SimPhysics(Sim* m)
{
	for (unsigned k = 0; k < m->l.nBack; ++k)
	{
		uintptr_t p = m->l.back[k];
		if (!p)
			continue;
		int i = Index(p);
		if (!m->alive[i])
		{
			++m->doubleFrees;
			continue;
		}
		if (!m->doomed[i])
			++m->liveDeletes;
		m->alive[i] = false;
		m->freeList[m->nFree++] = i;
	}
	Consume(&m->l);
}

static void SimFrame(Sim* m, bool guard, bool lateB, unsigned* rng)
{
	// Main thread, before updateUT.
	for (int k = 0; k < 6; ++k)
	{
		*rng = *rng * 1103515245u + 12345u;
		unsigned x = (*rng >> 8) % 100;
		if (x < 40 && m->nFree > 0 && m->l.nMake < 60)
		{
			int i = m->freeList[--m->nFree];
			m->alive[i] = true;
			m->doomed[i] = false;
			m->l.make[m->l.nMake++] = Addr(i);
			if (x < 8 && m->l.nMain < 60)
			{
				m->doomed[i] = true;             // made and destroyed in one frame
				m->l.main[m->l.nMain++] = Addr(i);
			}
		}
		else
		{
			int i = (int)((*rng >> 4) % Sim::N);
			if (!m->alive[i] || m->doomed[i] || m->l.nMain >= 58)
				continue;
			m->doomed[i] = true;
			m->l.main[m->l.nMain++] = Addr(i);
			if (x >= 88)
				m->l.main[m->l.nMain++] = Addr(i);
			else if (x >= 76 && m->nLateA < 16)
				m->lateA[m->nLateA++] = Addr(i);
			else if (lateB && x >= 64 && m->nLateB < 16)
				m->lateB[m->nLateB++] = Addr(i);
		}
	}

	// updateUT: judge, flush, record, the late queueings, the second pass.
	SimJudge(m, guard);
	Flush(&m->s, &m->l);
	for (int k = 0; k < m->nLateA; ++k)
		m->l.main[m->l.nMain++] = m->lateA[k];
	m->nLateA = 0;
	SimJudge(m, guard);

	// Main-thread work that overlaps the physics thread, before it deletes.
	for (int k = 0; k < m->nLateB; ++k)
		m->l.main[m->l.nMain++] = m->lateB[k];
	m->nLateB = 0;
	SimPhysics(m);
}

struct SimTotals { int dbl, live, leaks; };

static SimTotals RunSim(bool guard, bool lateB)
{
	SimTotals t = { 0, 0, 0 };
	for (unsigned seed = 1; seed <= 40; ++seed)
	{
		Sim* m = (Sim*)malloc(sizeof(Sim));
		unsigned rng = seed;
		SimInit(m);
		for (int f = 0; f < 400; ++f)
			SimFrame(m, guard, lateB, &rng);
		for (int f = 0; f < 3; ++f)
		{
			SimJudge(m, guard);
			Flush(&m->s, &m->l);
			SimJudge(m, guard);
			SimPhysics(m);
		}
		t.dbl += m->doubleFrees;
		t.live += m->liveDeletes;
		for (int i = 0; i < Sim::N; ++i)
			if (m->alive[i] && m->doomed[i])
				++t.leaks;
		free(m);
	}
	return t;
}

static void TestSimulation()
{
	SimTotals bare  = RunSim(false, false);
	SimTotals guard = RunSim(true, false);
	Check(bare.dbl + bare.live > 0, "the unguarded model reproduces double deletes (the model is live)");
	Check(guard.dbl == 0 && guard.live == 0 && guard.leaks == 0,
	      "guarded, in-list and post-flush routes: every object deleted exactly once");

	// A second queueing while the physics thread runs is judged a frame later.
	// Only when a new object is made at the freed address in that frame is the
	// entry kept -- the unguarded outcome for that one entry -- and no double
	// free remains.
	SimTotals bareB  = RunSim(false, true);
	SimTotals guardB = RunSim(true, true);
	Check(guardB.dbl == 0 && guardB.leaks == 0, "guarded, with physics-time pushes: no double free, no leak");
	Check(guardB.live <= bareB.live, "a kept re-made entry never does worse than the game alone");
	printf("simulation: unguarded dbl=%d live=%d, guarded dbl=%d live=%d leaks=%d; "
	       "with physics-time pushes unguarded dbl=%d live=%d, guarded dbl=%d live=%d leaks=%d\n",
	       bare.dbl, bare.live, guard.dbl, guard.live, guard.leaks,
	       bareB.dbl, bareB.live, guardB.dbl, guardB.live, guardB.leaks);
}

int main()
{
	TestEmptyAndDistinct();
	TestInList();
	TestNullEntriesIgnored();
	TestStraddleFreed();
	TestPushAfterDrainRemade();
	TestPushAfterDrainLater();
	TestPending();
	TestNoRecordPending();
	TestStateMismatch();
	TestObserve();
	TestRaced();
	TestBounds();
	TestEpochWrap();
	TestCallers();
	TestSimulation();
	return CheckExit("hull_queue_guard_units");
}
