// Host tests for the hull-queue lifecycle table (AuditHullsTable.h).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../profiler/AuditHullsTable.h"

using namespace hulls;

#include "check.h"

const int K_PUSH = 1, K_INLINE = 6;

struct Fixture
{
	Slot* mem;
	Table t;
	explicit Fixture(unsigned count = 1024)
	{
		mem = (Slot*)calloc(count, sizeof(Slot));
		t.Init(mem, count);
	}
	~Fixture() { free(mem); }
	int State(uintptr_t p) { Slot* s = t.Find(p, false); return s ? StateOf(s->word) : -1; }
};

// Normal flow: hooked push, flush, batch, consumer destructor, batch end.
static void NormalFlow()
{
	Fixture f;
	uintptr_t p = 0x10000;
	Check(f.t.Make(p).anomaly == AN_NONE, "make");
	Result r = f.t.Push(p, K_PUSH, 0x7DC1D4, 1);
	Check(r.anomaly == AN_NONE && f.State(p) == ST_QMAIN, "push -> QMAIN");
	r = f.t.FlushEntry(p, K_INLINE, 2);
	Check(r.anomaly == AN_NONE && !r.inlinePush && f.State(p) == ST_QBACK, "flush -> QBACK, not inline");
	Check(f.t.BatchEntry(p).anomaly == AN_NONE && f.State(p) == ST_DESTROYING, "batch -> DESTROYING");
	Check(f.t.Dtor(p, true, 3).anomaly == AN_NONE && f.State(p) == ST_DEAD, "consumer dtor -> DEAD");
	f.t.BatchExit(p);
	Check(f.State(p) == ST_DEAD, "batch exit keeps DEAD");

	// Address reused by a new hull, queued again: no anomaly.
	Check(f.t.Make(p).anomaly == AN_NONE && f.State(p) == ST_MADE, "reuse: make");
	Check(f.t.Push(p, K_PUSH, 0x7DC1D4, 4).anomaly == AN_NONE, "reuse: push");
}

// Pushes no hooked pusher reports are found at the flush; unhooked
// destructors are covered by the batch end.
static void InlineFlow()
{
	Fixture f;
	uintptr_t p = 0x20000;
	f.t.Make(p);
	Result r = f.t.FlushEntry(p, K_INLINE, 1);
	Check(r.anomaly == AN_NONE && r.inlinePush && f.State(p) == ST_QBACK, "inline push found at flush");
	Check(KindOf(f.t.Find(p, false)->word) == K_INLINE, "inline kind stored");
	f.t.BatchEntry(p);
	f.t.BatchExit(p);
	Check(f.State(p) == ST_DEAD, "unhooked destructor: batch exit -> DEAD");
}

static void DupPush()
{
	// Twice in the same main list through a hooked pusher: one anomaly.
	Fixture f;
	uintptr_t p = 0x30000;
	f.t.Push(p, K_PUSH, 0x100, 1);
	Result r = f.t.Push(p, K_PUSH, 0x200, 2);
	Check(r.anomaly == AN_DUP_PUSH && CallerOf(r.prev) == 0x100, "hooked dup push keeps the first caller");
	Check(f.t.FlushEntry(p, K_INLINE, 3).anomaly == AN_NONE, "flush copy 1");
	Check(f.t.FlushEntry(p, K_INLINE, 3).anomaly == AN_NONE, "flush copy 2 already counted");
	Check(f.t.BatchEntry(p).anomaly == AN_NONE, "batch copy 1");
	Check(f.t.BatchEntry(p).anomaly == AN_NONE, "batch copy 2");
	f.t.Dtor(p, true, 4);
	f.t.BatchExit(p);
	Check(f.State(p) == ST_DEAD, "dup in one batch ends DEAD");

	// Twice in one main list, both inline: counted at the flush.
	Fixture g;
	uintptr_t q = 0x31000;
	Check(g.t.FlushEntry(q, K_INLINE, 1).anomaly == AN_NONE, "inline copy 1");
	Result d = g.t.FlushEntry(q, K_INLINE, 1);
	Check(d.anomaly == AN_DUP_PUSH && d.inlinePush, "inline copy 2 is a dup push");

	// Pushed again after the flush, before the batch: one anomaly, and the
	// second copy stays queued after the first is destroyed.
	Fixture h;
	uintptr_t s = 0x32000;
	h.t.Push(s, K_PUSH, 0x100, 1);
	h.t.FlushEntry(s, K_INLINE, 2);
	Check(h.t.Push(s, K_PUSH, 0x300, 3).anomaly == AN_DUP_PUSH, "push while in the back list");
	h.t.BatchEntry(s);
	h.t.Dtor(s, true, 4);
	h.t.BatchExit(s);
	Check(h.State(s) == ST_QMAIN, "second copy still queued after the first delete");
	Check(h.t.FlushEntry(s, K_INLINE, 5).anomaly == AN_NONE, "second copy flushed without a new anomaly");
}

