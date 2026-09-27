// Lifecycle table for PhysX hull addresses in PhysicsInterface::hullsToDestroy.
// One slot per address, updated lock-free from any thread. A key is claimed
// once and never removed, so a slot keeps the address's last known place in
// the queue, its last pusher and when it was last destroyed.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>

namespace hulls
{

// Where an address stands. QMAIN: pushed into the main-thread list by a
// hooked pusher. QBACK: seen by the scan just before the flush hands the list
// to the physics thread. DESTROYING: in the batch threadJunkPreBT is deleting.
enum State { ST_NONE, ST_MADE, ST_QMAIN, ST_QBACK, ST_DESTROYING, ST_DEAD, ST_COUNT };

enum Anomaly
{
	AN_NONE = -1,
	AN_DUP_PUSH,          // pushed while already queued
	AN_PUSH_AFTER_DTOR,   // pushed after its destructor ran, with no make in between
	AN_OFF_MAIN_PUSH,     // a hooked pusher ran off the main thread (decided by the caller)
	AN_DTOR_UNQUEUED,     // destructor outside the consumer loop on an unqueued address
	AN_DTOR_QUEUED,       // destructor outside the consumer loop while queued
	AN_BATCH_UNQUEUED,    // in the physics thread's batch without passing the flush scan
	AN_MAKE_QUEUED,       // registered as a new hull while still in the destroy queue
	AN_COUNT
};

struct Slot
{
	volatile LONG64 key;      // hull address, 0 = free
	volatile LONG64 word;     // Pack(state, extra copies, push kind, push caller RVA)
	volatile LONG64 pushQpc;  // last push
	volatile LONG64 dtorQpc;  // last destructor
};

// Queued copies beyond the first that a word can count (2 bits).
const int MAX_EXTRA = 3;

inline LONG64 Pack(int state, int extra, int kind, unsigned callerRva)
{
	return (LONG64)((unsigned long long)(state & 0xF) |
	                ((unsigned long long)(extra & 0x3) << 4) |
	                ((unsigned long long)(kind & 0xFF) << 8) |
	                ((unsigned long long)callerRva << 32));
}
inline int      StateOf(LONG64 w)  { return (int)(w & 0xF); }
inline int      ExtraOf(LONG64 w)  { return (int)((w >> 4) & 0x3); }
inline int      KindOf(LONG64 w)   { return (int)((w >> 8) & 0xFF); }
inline unsigned CallerOf(LONG64 w) { return (unsigned)((unsigned long long)w >> 32); }
inline LONG64   WithState(LONG64 w, int state, int extra)
{
	return (w & ~(LONG64)0x3F) | (LONG64)(state & 0xF) | ((LONG64)(extra & 0x3) << 4);
}

struct Result
{
	int    anomaly;
	bool   inlinePush;   // FlushEntry: a push no hooked pusher reported
	bool   tracked;      // false: no slot (table full)
	LONG64 prev;         // the word before this event
	LONG64 prevPushQpc;
	LONG64 prevDtorQpc;
};

class Table
{
public:
	Table() : m_slots(NULL), m_mask(0), m_shift(64), overflow(0) {}

	// slots: zeroed, count a power of two.
	void Init(Slot* slots, unsigned count)
	{
		unsigned bits = 0;
		while ((1u << bits) < count)
			++bits;
		m_slots = slots;
		m_mask  = count - 1;
		m_shift = 64 - bits;
	}

	bool Ready() const { return m_slots != NULL; }

	Slot* Find(uintptr_t p, bool insert)
	{
		if (!m_slots || !p)
			return NULL;
		unsigned long long h = ((unsigned long long)p >> 3) * 0x9E3779B97F4A7C15ull;
		unsigned i = (unsigned)(m_shift >= 64 ? 0 : (h >> m_shift));
		for (unsigned n = 0; n < PROBE_MAX; ++n)
		{
			Slot* s = &m_slots[(i + n) & m_mask];
			LONG64 k = s->key;
			if (k == (LONG64)p)
				return s;
			if (k == 0)
			{
				if (!insert)
					return NULL;
				LONG64 was = InterlockedCompareExchange64(&s->key, (LONG64)p, 0);
				if (was == 0 || was == (LONG64)p)
					return s;
			}
		}
		if (insert)
			InterlockedIncrement(&overflow);
		return NULL;
	}

	// A hooked pusher is about to append p to the main-thread list.
	Result Push(uintptr_t p, int kind, unsigned callerRva, LONG64 qpc)
	{
		Result r = Blank();
		Slot* s = Find(p, true);
		if (!s) { r.tracked = false; return r; }
		LONG64 old, nw;
		do
		{
			old = s->word;
			int st = StateOf(old), ex = ExtraOf(old);
			r.anomaly = AN_NONE;
			if (st == ST_QMAIN || st == ST_QBACK || st == ST_DESTROYING)
			{
				r.anomaly = AN_DUP_PUSH;
				nw = Pack(st, ex < MAX_EXTRA ? ex + 1 : ex, kind, callerRva);
			}
			else
			{
				if (st == ST_DEAD)
					r.anomaly = AN_PUSH_AFTER_DTOR;
				nw = Pack(ST_QMAIN, 0, kind, callerRva);
			}
		} while (InterlockedCompareExchange64(&s->word, nw, old) != old);
		Finish(r, s, old);
		s->pushQpc = qpc;
		return r;
	}

