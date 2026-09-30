// plan_store.cpp - The plan slots: a file-static fixed array with one writer per field. The main
// thread writes a slot's plan while the slot's epoch is odd; the AI back thread copies it between
// two loads of the sequence word, advances its leg by a compare-exchange on that word, and sets
// the slot's words only while the epoch it read still holds. No lock, no allocation, no logging.

#include <math.h>
#include <string.h>
#include "planner/plan_store.h"

namespace planner {

namespace plan_store_detail {

struct PlanSlot
{
	volatile LONGLONG legWord;     // (epoch << 32) | legIndex; epoch odd while the main thread rewrites
	volatile LONG     loadedMask;  // main thread writes
	volatile LONG     waiting;     // AI thread writes: 1 at a held recheck, 0 on an advance
	volatile LONG     rungs;       // AI thread: rungs on the current leg, reset by the advance
	volatile LONG     arrival;     // AI thread sets on an advance; the tick clears it
	volatile LONG     consulted;   // AI thread: getZoneEdge calls with this plan
	uintptr_t         cm;          // 0 free; written only under an odd epoch
	int               verdict, legCount, routeTruncated;
	float             finalDest[3];
	PlanLeg           legs[PLAN_MAX_LEGS];
	PlanMainState     main;
};

typedef void (*PlanStorePause)(void* ctx);

} // namespace plan_store_detail
using namespace plan_store_detail;

static PlanSlot        s_slots[PLAN_SLOTS];
static volatile LONG   s_mode = PLANNER_OFF;
static PlannerCounters s_counters;
static PlanStorePause  s_pauseInRewrite = NULL;
static void*           s_pauseInRewriteCtx = NULL;
static PlanStorePause  s_pauseInRead = NULL;
static void*           s_pauseInReadCtx = NULL;

static LONGLONG MakeWord(unsigned epoch, unsigned leg)
{
	return (LONGLONG)(((unsigned long long)epoch << 32) | leg);
}

static unsigned EpochOf(LONGLONG word)
{
	return (unsigned)((unsigned long long)word >> 32);
}

static unsigned LegOf(LONGLONG word)
{
	return (unsigned)((unsigned long long)word & 0xFFFFFFFFull);
}

// An interlocked load: a full compiler and processor fence, so a slot's plain fields are copied
// strictly between a read's two loads of the word.
static LONGLONG LoadWord(PlanSlot& s)
{
	return InterlockedCompareExchange64(&s.legWord, 0, 0);
}

static bool SlotInRange(int slot)
{
	return slot >= 0 && slot < PLAN_SLOTS;
}

// Main thread: the slot's epoch goes odd; only this thread moves the epoch, so the value read is
// the even one the last rewrite published. Returns that even epoch.
static unsigned BeginRewrite(PlanSlot& s)
{
	unsigned e = EpochOf(s.legWord);
	InterlockedExchange64(&s.legWord, MakeWord(e + 1u, 0u));
	return e;
}

// Main thread: publishes the rewritten fields under the next even epoch, with the current leg.
static void EndRewrite(PlanSlot& s, unsigned e, int leg)
{
	if (s_pauseInRewrite) s_pauseInRewrite(s_pauseInRewriteCtx);
	InterlockedExchange64(&s.legWord, MakeWord(e + 2u, (unsigned)leg));
}

static void ZeroWords(PlanSlot& s)
{
	InterlockedExchange(&s.waiting, 0);
	InterlockedExchange(&s.rungs, 0);
	InterlockedExchange(&s.arrival, 0);
	InterlockedExchange(&s.consulted, 0);
}

// Main thread: frees a slot through the epoch, so an advance or a word update made against the
// plan it held is refused.
static void ClearSlot(PlanSlot& s)
{
	unsigned e = BeginRewrite(s);
	s.cm = 0;
	s.verdict = PV_NONE;
	s.legCount = 0;
	s.routeTruncated = 0;
	memset(s.finalDest, 0, sizeof(s.finalDest));
	memset(s.legs, 0, sizeof(s.legs));
	memset(&s.main, 0, sizeof(s.main));
	InterlockedExchange(&s.loadedMask, 0);
	ZeroWords(s);
	EndRewrite(s, e, 0);
}

// Main thread (the keys' one writer): the character's slot, or -1.
static int SlotOfKey(uintptr_t cm)
{
	for (int i = 0; i < PLAN_SLOTS; ++i)
		if (s_slots[i].cm == cm) return i;
	return -1;
}

// Any thread: whether the slot still carries `epoch` after a word update, which the caller
// reverts when a rewrite overtook it.
static bool EpochHolds(PlanSlot& s, unsigned epoch)
{
	return EpochOf(LoadWord(s)) == epoch;
}

void PlanStoreArm(int mode)
{
	InterlockedExchange(&s_mode, (LONG)mode);
}

int PlanStoreMode()
{
	return (int)InterlockedCompareExchange(&s_mode, 0, 0);
}

int PlanStoreWrite(const PlanWrite& w)
{
	if (!w.cm) return -1;
	int slot = SlotOfKey(w.cm);
	if (slot < 0) slot = SlotOfKey(0);
	if (slot < 0)
	{
		InterlockedIncrement(&s_counters.slotFull);
		return -1;
	}
	int legCount = w.legCount < 0 ? 0 : (w.legCount > PLAN_MAX_LEGS ? PLAN_MAX_LEGS : w.legCount);
	int firstLeg = (w.firstLeg >= 0 && w.firstLeg < legCount) ? w.firstLeg : 0;

	PlanSlot& s = s_slots[slot];
	unsigned e = BeginRewrite(s);
	s.cm = w.cm;
	s.verdict = w.verdict;
	s.legCount = legCount;
	s.routeTruncated = w.routeTruncated;
	memcpy(s.finalDest, w.finalDest, sizeof(s.finalDest));
	memset(s.legs, 0, sizeof(s.legs));
	memcpy(s.legs, w.legs, sizeof(PlanLeg) * legCount);
	InterlockedExchange(&s.loadedMask, (LONG)w.loadedMask);
	ZeroWords(s);
	s.main.planTime = w.now;
	s.main.waitSince = 0.0;
	s.main.completeSince = 0.0;
	s.main.goalByFootprint = w.goalByFootprint;
	s.main.routeTruncated = w.routeTruncated;
	s.main.consultedAtPlan = 0;
	s.main.loadedGenAtPlan = 0;
	EndRewrite(s, e, firstLeg);
	return slot;
}

int PlanStoreDrop(uintptr_t cm)
{
	if (!cm) return -1;
	int slot = SlotOfKey(cm);
	if (slot < 0) return -1;
	PlanSlot& s = s_slots[slot];
	int consulted = (int)InterlockedCompareExchange(&s.consulted, 0, 0);
	ClearSlot(s);
	return consulted;
}

void PlanStoreReset()
{
	for (int i = 0; i < PLAN_SLOTS; ++i)
		if (s_slots[i].cm) ClearSlot(s_slots[i]);
}

void PlanStoreSetLoaded(int slot, unsigned mask)
{
	if (!SlotInRange(slot)) return;
	InterlockedExchange(&s_slots[slot].loadedMask, (LONG)mask);
}

PlanMainState* PlanStoreMain(int slot)
{
	return SlotInRange(slot) ? &s_slots[slot].main : NULL;
}

uintptr_t PlanStoreKey(int slot)
{
	return SlotInRange(slot) ? s_slots[slot].cm : 0;
}

// Any thread: the keys are scanned with plain aligned loads; a match counts only when the key
// reads the same between two loads of an even word.
int PlanStoreFind(uintptr_t cm)
{
	if (PlanStoreMode() == PLANNER_OFF || !cm) return -1;
	for (int i = 0; i < PLAN_SLOTS; ++i)
	{
		PlanSlot& s = s_slots[i];
		if (*(volatile uintptr_t*)&s.cm != cm) continue;
		LONGLONG w1 = LoadWord(s);
		if (EpochOf(w1) & 1u) continue;
		uintptr_t key = *(volatile uintptr_t*)&s.cm;
		LONGLONG w2 = LoadWord(s);
		if (key == cm && EpochOf(w2) == EpochOf(w1)) return i;
	}
	return -1;
}

// Any thread: an odd word refuses at once; a word that moved during the copy gets one more try.
bool PlanStoreRead(int slot, PlanView* out)
{
	if (!SlotInRange(slot) || !out) return false;
	PlanSlot& s = s_slots[slot];
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONGLONG w1 = LoadWord(s);
		unsigned e = EpochOf(w1);
		if (e & 1u) return false;
		if (s_pauseInRead) s_pauseInRead(s_pauseInReadCtx);
		out->cm = s.cm;
		out->verdict = s.verdict;
		out->legCount = s.legCount;
		out->routeTruncated = s.routeTruncated;
		out->loadedMask = (unsigned)s.loadedMask;
		out->waiting = (int)s.waiting;
		out->rungs = (int)s.rungs;
		memcpy(out->finalDest, s.finalDest, sizeof(out->finalDest));
		memcpy(out->legs, s.legs, sizeof(out->legs));
		LONGLONG w2 = LoadWord(s);
		if (EpochOf(w2) != e) continue;
		if (!out->cm) return false;
		out->slot = slot;
		out->epoch = e;
		out->legIndex = (int)LegOf(w2);
		return true;
	}
	return false;
}

