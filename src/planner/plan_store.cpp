// plan_store.cpp - The plan slots: a file-static fixed array with one writer per field. The
// pre-arrival words have two writers: the thread updating the character issues and resolves them, and
// the main thread's slot clear and save-load reset zero them; a word that survives a clear carries the
// old epoch, and every reader drops it. The main thread writes a slot's plan while the slot's epoch is
// odd; the AI back thread copies it between two loads of the sequence word, advances its leg by a
// compare-exchange on that word, and sets the slot's words only while the epoch it read still holds.
// No lock, no allocation, no logging.

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
	volatile LONGLONG preWord;     // (epoch << 32) | (state << 16) | (from << 8) | to; 0 none
	volatile LONGLONG preQpc;      // the issue time, stored before the word
	volatile LONG     preTried;    // ((epoch & 0xFFFF) << 16) | (kind << 8) | leg; kind 1 skipped, 2 blocked
	float             preX, preZ;  // the issue position, stored before the word
	uintptr_t         cm;          // 0 free; written only under an odd epoch
	int               verdict, legCount, routeTruncated;
	float             finalDest[3];
	float             destAtPlan[3];                                    // written only under an odd epoch
	float             resend[PLAN_RESEND_POINTS][3]; int resendCount;   // written only under an odd epoch
	float             waterMult;                                        // written only under an odd epoch
	float             acidMult;                                         // written only under an odd epoch
	int               orderOutdoors, holdInteriorPortal;                // written only under an odd epoch
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
static PlanMoveDestReader s_moveDestReader = NULL;
static int                s_legAim = 0;   // written once at the arm, before any reader runs
static LONGLONG          s_preHoldTicks = 0;   // written once at the pre-arrival install

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

