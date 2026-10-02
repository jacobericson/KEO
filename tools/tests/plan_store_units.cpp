// The plan store's slots, epochs and ownership query, single-threaded: the test hooks run a read
// inside a rewrite and a rewrite inside a read, so each interleaving the lock-free protocol must
// refuse is driven deterministically. The concurrent run is plan_store_injection.

#include <cmath>
#include <cstdio>
#include <cstring>
#include "planner/plan_store.h"

#include "check.h"

using namespace planner;

static const uintptr_t CM_A = 0x10000;
static const uintptr_t CM_B = 0x20000;

// A route east along z = 0: portals at x = 1000 and 2000, then the destination at x = 3000.
static void MakeWrite(PlanWrite* w, uintptr_t cm, float stamp)
{
	memset(w, 0, sizeof(*w));
	w->cm = cm;
	w->verdict = PV_LEGGED;
	w->legCount = 3;
	w->firstLeg = 0;
	w->loadedMask = 0x7u;
	w->finalDest[0] = 3000.0f;
	w->finalDest[1] = stamp;
	w->now = 10.0;
	for (int i = 0; i < 3; ++i)
	{
		PlanLeg& leg = w->legs[i];
		leg.point[0] = 1000.0f * (float)(i + 1);
		leg.point[1] = stamp;
		leg.point[2] = 0.0f;
		memcpy(leg.edgeA, leg.point, sizeof(leg.point));
		memcpy(leg.edgeB, leg.point, sizeof(leg.point));
		leg.farSection = 20 + i;
		leg.isDestination = (i == 2) ? 1 : 0;
	}
}

static void Fresh(int mode)
{
	PlanStoreTestPauseInRewrite(NULL, NULL);
	PlanStoreTestPauseInRead(NULL, NULL);
	PlanStoreReset();
	PlanStoreArm(mode);
}

// ---- The rewrite and read hooks ---------------------------------------------------------------

struct ReadInRewrite
{
	int  slot;
	int  calls;
	bool readAnswered;
};

static void ReadDuringRewrite(void* ctx)
{
	ReadInRewrite* r = (ReadInRewrite*)ctx;
	PlanView v;
	r->calls++;
	r->readAnswered = PlanStoreRead(r->slot, &v);
}

struct RewriteInRead
{
	uintptr_t cm;
	int       calls;
	float     stamp;
};

static void RewriteDuringRead(void* ctx)
{
	RewriteInRead* r = (RewriteInRead*)ctx;
	PlanWrite w;
	r->calls++;
	r->stamp += 1.0f;
	MakeWrite(&w, r->cm, r->stamp);
	PlanStoreWrite(w);
}

// ---- Store rows -------------------------------------------------------------------------------

