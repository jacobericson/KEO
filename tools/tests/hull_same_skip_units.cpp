// Host tests for the click-hull same-target skip: the decision for every
// input, the sequences the physics thread runs, the clear, the table's
// half-full reset, the probe window, the field reads and the thunk check.

#include <cstdio>
#include <cstring>
#include "fixes/physx/hull_same_skip_policy.h"

#include "check.h"

static HullSameEntry g_big[HULL_SAME_SLOTS];
static HullSameEntry g_small[128];
static HullSameEntry g_tiny[4];

static void Words(unsigned out[3], float x, float y, float z)
{
	float f[3] = { x, y, z };
	memcpy(out, f, sizeof(f));
}

// The home-slot formula, written out a second time so the probe case can
// fill a hull's window without reaching into the policy.
static unsigned TestSlot(uintptr_t key, unsigned mask)
{
	unsigned long long v = (unsigned long long)key;
	v ^= v >> 29;
	v *= 0x9E3779B97F4A7C15ULL;
	return (unsigned)(v >> 32) & mask;
}

static HullApplyAction Step(HullSameTable* t, uintptr_t hull, uintptr_t actor, bool teleport,
                            const unsigned pos[3])
{
	bool emptied = false;
	return HullSameStep(t, hull, actor, teleport, pos, &emptied);
}

static void TestDecide()
{
	HullSameTable t;
	unsigned p[3];
	Words(p, 10.0f, 20.0f, 30.0f);
	const uintptr_t hull = 0x2000;
	const uintptr_t actor = 0x9000;

	// 1. no actor
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		bool ok = Step(&t, hull, 0, false, p) == HULL_FORWARD_CREATE
		       && Step(&t, hull, 0, true, p) == HULL_FORWARD_CREATE
		       && t.used == 0;
		Step(&t, hull, actor, false, p);   // a stored match for the same hull
		ok = ok && Step(&t, hull, 0, false, p) == HULL_FORWARD_CREATE
		        && Step(&t, hull, 0, true, p) == HULL_FORWARD_CREATE
		        && t.used == 1;
		Check(ok, "decide: no actor is a create, whatever else");
	}

	// 2. teleports
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Check(Step(&t, hull, actor, true, p) == HULL_FORWARD_TELEPORT,
		      "decide: a latched teleport is forwarded");
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Step(&t, hull, actor, false, p);
		Check(Step(&t, hull, actor, true, p) == HULL_FORWARD_TELEPORT,
		      "decide: a teleport is never skipped even with a stored match");
	}

	// 3. unknown hull
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Check(Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE, "decide: an unknown hull is a move");
	}

	// 4. the same move twice
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		bool ok = Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_SKIP;
		Check(ok, "decide: the same hull, actor and target is skipped");
	}

	// 5. one bit apart
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Step(&t, hull, actor, false, p);
		unsigned q[3] = { p[0] ^ 1u, p[1], p[2] };
		Check(Step(&t, hull, actor, false, q) == HULL_FORWARD_MOVE, "decide: a target differing in one bit is a move");
	}

	// 6. -0.0 against 0.0
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		unsigned a[3] = { 0x80000000u, p[1], p[2] };
		unsigned b[3] = { 0u, p[1], p[2] };
		Step(&t, hull, actor, false, a);
		Check(Step(&t, hull, actor, false, b) == HULL_FORWARD_MOVE, "decide: negative zero against zero is a move");
	}

	// 7. the same NaN
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		unsigned n[3] = { 0x7FC00001u, p[1], p[2] };
		Step(&t, hull, actor, false, n);
		Check(Step(&t, hull, actor, false, n) == HULL_SKIP, "decide: the same NaN bits are skipped");
	}

	// 8. a new actor at the same hull
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Step(&t, hull, actor, false, p);
		Check(Step(&t, hull, actor + 0x100, false, p) == HULL_FORWARD_MOVE, "decide: a new actor at the same hull is a move");
	}
}

static void TestSequences()
{
	HullSameTable t;
	unsigned p[3], q[3];
	Words(p, 1.5f, -2.25f, 100.0f);
	Words(q, 1.5f, -2.25f, 101.0f);
	const uintptr_t hull = 0x7FF612345670ull;
	const uintptr_t actor = 0x1D4C2A30000ull;

	// 9. create, teleport, then the same target
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		bool ok = Step(&t, hull, 0, true, p) == HULL_FORWARD_CREATE;
		ok = ok && Step(&t, hull, actor, true, p) == HULL_FORWARD_TELEPORT;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_SKIP;
		Check(ok, "sequence: create, teleport, then the same target is skipped");
	}

	// 10. a move after a skip
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		bool ok = Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_SKIP;
		ok = ok && Step(&t, hull, actor, false, q) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, q) == HULL_SKIP;
		Check(ok, "sequence: a move after a skip forwards, and its target is skipped next");
	}

	// 11. clear
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		Step(&t, hull, actor, false, p);
		HullSameClear(&t);
		bool ok = t.used == 0;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_SKIP;
		Check(ok, "clear: an emptied table forwards the next same target");
	}

	// 16. a create at a stored hull, then the old actor and target again:
	// the pair may be reused addresses, so the move is forwarded.
	{
		HullSameInit(&t, g_big, HULL_SAME_SLOTS);
		bool ok = Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, 0, false, q) == HULL_FORWARD_CREATE;
		ok = ok && t.used == 1;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_SKIP;
		ok = ok && t.used == 1;
		Check(ok, "sequence: a create forgets the hull's stored target");
	}
}