// Any thread: succeeds only when neither the epoch nor the leg moved since the caller's read.
bool PlanStoreAdvance(int slot, unsigned epoch, int fromLeg, int toLeg)
{
	if (!SlotInRange(slot) || (epoch & 1u) || fromLeg < 0 || toLeg < 0 || toLeg >= PLAN_MAX_LEGS)
	{
		InterlockedIncrement(&s_counters.staleAdvance);
		return false;
	}
	PlanSlot& s = s_slots[slot];
	LONGLONG expect = MakeWord(epoch, (unsigned)fromLeg);
	if (InterlockedCompareExchange64(&s.legWord, MakeWord(epoch, (unsigned)toLeg), expect) != expect)
	{
		InterlockedIncrement(&s_counters.staleAdvance);
		return false;
	}
	InterlockedExchange(&s.rungs, 0);
	return true;
}

// Any thread: a rewrite between the check and the write is caught by the second check, and the
// value is taken back so the rewritten plan keeps its zero.
void PlanStoreSetWaiting(int slot, unsigned epoch, int waiting)
{
	if (!SlotInRange(slot) || (epoch & 1u)) return;
	PlanSlot& s = s_slots[slot];
	if (!EpochHolds(s, epoch)) return;
	LONG v = waiting ? 1 : 0;
	InterlockedExchange(&s.waiting, v);
	if (v && !EpochHolds(s, epoch)) InterlockedCompareExchange(&s.waiting, 0, v);
}