static void StoreRows()
{
	PlanWrite w;
	PlanView v;

	Fresh(PLANNER_OFF);
	MakeWrite(&w, CM_A, 1.0f);
	int slot = PlanStoreWrite(w);
	Check(slot >= 0 && PlanStoreFind(CM_A) == -1, "store: an unarmed store finds nothing");
	PlanStoreArm(PLANNER_ON);
	Check(PlanStoreFind(CM_A) == slot, "store: an armed store finds the plan written while unarmed");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	Check(slot >= 0 && PlanStoreFind(CM_A) == slot && PlanStoreKey(slot) == CM_A,
	      "store: a write is found by its key");
	Check(PlanStoreFind(CM_B) == -1, "store: an unplanned character is not found");
	Check(PlanStoreFind(0) == -1, "store: a null key is not found");
	Check(PlanStoreRead(slot, &v) && v.cm == CM_A && v.slot == slot && v.legIndex == 0 && v.legCount == 3
	      && v.verdict == PV_LEGGED && v.loadedMask == 0x7u && (v.epoch & 1u) == 0
	      && v.legs[1].point[0] == 2000.0f && v.finalDest[0] == 3000.0f,
	      "store: a read copies the plan with an even epoch");
	unsigned firstEpoch = v.epoch;

	MakeWrite(&w, CM_A, 2.0f);
	w.legCount = 2;
	w.firstLeg = 1;
	w.legs[1].isDestination = 1;
	int again = PlanStoreWrite(w);
	Check(again == slot && PlanStoreRead(slot, &v) && v.cm == CM_A && v.legCount == 2 && v.legIndex == 1
	      && v.legs[0].point[1] == 2.0f && v.legs[2].point[0] == 0.0f && v.epoch == firstEpoch + 2u,
	      "store: a rewrite replaces the character's plan in place");

	// The movement destination held at plan time is written with every plan, a re-plan included.
	MakeWrite(&w, CM_A, 3.0f);
	w.destAtPlan[0] = 1500.0f;
	w.destAtPlan[2] = -250.0f;
	w.keepSends = 1;
	Check(PlanStoreWrite(w) == slot && PlanStoreRead(slot, &v) && v.destAtPlan[0] == 1500.0f
	      && v.destAtPlan[1] == 0.0f && v.destAtPlan[2] == -250.0f,
	      "store: the destination held at plan time is in the view, a re-plan included");

	// The water multiplier the plan was searched at is in the view, and a rewrite replaces it.
	MakeWrite(&w, CM_A, 4.0f);
	w.waterMult = 10.13f;
	bool first = PlanStoreWrite(w) == slot && PlanStoreRead(slot, &v) && v.waterMult == 10.13f;
	w.waterMult = 5.0f;
	Check(first && PlanStoreWrite(w) == slot && PlanStoreRead(slot, &v) && v.waterMult == 5.0f,
	      "store: a plan's water multiplier reads back");

	// Every slot taken by a distinct character: the next is refused and counted, and a rewrite of a
	// planned character still lands in its own slot.
	Fresh(PLANNER_ON);
	bool allTaken = true;
	for (int i = 0; i < PLAN_SLOTS; ++i)
	{
		MakeWrite(&w, 0x100000 + (uintptr_t)i * 0x1000, 1.0f);
		if (PlanStoreWrite(w) != i) allTaken = false;
	}
	LONG fullBefore = PlannerCountersGet()->slotFull;
	MakeWrite(&w, 0x900000, 1.0f);
	int refused = PlanStoreWrite(w);
	Check(allTaken && refused == -1 && PlannerCountersGet()->slotFull == fullBefore + 1
	      && PlanStoreFind(0x900000) == -1,
	      "store: the 65th character is refused and counted");
	MakeWrite(&w, 0x100000 + 5 * 0x1000, 3.0f);
	Check(PlanStoreWrite(w) == 5 && PlannerCountersGet()->slotFull == fullBefore + 1,
	      "store: a full store still rewrites a planned character");
	Check(PlanStoreDrop(0x100000 + 7 * 0x1000) == 0, "store: a drop frees its slot");
	MakeWrite(&w, 0x900000, 1.0f);
	Check(PlanStoreWrite(w) == 7, "store: a freed slot takes the next character");

	// A read from inside a rewrite (the epoch odd) is refused.
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	ReadInRewrite rr = { slot, 0, true };
	PlanStoreTestPauseInRewrite(ReadDuringRewrite, &rr);
	MakeWrite(&w, CM_A, 2.0f);
	PlanStoreWrite(w);
	PlanStoreTestPauseInRewrite(NULL, NULL);
	Check(rr.calls == 1 && !rr.readAnswered, "store: a read during a rewrite is refused");
	Check(PlanStoreRead(slot, &v) && v.legs[0].point[1] == 2.0f, "store: the read after the rewrite answers");

	// A read whose copy a whole rewrite overtook (the callback rewrites on each attempt) is refused.
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	RewriteInRead ri = { CM_A, 0, 1.0f };
	PlanStoreTestPauseInRead(RewriteDuringRead, &ri);
	bool overtaken = PlanStoreRead(slot, &v);
	PlanStoreTestPauseInRead(NULL, NULL);
	Check(!overtaken && ri.calls == 2, "store: a read a rewrite overtook is refused");
	Check(PlanStoreRead(slot, &v) && v.legs[0].point[1] == ri.stamp && v.finalDest[1] == ri.stamp,
	      "store: the read after the overtaking rewrite answers the last plan");

	// The rewrite writes the same firstLeg (0) as the stale view's legIndex, so the leg alone cannot
	// refuse the advance: only the epoch can.
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanView stale = v;
	MakeWrite(&w, CM_A, 2.0f);
	PlanStoreWrite(w);
	LONG staleBefore = PlannerCountersGet()->staleAdvance;
	bool staleMoved = PlanStoreAdvance(slot, stale.epoch, stale.legIndex, 1);
	Check(stale.legIndex == 0 && !staleMoved && PlanStoreRead(slot, &v) && v.legIndex == 0
	      && PlannerCountersGet()->staleAdvance == staleBefore + 1,
	      "store: a stale advance after a rewrite is refused");

	PlanStoreRead(slot, &v);
	staleBefore = PlannerCountersGet()->staleAdvance;
	bool wrongMoved = PlanStoreAdvance(slot, v.epoch, 1, 2);
	Check(!wrongMoved && PlanStoreRead(slot, &v) && v.legIndex == 0
	      && PlannerCountersGet()->staleAdvance == staleBefore + 1,
	      "store: an advance from the wrong leg is refused");

	PlanStoreAddRung(slot, v.epoch);
	PlanStoreAddRung(slot, v.epoch);
	Check(PlanStoreRead(slot, &v) && v.rungs == 2, "store: rungs count on the current leg");
	Check(PlanStoreAdvance(slot, v.epoch, 0, 1) && PlanStoreRead(slot, &v) && v.legIndex == 1 && v.rungs == 0,
	      "store: an advance moves the leg and resets its rungs");
	Check(!PlanStoreAdvance(slot, v.epoch + 1u, 1, 2), "store: an odd epoch never advances");

	// The words follow the epoch: a setter holding an old epoch changes nothing.
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 1);
	Check(PlanStoreRead(slot, &v) && v.waiting == 1, "store: a waiting word under the current epoch is set");
	unsigned oldEpoch = v.epoch;
	MakeWrite(&w, CM_A, 2.0f);
	PlanStoreWrite(w);
	Check(PlanStoreRead(slot, &v) && v.waiting == 0, "store: a rewrite clears the waiting word");
	PlanStoreSetWaiting(slot, oldEpoch, 1);
	PlanStoreAddRung(slot, oldEpoch);
	PlanStoreNoteArrival(slot, oldEpoch);
	Check(PlanStoreRead(slot, &v) && v.waiting == 0 && v.rungs == 0 && PlanStoreTakeArrival(slot) == 0,
	      "store: a waiting word from an old epoch is ignored");
	PlanStoreNoteArrival(slot, v.epoch);
	Check(PlanStoreTakeArrival(slot) == 1 && PlanStoreTakeArrival(slot) == 0,
	      "store: the arrival word is taken once");

	PlanStoreNoteConsulted(slot);
	PlanStoreNoteConsulted(slot);
	Check(PlanStoreDrop(CM_A) == 2 && PlanStoreFind(CM_A) == -1 && PlanStoreKey(slot) == 0
	      && !PlanStoreRead(slot, &v),
	      "store: a drop answers the plan's consultations and frees the slot");
	Check(PlanStoreDrop(CM_A) == -1, "store: a drop without a plan answers -1");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	w.routeTruncated = 1;
	w.goalByFootprint = 1;
	slot = PlanStoreWrite(w);
	PlanMainState* ms = PlanStoreMain(slot);
	Check(PlanStoreRead(slot, &v) && v.routeTruncated == 1 && ms && ms->routeTruncated == 1
	      && ms->goalByFootprint == 1 && ms->planTime == 10.0,
	      "store: a truncated plan's flag is read back");
	PlanStoreSetLoaded(slot, 0x1u);
	Check(PlanStoreRead(slot, &v) && v.loadedMask == 0x1u, "store: the loaded mask is set by the main thread");
	Check(PlanStoreMain(-1) == NULL && PlanStoreMain(PLAN_SLOTS) == NULL && !PlanStoreRead(PLAN_SLOTS, &v),
	      "store: a slot out of range answers nothing");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	int sa = PlanStoreWrite(w);
	MakeWrite(&w, CM_B, 1.0f);
	int sb = PlanStoreWrite(w);
	PlanStoreReset();
	Check(PlanStoreFind(CM_A) == -1 && PlanStoreFind(CM_B) == -1 && PlanStoreKey(sa) == 0
	      && PlanStoreKey(sb) == 0 && !PlanStoreRead(sa, &v) && !PlanStoreRead(sb, &v),
	      "store: reset frees every slot");
}

