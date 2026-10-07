// Host tests for the hull pose table (audit_pose_table.h).

#include "../../profiler/audit_pose_table.h"

using namespace hullpose;

#include "check.h"

static Entry g_small[64];
static Entry g_large[1024];

// The class precedence: create, teleport, first, same.
static void Precedence()
{
	Table t;
	t.Init(g_small, 64);
	uintptr_t h = 0x10000;
	Check(t.Classify(h, false, true, 1, 2, 3) == PC_CREATE,
	      "a submission with no actor is a create, teleport flag or not");
	Check(t.Classify(h, true, true, 1, 2, 3) == PC_TELEPORT, "the teleport flag outranks a known pose");
	Check(t.Classify(h, true, false, 1, 2, 3) == PC_SAME, "create and teleport store the pose they submitted");
	uintptr_t g = 0x20000;
	Check(t.Classify(g, true, false, 5, 5, 5) == PC_FIRST, "a hull seen for the first time is first");
	Check(t.Classify(g, true, false, 5, 5, 5) == PC_SAME, "an identical pose is same");
}

// The tiny threshold is a length, and only an exact repeat is same.
static void Distances()
{
	Table t;
	t.Init(g_small, 64);
	t.Classify(0x10000, true, false, 0, 0, 0);
	Check(t.Classify(0x10000, true, false, 0.009f, 0, 0) == PC_TINY, "a move shorter than the threshold is tiny");
	Check(t.Classify(0x10000, true, false, 0.009f, 0, 0) == PC_SAME, "the table keeps the last pose, not the first");
	Check(t.Classify(0x10000, true, false, 0.009f + 0.0101f, 0, 0) == PC_MOVED,
	      "a move of the threshold or more is moved");
	t.Classify(0x20000, true, false, 0, 0, 0);
	Check(t.Classify(0x20000, true, false, 0.006f, 0.006f, 0.006f) == PC_MOVED,
	      "a move under the threshold on every axis but over it in length is moved");
	t.Classify(0x30000, true, false, 0, 0, 0);
	Check(t.Classify(0x30000, true, false, 0.005f, 0.005f, 0) == PC_TINY,
	      "a diagonal move under the threshold in length is tiny");
	t.Classify(0x40000, true, false, 100, 200, 300);
	Check(t.Classify(0x40000, true, false, 100, 200, 300.00003f) == PC_TINY, "only an exact repeat is same");
}

// The table empties once half its slots hold a key.
static void ClearRule()
{
	Table t;
	t.Init(g_small, 64);
	uintptr_t k = 0x1000;
	while (t.used < 31)
	{
		t.Classify(k, true, false, 1, 1, 1);
		k += 0x1000;
	}
	Check(!t.ClearIfHalfFull() && t.used == 31, "below half the table is kept");
	while (t.used < 32)
	{
		t.Classify(k, true, false, 1, 1, 1);
		k += 0x1000;
	}
	Check(t.ClearIfHalfFull() && t.used == 0 && t.clears == 1, "the table empties once half its slots are used");
	Check(t.Classify(0x1000, true, false, 1, 1, 1) == PC_FIRST, "after a clear a known hull is first again");
}

// A lookup reads at most PROBE_LIMIT slots.
static void ProbeLimit()
{
	Table t;
	t.Init(g_large, 1024);
	uintptr_t keys[33];
	unsigned home = SlotOf(0x10000, 1023);
	int n = 0;
	for (uintptr_t k = 0x10000; n < 33; k += 8)
	{
		if (SlotOf(k, 1023) == home)
			keys[n++] = k;
	}
	for (int i = 0; i < 32; ++i)
		t.Classify(keys[i], true, false, 1, 1, 1);
	Check(t.used == 32, "32 colliding hulls fill the probe run");
	Check(t.Classify(keys[32], true, false, 1, 1, 1) == PC_FIRST && t.used == 32,
	      "a hull past the probe limit is first and not stored");
	Check(t.Classify(keys[32], true, false, 1, 1, 1) == PC_FIRST, "a hull past the probe limit stays first");
	Check(t.Classify(keys[0], true, false, 1, 1, 1) == PC_SAME, "a hull inside the probe run is found");
}

// Hull 0 is classified by its flags and never stored.
static void NullHull()
{
	Table t;
	t.Init(g_small, 64);
	Check(t.Classify(0, true, false, 1, 1, 1) == PC_FIRST && t.used == 0, "a null hull is classified but never stored");
	Check(t.Classify(0, false, false, 1, 1, 1) == PC_CREATE, "a null hull with no actor is a create");
}

int main()
{
	Precedence();
	Distances();
	ClearRule();
	ProbeLimit();
	NullHull();
	return CheckExit("hull_pose_units");
}