static void AfterDtor()
{
	Fixture f;
	uintptr_t p = 0x40000;
	f.t.Push(p, K_PUSH, 0x100, 1);
	f.t.FlushEntry(p, K_INLINE, 2);
	f.t.BatchEntry(p);
	f.t.Dtor(p, true, 3);
	f.t.BatchExit(p);
	Result r = f.t.Push(p, K_PUSH, 0x200, 4);
	Check(r.anomaly == AN_PUSH_AFTER_DTOR && CallerOf(r.prev) == 0x100 && r.prevDtorQpc == 3,
	      "push after dtor reports the previous caller and dtor time");

	Fixture g;
	uintptr_t q = 0x41000;
	g.t.Dtor(q, false, 1);
	Result i = g.t.FlushEntry(q, K_INLINE, 2);
	Check(i.anomaly == AN_PUSH_AFTER_DTOR && i.inlinePush, "inline push after dtor");
}

static void ForeignDtor()
{
	Fixture f;
	uintptr_t p = 0x50000;
	f.t.Push(p, K_PUSH, 0x100, 1);
	f.t.FlushEntry(p, K_INLINE, 2);
	Check(f.t.Dtor(p, false, 3).anomaly == AN_DTOR_QUEUED, "free while queued");
	Check(f.t.BatchEntry(p).anomaly == AN_BATCH_UNQUEUED, "the freed entry then reaches the batch");

	Fixture g;
	uintptr_t q = 0x51000;
	g.t.Make(q);
	Check(g.t.Dtor(q, false, 1).anomaly == AN_DTOR_UNQUEUED, "direct delete of an unqueued hull");
}

static void BatchAndMake()
{
	// In the batch without passing the flush scan.
	Fixture f;
	uintptr_t p = 0x60000;
	f.t.Push(p, K_PUSH, 0x100, 1);
	Check(f.t.BatchEntry(p).anomaly == AN_BATCH_UNQUEUED, "pushed but never flushed");
	Check(f.t.BatchEntry(0x61000).anomaly == AN_BATCH_UNQUEUED, "never seen at all");

	// Created and queued for destroy in the same frame.
	Fixture g;
	uintptr_t q = 0x62000;
	g.t.Push(q, K_PUSH, 0x100, 1);
	Check(g.t.Make(q).anomaly == AN_NONE && g.State(q) == ST_QMAIN, "same-frame create and destroy");

	// A new hull at an address still waiting in the back list.
	Fixture h;
	uintptr_t s = 0x63000;
	h.t.Push(s, K_PUSH, 0x100, 1);
	h.t.FlushEntry(s, K_INLINE, 2);
	Check(h.t.Make(s).anomaly == AN_MAKE_QUEUED && h.State(s) == ST_MADE, "make while queued");
}

static void Overflow()
{
	Fixture f(64);
	int tracked = 0;
	for (uintptr_t i = 1; i <= 100; ++i)
		if (f.t.Push(i * 0x40, K_PUSH, 0, 1).tracked)
			++tracked;
	Check(tracked == 64 && f.t.overflow == 36, "overflow counted, never blocks");
	Check(f.t.Find(0x40, false) != NULL, "early keys stay");
}

static void Packing()
{
	LONG64 w = Pack(ST_QBACK, 2, 0xAB, 0xDEADBEEF);
	Check(StateOf(w) == ST_QBACK && ExtraOf(w) == 2 && KindOf(w) == 0xAB && CallerOf(w) == 0xDEADBEEF, "pack");
	LONG64 v = WithState(w, ST_DEAD, 0);
	Check(StateOf(v) == ST_DEAD && ExtraOf(v) == 0 && KindOf(v) == 0xAB && CallerOf(v) == 0xDEADBEEF,
	      "state change keeps the pusher");
}

int main()
{
	Packing();
	NormalFlow();
	InlineFlow();
	DupPush();
	AfterDtor();
	ForeignDtor();
	BatchAndMake();
	Overflow();
	return CheckExit("hull_units");
}