// ---- Ownership rows ---------------------------------------------------------------------------

// Writes CM_A's plan with leg 0 current and its waiting word set, under `mode`.
static void HeldAtPortal(int mode)
{
	PlanWrite w;
	PlanView v;
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	int slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 1);
	PlanStoreArm(mode);
}

static void OwnsRows()
{
	HeldAtPortal(PLANNER_OFF);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: an unarmed store owns nothing");

	HeldAtPortal(PLANNER_OBSERVE);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: observe never owns a wait");

	HeldAtPortal(PLANNER_ON);
	Check(PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: on owns a held portal wait within reach");
	Check(!PlannerOwnsWait(CM_A, 1025.0f, 0.0f, 9000.0f, 9000.0f), "owns: a wait beyond reach of the portal is not owned");
	Check(!PlannerOwnsWait(CM_B, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: an unplanned character's wait is not owned");

	PlanView v;
	int slot = PlanStoreFind(CM_A);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 0);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: a portal without the waiting word set is not owned");
	Check(PlannerOwnsWait(CM_A, 1003.0f, 2.0f, 1005.0f, 0.0f), "owns: on owns a stop at the planner's waypoint at the portal");
	Check(!PlannerOwnsWait(CM_A, 1503.0f, 2.0f, 1505.0f, 0.0f), "owns: a waypoint away from the portal is not owned");

	// The current leg is the destination (the plan written starting on leg 2), waiting set.
	PlanWrite w;
	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	w.firstLeg = 2;
	slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 1);
	Check(v.legIndex == 2 && !PlannerOwnsWait(CM_A, 3000.0f, 0.0f, 9000.0f, 9000.0f), "owns: a destination leg is never owned");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	w.verdict = PV_DIRECT;
	slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 1);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: a direct plan's wait is not owned");
	Fresh(PLANNER_OFF);
}