// Main thread: as BeginRewrite, returning the current leg from the exchange itself, so an AI-thread
// advance that landed before the exchange is kept.
static unsigned BeginRewriteKeepLeg(PlanSlot& s, unsigned* leg)
{
	unsigned e = EpochOf(s.legWord);
	*leg = LegOf(InterlockedExchange64(&s.legWord, MakeWord(e + 1u, 0u)));
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
	memset(s.destAtPlan, 0, sizeof(s.destAtPlan));
	memset(s.resend, 0, sizeof(s.resend));
	s.resendCount = 0;
	s.waterMult = 0.0f;
	s.acidMult = 0.0f;
	s.orderOutdoors = 0;
	s.holdInteriorPortal = 0;
	memset(s.legs, 0, sizeof(s.legs));
	memset(&s.main, 0, sizeof(s.main));
	InterlockedExchange(&s.loadedMask, 0);
	ZeroWords(s);
	InterlockedExchange64(&s.preWord, 0);
	InterlockedExchange64(&s.preQpc, 0);
	InterlockedExchange(&s.preTried, 0);
	s.preX = 0.0f;
	s.preZ = 0.0f;
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
	bool keep = w.keepSends && s.cm == w.cm;
	unsigned e = BeginRewrite(s);
	s.cm = w.cm;
	s.verdict = w.verdict;
	s.legCount = legCount;
	s.routeTruncated = w.routeTruncated;
	memcpy(s.finalDest, w.finalDest, sizeof(s.finalDest));
	s.waterMult = w.waterMult;
	s.acidMult = w.acidMult;
	s.orderOutdoors = w.orderOutdoors;
	s.holdInteriorPortal = w.holdInteriorPortal;
	if (!keep)
	{
		memcpy(s.destAtPlan, w.destAtPlan, sizeof(s.destAtPlan));
		memset(s.resend, 0, sizeof(s.resend));
		s.resendCount = 0;
		s.main.haveHold = 0;
		s.main.holdTime = 0.0;
		memset(s.main.holdDest, 0, sizeof(s.main.holdDest));
	}
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
		memcpy(out->destAtPlan, s.destAtPlan, sizeof(out->destAtPlan));
		memcpy(out->resend, s.resend, sizeof(out->resend));
		out->resendCount = s.resendCount;
		out->waterMult = s.waterMult;
		out->acidMult = s.acidMult;
		out->orderOutdoors = s.orderOutdoors;
		out->holdInteriorPortal = s.holdInteriorPortal;
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

static float DistXz(float ax, float az, float bx, float bz)
{
	float dx = ax - bx, dz = az - bz;
	return sqrtf(dx * dx + dz * dz);
}

static LONGLONG QpcTicks()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

bool PlannerOwnsWait(uintptr_t cm, float posX, float posZ, float wpX, float wpZ)
{
	if (PlanStoreMode() == PLANNER_OFF) return false;
	if (!s_moveDestReader) return false;
	int slot = PlanStoreFind(cm);
	if (slot < 0) return false;
	PlanView v;
	if (!PlanStoreRead(slot, &v) || v.cm != cm) return false;
	if (v.legIndex < 0 || v.legIndex >= v.legCount) return false;
	const PlanLeg& leg = v.legs[v.legIndex];
	const float pos[3] = { posX, 0.0f, posZ };
	const float wp[3] = { wpX, 0.0f, wpZ };
	float posToPortal = PlanDistToPortal(leg, pos, s_legAim);
	float wpToPortal = PlanDistToPortal(leg, wp, s_legAim);
	float posToWp = DistXz(posX, posZ, wpX, wpZ);
	float moveDest[3];
	s_moveDestReader(cm, moveDest);
	bool destIsPlans = PlanDestIsPlansXz(moveDest, v.finalDest, v.resend, v.resendCount);
	int preInFlight = 0;
	float posToPrePortal = 0.0f;
	PlanPreFlight f;
	if (s_preHoldTicks > 0 && PlanStorePreRead(slot, &f) && f.epoch == v.epoch && f.to == v.legIndex
	    && f.from >= 0 && f.from < v.legCount && QpcTicks() - f.issueQpc < s_preHoldTicks)
	{
		preInFlight = 1;
		posToPrePortal = PlanDistToPortal(v.legs[f.from], pos, s_legAim);
	}
	return PlanOwnsWait(PlanStoreMode(), v.verdict, leg.isDestination, v.waiting, posToPortal, wpToPortal,
	                    posToWp, destIsPlans, preInFlight, posToPrePortal);
}

void PlanStoreSetMoveDestReader(PlanMoveDestReader fn)
{
	s_moveDestReader = fn;
}

void PlanStoreSetLegAim(int on)
{
	s_legAim = on ? 1 : 0;
}

static LONGLONG MakePre(unsigned epoch, int state, int from, int to)
{
	return (LONGLONG)(((unsigned long long)epoch << 32) | ((unsigned long long)(state & 0xFF) << 16)
	                  | ((unsigned long long)(from & 0xFF) << 8) | (unsigned long long)(to & 0xFF));
}

static LONG MakeTried(unsigned epoch, int kind, int leg)
{
	return (LONG)(((epoch & 0xFFFFu) << 16) | ((unsigned)(kind & 0xFF) << 8) | ((unsigned)leg & 0xFFu));
}

void PlanStorePreIssue(int slot, unsigned epoch, int from, int to, float x, float z, LONGLONG qpc)
{
	if (!SlotInRange(slot) || from < 0 || to < 0 || from >= PLAN_MAX_LEGS || to >= PLAN_MAX_LEGS) return;
	PlanSlot& s = s_slots[slot];
	s.preX = x;
	s.preZ = z;
	InterlockedExchange64(&s.preQpc, qpc);
	InterlockedExchange64(&s.preWord, MakePre(epoch, PLAN_PRE_ISSUED, from, to));
}

bool PlanStorePreRead(int slot, PlanPreFlight* out)
{
	if (!SlotInRange(slot) || !out) return false;
	PlanSlot& s = s_slots[slot];
	unsigned long long w = (unsigned long long)InterlockedCompareExchange64(&s.preWord, 0, 0);
	int state = (int)((w >> 16) & 0xFF);
	if ((state & PLAN_PRE_STATE_MASK) == PLAN_PRE_NONE) return false;
	out->epoch = (unsigned)(w >> 32);
	out->state = state;
	out->from = (int)((w >> 8) & 0xFF);
	out->to = (int)(w & 0xFF);
	out->issueX = s.preX;
	out->issueZ = s.preZ;
	out->issueQpc = InterlockedCompareExchange64(&s.preQpc, 0, 0);
	return true;
}

void PlanStorePreUpdate(int slot, const PlanPreFlight& f, int state)
{
	if (!SlotInRange(slot)) return;
	LONGLONG seen = MakePre(f.epoch, f.state, f.from, f.to);
	LONGLONG next = (state & PLAN_PRE_STATE_MASK) == PLAN_PRE_NONE ? 0 : MakePre(f.epoch, state, f.from, f.to);
	InterlockedCompareExchange64(&s_slots[slot].preWord, next, seen);
}

bool PlanStoreLeg(int slot, unsigned* epoch, int* leg)
{
	if (!SlotInRange(slot) || !epoch || !leg) return false;
	LONGLONG w = LoadWord(s_slots[slot]);
	if (EpochOf(w) & 1u) return false;
	*epoch = EpochOf(w);
	*leg = (int)LegOf(w);
	return true;
}

int PlanStorePreNoteSkip(int slot, unsigned epoch, int leg, int blocks)
{
	if (!SlotInRange(slot)) return 0;
	PlanSlot& s = s_slots[slot];
	LONG cur = InterlockedCompareExchange(&s.preTried, 0, 0);
	bool same = (cur & 0xFFFF00FF) == (MakeTried(epoch, 0, leg) & 0xFFFF00FF);
	int had = same ? (int)((cur >> 8) & 0xFF) : 0;
	int kind = (blocks || had == 2) ? 2 : 1;
	if (kind != had)
		InterlockedExchange(&s.preTried, MakeTried(epoch, kind, leg));
	return had == 0 ? 1 : 0;
}

bool PlanStorePreBlocked(int slot, unsigned epoch, int leg)
{
	if (!SlotInRange(slot)) return false;
	return InterlockedCompareExchange(&s_slots[slot].preTried, 0, 0) == MakeTried(epoch, 2, leg);
}

void PlanStorePreSkip(int slot, unsigned epoch, int leg, int why)
{
	if (!PlanStorePreNoteSkip(slot, epoch, leg, why != PPS_WAIT ? 1 : 0))
		return;
	InterlockedIncrement(&s_counters.preSkip);
	if (why == PPS_WAIT) InterlockedIncrement(&s_counters.preSkipWait);
	else if (why == PPS_SNAP) InterlockedIncrement(&s_counters.preSkipSnap);
	else if (why == PPS_SAME) InterlockedIncrement(&s_counters.preSkipSame);
	else if (why == PPS_HELD) InterlockedIncrement(&s_counters.preSkipHeld);
}

int PlanStorePreSnap(int slot, unsigned epoch, int leg, int got)
{
	if (got == 1)
		return PLAN_PRE_SNAP_HIT;
	if (got == -1)
	{
		InterlockedIncrement(&s_counters.preBusy);
		return PLAN_PRE_SNAP_RETRY;
	}
	PlanStorePreSkip(slot, epoch, leg, PPS_SNAP);
	return PLAN_PRE_SNAP_BLOCK;
}

void PlanStoreSetPreHold(LONGLONG ticks)
{
	s_preHoldTicks = ticks > 0 ? ticks : 0;
}

// Main thread (the slots' one writer).
int PlannerNoteModSend(uintptr_t cm, const float sent[3], int kind, double now)
{
	if (PlanStoreMode() == PLANNER_OFF || !cm || !sent) return 0;
	int slot = SlotOfKey(cm);
	if (slot < 0) return 0;
	PlanSlot& s = s_slots[slot];
	if (kind == PLAN_SEND_HOLD)
	{
		memcpy(s.main.holdDest, sent, sizeof(s.main.holdDest));
		s.main.holdTime = now;
		s.main.haveHold = 1;
		InterlockedIncrement(&s_counters.heldPlanned);
		return 1;
	}
	if (kind != PLAN_SEND_RESEND) return 0;
	if (DistXz(sent[0], sent[2], s.finalDest[0], s.finalDest[2]) > PLAN_RESEND_REACH)
	{
		InterlockedIncrement(&s_counters.reissueRefused);
		return -1;
	}
	InterlockedIncrement(&s_counters.reissuedPlanned);
	if (PlanDestIsPlans(sent, s.finalDest, s.resend, s.resendCount)) return 1;
	unsigned leg = 0;
	unsigned e = BeginRewriteKeepLeg(s, &leg);
	if (s.resendCount >= PLAN_RESEND_POINTS)
	{
		memmove(s.resend[0], s.resend[1], sizeof(s.resend[0]) * (PLAN_RESEND_POINTS - 1));
		s.resendCount = PLAN_RESEND_POINTS - 1;
	}
	memcpy(s.resend[s.resendCount], sent, sizeof(s.resend[0]));
	++s.resendCount;
	EndRewrite(s, e, (int)leg);
	return 1;
}

// Any thread: the far-snap count is one interlocked increment; the maximum is raised by a
// compare-exchange loop that retries only when another thread raised it in between.
void PlannerNoteSnap(float distance)
{
	if (!(distance >= 0.0f)) return;   // negative or NaN
	if (distance > 1.0e6f) distance = 1.0e6f;
	if (distance > PLAN_REACH) InterlockedIncrement(&s_counters.snapFar);
	LONG d = (LONG)(distance + 0.5f);
	LONG cur = InterlockedCompareExchange(&s_counters.snapMax, 0, 0);
	while (d > cur)
	{
		LONG seen = InterlockedCompareExchange(&s_counters.snapMax, d, cur);
		if (seen == cur) break;
		cur = seen;
	}
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