static void TestCapacity()
{
	// 12. distinct hulls until the table holds half its slots; the next new
	// hull empties it first and is then stored, so its own target skips.
	HullSameTable t;
	HullSameInit(&t, g_big, HULL_SAME_SLOTS);
	unsigned p[3];
	Words(p, 4.0f, 5.0f, 6.0f);
	const uintptr_t actor = 0x5000;
	bool ok = true;
	uintptr_t h = 0x10000000ull;
	unsigned tries = 0;
	while (t.used < t.count / 2 && tries < t.count * 4)
	{
		bool emptied = true;
		ok = ok && HullSameStep(&t, h, actor, false, p, &emptied) == HULL_FORWARD_MOVE && !emptied;
		h += 0x60;
		++tries;
	}
	ok = ok && t.used == t.count / 2;

	bool emptied = false;
	ok = ok && HullSameStep(&t, h, actor, false, p, &emptied) == HULL_FORWARD_MOVE && emptied;
	ok = ok && t.used == 1;
	bool emptied2 = true;
	ok = ok && HullSameStep(&t, h, actor, false, p, &emptied2) == HULL_SKIP && !emptied2;
	Check(ok, "capacity: the table empties itself at half full and keeps working");
}

static void TestProbe()
{
	// 13. every slot of one hull's window holds another key, with used left
	// at 0: nothing is stored and the same target is forwarded twice.
	unsigned p[3];
	Words(p, 7.0f, 8.0f, 9.0f);
	const uintptr_t hull = 0x3000;
	const uintptr_t actor = 0x6000;
	bool ok = true;

	{
		HullSameTable t;
		HullSameInit(&t, g_small, 128);
		const unsigned mask = t.count - 1;
		unsigned home = TestSlot(hull, mask);
		for (unsigned n = 0; n < HULL_SAME_PROBES; ++n)
			t.slots[(home + n) & mask].hull = 0x100000 + n * 0x10;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && t.used == 0;
	}
	{
		// A table smaller than the window reads each slot once.
		HullSameTable t;
		HullSameInit(&t, g_tiny, 4);
		for (unsigned n = 0; n < 4; ++n)
			t.slots[n].hull = 0x200000 + n * 0x10;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && Step(&t, hull, actor, false, p) == HULL_FORWARD_MOVE;
		ok = ok && t.used == 0;
	}
	Check(ok, "probe: a full probe window stores nothing and forwards");
}

static void TestRead()
{
	// 14.
	unsigned char buf[0x60];
	memset(buf, 0xCD, sizeof(buf));
	const uintptr_t actor = 0x00000123456789A0ull;
	memcpy(buf + 0x50, &actor, sizeof(actor));
	buf[0x34] = 1;
	unsigned w[3];
	Words(w, -1.0f, 2.5f, 3.75f);
	memcpy(buf + 0x38, w, sizeof(w));

	uintptr_t a = 0;
	bool tp = false;
	unsigned pos[3] = { 0, 0, 0 };
	HullSameRead(buf, &a, &tp, pos);
	bool ok = a == actor && tp && pos[0] == w[0] && pos[1] == w[1] && pos[2] == w[2];
	buf[0x34] = 0;
	HullSameRead(buf, &a, &tp, pos);
	ok = ok && !tp;
	Check(ok, "read: the apply's fields are read at their offsets");
}

static void TestThunk()
{
	// 15. the bytes at 0x259F5: E9 E6 56 4A 00, rel32 to 0x4CB0E0.
	const unsigned char real[5] = { 0xE9, 0xE6, 0x56, 0x4A, 0x00 };
	const uintptr_t base = 0x140000000ull;
	Check(HullSameThunkOk(real, base + 0x259F5, base + 0x4CB0E0), "thunk: an E9 to the apply is accepted");
	Check(!HullSameThunkOk(real, base + 0x259F5, base + 0x4CB0F0), "thunk: an E9 elsewhere is refused");
	const unsigned char other[5] = { 0xE8, 0xE6, 0x56, 0x4A, 0x00 };
	Check(!HullSameThunkOk(other, base + 0x259F5, base + 0x4CB0E0), "thunk: another opcode is refused");
}

int main()
{
	TestDecide();
	TestSequences();
	TestCapacity();
	TestProbe();
	TestRead();
	TestThunk();
	return CheckExit("hull_same_skip_units");
}