	// One entry of the main-thread list, just before the flush moves it.
	Result FlushEntry(uintptr_t p, int inlineKind, LONG64 qpc)
	{
		Result r = Blank();
		Slot* s = Find(p, true);
		if (!s) { r.tracked = false; return r; }
		LONG64 old, nw;
		do
		{
			old = s->word;
			int st = StateOf(old), ex = ExtraOf(old);
			r.anomaly = AN_NONE;
			r.inlinePush = false;
			if (st == ST_QMAIN)
				nw = WithState(old, ST_QBACK, ex);
			else if (st == ST_QBACK || st == ST_DESTROYING)
			{
				if (ex > 0)
					nw = WithState(old, st, ex - 1);   // a copy the pusher already reported
				else
				{
					r.anomaly = AN_DUP_PUSH;
					r.inlinePush = true;
					nw = Pack(st, 0, inlineKind, 0);
				}
			}
			else
			{
				if (st == ST_DEAD)
					r.anomaly = AN_PUSH_AFTER_DTOR;
				r.inlinePush = true;
				nw = Pack(ST_QBACK, 0, inlineKind, 0);
			}
		} while (InterlockedCompareExchange64(&s->word, nw, old) != old);
		Finish(r, s, old);
		if (r.inlinePush)
			s->pushQpc = qpc;
		return r;
	}

	// One entry of the batch threadJunkPreBT is about to delete. A second copy
	// of an address in one batch is not flagged here: the push that queued it
	// was already counted as dupPush (by Push or FlushEntry).
	Result BatchEntry(uintptr_t p)
	{
		Result r = Blank();
		Slot* s = Find(p, true);
		if (!s) { r.tracked = false; return r; }
		LONG64 old, nw;
		do
		{
			old = s->word;
			int st = StateOf(old);
			r.anomaly = AN_NONE;
			if (st == ST_QBACK)
				nw = WithState(old, ST_DESTROYING, ExtraOf(old));
			else if (st == ST_DESTROYING)
				nw = old;                             // a further copy, counted when pushed
			else
			{
				r.anomaly = AN_BATCH_UNQUEUED;
				nw = WithState(old, ST_DESTROYING, 0);
			}
		} while (nw != old && InterlockedCompareExchange64(&s->word, nw, old) != old);
		Finish(r, s, old);
		return r;
	}

	// The batch is done: anything still DESTROYING had a destructor we do not hook.
	void BatchExit(uintptr_t p)
	{
		Slot* s = Find(p, false);
		if (!s)
			return;
		LONG64 old, nw;
		do
		{
			old = s->word;
			if (StateOf(old) != ST_DESTROYING)
				return;
			nw = Released(old);
		} while (InterlockedCompareExchange64(&s->word, nw, old) != old);
	}

	// A hooked deleting destructor is about to run on p. consumer: called
	// from threadJunkPreBT's delete loop.
	Result Dtor(uintptr_t p, bool consumer, LONG64 qpc)
	{
		Result r = Blank();
		Slot* s = Find(p, true);
		if (!s) { r.tracked = false; return r; }
		LONG64 old, nw;
		do
		{
			old = s->word;
			int st = StateOf(old);
			bool queued = st == ST_QMAIN || st == ST_QBACK || st == ST_DESTROYING;
			r.anomaly = AN_NONE;
			if (consumer)
				nw = st == ST_DESTROYING ? Released(old) : WithState(old, ST_DEAD, 0);
			else
			{
				r.anomaly = queued ? AN_DTOR_QUEUED : AN_DTOR_UNQUEUED;
				nw = WithState(old, ST_DEAD, 0);
			}
		} while (InterlockedCompareExchange64(&s->word, nw, old) != old);
		Finish(r, s, old);
		s->dtorQpc = qpc;
		return r;
	}

	// One entry of hullsToMake's main-thread list: a new hull at p.
	Result Make(uintptr_t p)
	{
		Result r = Blank();
		Slot* s = Find(p, true);
		if (!s) { r.tracked = false; return r; }
		LONG64 old, nw;
		do
		{
			old = s->word;
			int st = StateOf(old);
			r.anomaly = AN_NONE;
			if (st == ST_QMAIN)
				nw = old;                             // created and queued for destroy in one frame
			else
			{
				if (st == ST_QBACK || st == ST_DESTROYING)
					r.anomaly = AN_MAKE_QUEUED;
				nw = Pack(ST_MADE, 0, 0, 0);
			}
		} while (nw != old && InterlockedCompareExchange64(&s->word, nw, old) != old);
		Finish(r, s, old);
		return r;
	}

	volatile LONG overflow;   // events with no free slot within PROBE_MAX

private:
	static const unsigned PROBE_MAX = 64;

	static Result Blank()
	{
		Result r;
		r.anomaly = AN_NONE;
		r.inlinePush = false;
		r.tracked = true;
		r.prev = 0;
		r.prevPushQpc = 0;
		r.prevDtorQpc = 0;
		return r;
	}

	// A copy leaves the queue: another pushed copy may still be in the main list.
	static LONG64 Released(LONG64 w)
	{
		int ex = ExtraOf(w);
		return ex > 0 ? WithState(w, ST_QMAIN, ex - 1) : WithState(w, ST_DEAD, 0);
	}

	static void Finish(Result& r, Slot* s, LONG64 old)
	{
		r.prev = old;
		r.prevPushQpc = s->pushQpc;
		r.prevDtorQpc = s->dtorQpc;
	}

	Slot*    m_slots;
	unsigned m_mask;
	unsigned m_shift;
};

} // namespace hulls