void PlanStoreAddRung(int slot, unsigned epoch)
{
	if (!SlotInRange(slot) || (epoch & 1u)) return;
	PlanSlot& s = s_slots[slot];
	if (!EpochHolds(s, epoch)) return;
	InterlockedIncrement(&s.rungs);
}

void PlanStoreNoteArrival(int slot, unsigned epoch)
{
	if (!SlotInRange(slot) || (epoch & 1u)) return;
	PlanSlot& s = s_slots[slot];
	if (!EpochHolds(s, epoch)) return;
	InterlockedExchange(&s.arrival, 1);
	if (!EpochHolds(s, epoch)) InterlockedCompareExchange(&s.arrival, 0, 1);
}

int PlanStoreTakeArrival(int slot)
{
	if (!SlotInRange(slot)) return 0;
	return (int)InterlockedExchange(&s_slots[slot].arrival, 0);
}

void PlanStoreNoteConsulted(int slot)
{
	if (!SlotInRange(slot)) return;
	InterlockedIncrement(&s_slots[slot].consulted);
}

bool PlannerOwnsWait(uintptr_t cm, float posX, float posZ)
{
	if (PlanStoreMode() == PLANNER_OFF) return false;
	int slot = PlanStoreFind(cm);
	if (slot < 0) return false;
	PlanView v;
	if (!PlanStoreRead(slot, &v) || v.cm != cm) return false;
	if (v.legIndex < 0 || v.legIndex >= v.legCount) return false;
	const PlanLeg& leg = v.legs[v.legIndex];
	float dx = posX - leg.point[0];
	float dz = posZ - leg.point[2];
	return PlanOwnsWait(PlanStoreMode(), v.verdict, leg.isDestination, v.waiting, sqrtf(dx * dx + dz * dz));
}

void PlanStoreTestPauseInRewrite(void (*fn)(void* ctx), void* ctx)
{
	s_pauseInRewrite = fn;
	s_pauseInRewriteCtx = ctx;
}

void PlanStoreTestPauseInRead(void (*fn)(void* ctx), void* ctx)
{
	s_pauseInRead = fn;
	s_pauseInReadCtx = ctx;
}

PlannerCounters* PlannerCountersGet()
{
	return &s_counters;
}

} // namespace planner