// ---- Re-send rows -----------------------------------------------------------------------------

static void Set3(float out[3], float x, float y, float z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

// The counters are global, so each row reads a delta.
static void SendRows()
{
	PlanWrite w;
	PlanView v;
	float p[3];
	PlannerCounters* c = PlannerCountersGet();

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	int slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	PlanStoreAdvance(slot, v.epoch, 0, 1);
	Set3(p, 3008.0f, 1.0f, 0.0f);
	int sent = PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 20.0);
	Check(sent == 1 && PlanStoreRead(slot, &v) && v.resendCount == 1 && v.resend[0][0] == 3008.0f && v.legIndex == 1,
	      "store: a recorded re-send is in the view and keeps the leg");

	LONG before = c->reissuedPlanned;
	int again = PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 21.0);
	Set3(p, 3000.5f, 1.0f, 0.0f);
	int accepted = PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 22.0);
	Check(again == 1 && accepted == 1 && PlanStoreRead(slot, &v) && v.resendCount == 1
	      && c->reissuedPlanned == before + 2,
	      "store: a re-send the plan already accepts is not recorded twice");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	Set3(p, 3008.0f, 1.0f, 0.0f);
	PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 20.0);
	Set3(p, 2992.0f, 1.0f, 0.0f);
	PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 22.0);
	Set3(p, 3000.0f, 1.0f, 8.0f);
	PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 24.0);
	Check(PlanStoreRead(slot, &v) && v.resendCount == 2 && v.resend[0][0] == 2992.0f && v.resend[0][2] == 0.0f
	      && v.resend[1][0] == 3000.0f && v.resend[1][2] == 8.0f,
	      "store: a third distinct re-send keeps the two newest");

	LONG refusedBefore = c->reissueRefused;
	Set3(p, 3030.0f, 1.0f, 0.0f);
	int refused = PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 26.0);
	Check(refused == -1 && c->reissueRefused == refusedBefore + 1 && PlanStoreRead(slot, &v) && v.resendCount == 2
	      && v.resend[0][0] == 2992.0f && v.resend[1][2] == 8.0f,
	      "store: a re-send beyond the reach is refused and counted");

	MakeWrite(&w, CM_A, 1.0f);
	w.keepSends = 1;
	PlanStoreWrite(w);
	Check(PlanStoreRead(slot, &v) && v.resendCount == 2 && v.resend[0][0] == 2992.0f && v.resend[1][2] == 8.0f,
	      "store: a re-plan keeps the recorded re-sends");

	MakeWrite(&w, CM_A, 1.0f);
	w.keepSends = 0;
	PlanStoreWrite(w);
	Check(PlanStoreRead(slot, &v) && v.resendCount == 0, "store: a new order clears the recorded re-sends");

	Set3(p, 3008.0f, 1.0f, 0.0f);
	PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 30.0);
	bool recorded = PlanStoreRead(slot, &v) && v.resendCount == 1;
	PlanStoreDrop(CM_A);
	MakeWrite(&w, CM_A, 1.0f);
	w.keepSends = 1;
	slot = PlanStoreWrite(w);
	Check(recorded && PlanStoreRead(slot, &v) && v.resendCount == 0, "store: a drop clears the recorded re-sends");

	Set3(p, 3008.0f, 1.0f, 0.0f);
	int unplanned = PlannerNoteModSend(CM_B, p, PLAN_SEND_RESEND, 32.0);
	PlanStoreArm(PLANNER_OFF);
	int unarmed = PlannerNoteModSend(CM_A, p, PLAN_SEND_RESEND, 32.0);
	PlanStoreArm(PLANNER_ON);
	Check(unplanned == 0 && unarmed == 0 && PlanStoreRead(slot, &v) && v.resendCount == 0,
	      "store: an unplanned or unarmed character records nothing");

	LONG heldBefore = c->heldPlanned;
	Set3(p, 500.0f, 1.0f, 40.0f);
	int held = PlannerNoteModSend(CM_A, p, PLAN_SEND_HOLD, 34.5);
	PlanMainState* ms = PlanStoreMain(slot);
	Check(held == 1 && ms && ms->haveHold == 1 && ms->holdTime == 34.5 && ms->holdDest[0] == 500.0f
	      && ms->holdDest[2] == 40.0f && PlanStoreRead(slot, &v) && v.resendCount == 0
	      && c->heldPlanned == heldBefore + 1,
	      "store: a hold is recorded on the main side only");

	LONG farBefore = c->snapFar;
	PlannerNoteSnap(5.0f);
	PlannerNoteSnap(25.0f);
	PlannerNoteSnap(12.0f);
	Check(c->snapFar == farBefore + 1 && c->snapMax == 25, "store: a snap beyond reach is counted and raises the maximum");
	Fresh(PLANNER_OFF);
}

int main()
{
	StoreRows();
	OwnsRows();
	SendRows();
	return CheckExit("plan_store_units");
}
