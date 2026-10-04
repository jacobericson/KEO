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

	// The movement destination held at plan time is written with an order's plan (a re-plan that keeps
	// the sends keeps it: SendRows).
	MakeWrite(&w, CM_A, 3.0f);
	w.destAtPlan[0] = 1500.0f;
	w.destAtPlan[2] = -250.0f;
	Check(PlanStoreWrite(w) == slot && PlanStoreRead(slot, &v) && v.destAtPlan[0] == 1500.0f
	      && v.destAtPlan[1] == 0.0f && v.destAtPlan[2] == -250.0f,
	      "store: the destination held at plan time is in the view");

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

// The movement destination the fake reader returns: the plan's own (x 3000) unless a row moves it.
static float s_fakeDest[3];

static void FakeMoveDest(uintptr_t cm, float out[3])
{
	(void)cm;
	out[0] = s_fakeDest[0];
	out[1] = 0.0f;
	out[2] = s_fakeDest[2];
}

static void OwnsRows()
{
	s_fakeDest[0] = 3000.0f;
	s_fakeDest[1] = 0.0f;
	s_fakeDest[2] = 0.0f;
	HeldAtPortal(PLANNER_ON);
	PlanStoreSetMoveDestReader(NULL);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: no movement destination reader owns nothing");
	PlanStoreSetMoveDestReader(FakeMoveDest);

	HeldAtPortal(PLANNER_OFF);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: an unarmed store owns nothing");

	HeldAtPortal(PLANNER_OBSERVE);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: observe never owns a wait");

	HeldAtPortal(PLANNER_ON);
	Check(PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: on owns a held portal wait within reach");
	s_fakeDest[0] = 1005.0f;
	s_fakeDest[2] = 5.0f;
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: a member halted at its own position is not owned");
	s_fakeDest[0] = 3000.0f;
	s_fakeDest[2] = 0.0f;
	Check(!PlannerOwnsWait(CM_A, 1025.0f, 0.0f, 9000.0f, 9000.0f), "owns: a wait beyond reach of the portal is not owned");
	Check(!PlannerOwnsWait(CM_B, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: an unplanned character's wait is not owned");

	// A member sent to the gather point and held there, its movement destination that point.
	HeldAtPortal(PLANNER_ON);
	float gatherPoint[3] = { 1010.0f, 0.0f, 20.0f };
	int heldSent = PlannerNoteModSend(CM_A, gatherPoint, PLAN_SEND_HOLD, 10.0);
	s_fakeDest[0] = gatherPoint[0];
	s_fakeDest[2] = gatherPoint[2];
	Check(heldSent == 1 && !PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f),
	      "owns: a gather-held member at a portal is not owned");
	s_fakeDest[0] = 3000.0f;
	s_fakeDest[2] = 0.0f;
	HeldAtPortal(PLANNER_ON);

	PlanView v;
	int slot = PlanStoreFind(CM_A);
	PlanStoreRead(slot, &v);
	PlanStoreSetWaiting(slot, v.epoch, 0);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 9000.0f, 9000.0f), "owns: a portal without the waiting word set is not owned");
	Check(PlannerOwnsWait(CM_A, 1003.0f, 2.0f, 1005.0f, 0.0f), "owns: on owns a stop at the planner's waypoint at the portal");
	s_fakeDest[0] = 1000.0f;
	Check(!PlannerOwnsWait(CM_A, 1003.0f, 2.0f, 1005.0f, 0.0f), "owns: a halt at the portal is not owned");
	s_fakeDest[0] = 3000.0f;
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
	PlanStoreSetMoveDestReader(NULL);
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

	// The movement destination held when the order's plan was written, then a re-plan while the
	// engine holds a gather point, then a new order.
	MakeWrite(&w, CM_A, 1.0f);
	Set3(w.destAtPlan, 2500.0f, 0.0f, 0.0f);
	PlanStoreWrite(w);
	MakeWrite(&w, CM_A, 1.0f);
	w.keepSends = 1;
	Set3(w.destAtPlan, 500.0f, 0.0f, 40.0f);
	PlanStoreWrite(w);
	bool snapKept = PlanStoreRead(slot, &v) && v.destAtPlan[0] == 2500.0f && v.destAtPlan[2] == 0.0f;
	MakeWrite(&w, CM_A, 1.0f);
	Set3(w.destAtPlan, 500.0f, 0.0f, 40.0f);
	PlanStoreWrite(w);
	Check(snapKept && PlanStoreRead(slot, &v) && v.destAtPlan[0] == 500.0f && v.destAtPlan[2] == 40.0f,
	      "store: a re-plan keeps the order's movement destination snapshot and a new order takes its own");

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

// The order's outdoors bit and the plan's hold flag ride the slot as written, and a rewrite replaces them.
static void HoldRows()
{
	Fresh(PLANNER_ON);
	PlanWrite w;
	MakeWrite(&w, CM_A, 1.0f);
	w.orderOutdoors = 1;
	w.holdInteriorPortal = 1;
	int slot = PlanStoreWrite(w);
	PlanView v;
	bool held = slot >= 0 && PlanStoreRead(slot, &v) && v.orderOutdoors == 1 && v.holdInteriorPortal == 1;
	w.holdInteriorPortal = 0;
	PlanStoreWrite(w);
	bool cleared = PlanStoreRead(slot, &v) && v.orderOutdoors == 1 && v.holdInteriorPortal == 0;
	PlanStoreDrop(CM_A);
	Check(held && cleared, "store: a plan's outdoors bit and hold flag read back as written");
	Fresh(PLANNER_OFF);
}

// The in-flight words: published, updated against the word read, left by a rewrite whose epoch then
// drops them, cleared by the reset; the backward advance; the owned-wait clause, its age cap and its
// epoch and leg tests, and a word issued after the slot's clear; the tried word.
static void PreFlightRows()
{
	s_fakeDest[0] = 3000.0f;
	s_fakeDest[1] = 0.0f;
	s_fakeDest[2] = 0.0f;
	PlanStoreSetMoveDestReader(FakeMoveDest);
	Fresh(PLANNER_ON);
	PlanWrite w;
	MakeWrite(&w, CM_A, 1.0f);
	int slot = PlanStoreWrite(w);
	PlanView v;
	PlanStoreRead(slot, &v);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	PlanStorePreIssue(slot, v.epoch, 0, 1, 990.0f, 4.0f, now.QuadPart);
	PlanPreFlight f;
	bool read = PlanStorePreRead(slot, &f) && f.epoch == v.epoch && f.state == PLAN_PRE_ISSUED && f.from == 0
	            && f.to == 1 && f.issueX == 990.0f && f.issueZ == 4.0f && f.issueQpc == now.QuadPart;
	Check(read, "pre flight: a published word reads back with its position and time");
	PlanStorePreUpdate(slot, f, PLAN_PRE_LATE);
	PlanPreFlight late;
	bool updated = PlanStorePreRead(slot, &late) && late.state == PLAN_PRE_LATE;
	PlanStorePreUpdate(slot, f, PLAN_PRE_NONE);
	PlanPreFlight still;
	Check(updated && PlanStorePreRead(slot, &still) && still.state == PLAN_PRE_LATE,
	      "pre flight: an update applies against the word read, and a stale one is left");
	PlanStorePreUpdate(slot, still, PLAN_PRE_NONE);
	Check(!PlanStorePreRead(slot, &f), "pre flight: a NONE update clears the word");

	PlanStorePreIssue(slot, v.epoch, 0, 1, 990.0f, 4.0f, now.QuadPart);
	MakeWrite(&w, CM_A, 2.0f);
	PlanStoreWrite(w);
	unsigned epoch = 0;
	int leg = -1;
	Check(PlanStorePreRead(slot, &f) && PlanStoreLeg(slot, &epoch, &leg) && f.epoch != epoch,
	      "pre flight: a rewrite leaves the word, its epoch no longer the slot's");
	PlanStoreReset();
	Check(!PlanStorePreRead(slot, &f), "pre flight: the save-load reset clears the word");

	Fresh(PLANNER_ON);
	MakeWrite(&w, CM_A, 1.0f);
	slot = PlanStoreWrite(w);
	PlanStoreRead(slot, &v);
	bool forward = PlanStoreAdvance(slot, v.epoch, 0, 1);
	PlanStoreAddRung(slot, v.epoch);
	PlanStoreAddRung(slot, v.epoch);
	bool back = PlanStoreAdvance(slot, v.epoch, 1, 0);
	PlanStoreRead(slot, &v);
	Check(forward && back && v.legIndex == 0 && v.rungs == 0,
	      "pre flight: the step-back's backward advance restarts the rungs");

	PlanStoreSetPreHold(0x7FFFFFFFFFFFLL);
	PlanStoreAdvance(slot, v.epoch, 0, 1);
	QueryPerformanceCounter(&now);
	PlanStorePreIssue(slot, v.epoch, 0, 1, 990.0f, 4.0f, now.QuadPart - 1000);
	Check(PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f),
	      "owns: a late landing's standstill at the portal the pre-request left is the planner's");
	PlanStoreSetPreHold(1);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f), "owns: an in-flight word older than the hold is not owned");
	PlanStoreSetPreHold(0);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f), "owns: with the hold at 0 the clause is off");

	PlanStoreSetPreHold(0x7FFFFFFFFFFFLL);
	PlanStoreRead(slot, &v);
	PlanStorePreIssue(slot, v.epoch, 0, 2, 990.0f, 4.0f, now.QuadPart);
	Check(!PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f),
	      "owns: a word for another leg than the slot's does not own the standstill");
	PlanStorePreIssue(slot, v.epoch, 0, 1, 990.0f, 4.0f, now.QuadPart);
	MakeWrite(&w, CM_A, 1.0f);
	w.firstLeg = 1;
	PlanStoreWrite(w);
	Check(PlanStorePreRead(slot, &f) && f.to == 1 && !PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f),
	      "owns: a word from an earlier epoch does not own the standstill");
	PlanStoreRead(slot, &v);
	unsigned cleared = v.epoch;
	PlanStoreDrop(CM_A);
	PlanStorePreIssue(slot, cleared, 0, 1, 990.0f, 4.0f, now.QuadPart);
	MakeWrite(&w, CM_A, 1.0f);
	w.firstLeg = 1;
	int reused = PlanStoreWrite(w);
	Check(reused == slot && PlanStorePreRead(slot, &f) && f.epoch == cleared && PlanStoreLeg(slot, &epoch, &leg)
	      && epoch != cleared && leg == 1 && !PlannerOwnsWait(CM_A, 1005.0f, 5.0f, 2000.0f, 0.0f),
	      "pre flight: a word issued after the slot's clear keeps the old epoch and owns nothing");
	PlanStoreSetPreHold(0);

	PlanStoreRead(slot, &v);
	int first = PlanStorePreNoteSkip(slot, v.epoch, 1, 0);
	int again = PlanStorePreNoteSkip(slot, v.epoch, 1, 0);
	bool waitOpen = !PlanStorePreBlocked(slot, v.epoch, 1);
	int blockedNow = PlanStorePreNoteSkip(slot, v.epoch, 1, 1);
	bool blocked = PlanStorePreBlocked(slot, v.epoch, 1);
	int other = PlanStorePreNoteSkip(slot, v.epoch, 2, 1);
	Check(first == 1 && again == 0 && waitOpen && blockedNow == 0 && blocked && other == 1
	      && !PlanStorePreBlocked(slot, v.epoch, 1),
	      "pre flight: a leg's first skip counts once, a wait is tried again, a block holds until another leg");
	Fresh(PLANNER_OFF);
}

// The pre-arrival snap's answer: a face goes on; the navmesh lock's refusal (-1) retries next frame,
// counted in preBusy with the leg unmarked, so the next answer is still the leg's first; no face (0, or
// any other value) is a counted SNAP skip that blocks the leg; a wait skip counts and never blocks.
static void PreSnapRows()
{
	Fresh(PLANNER_ON);
	PlanWrite w;
	MakeWrite(&w, CM_A, 1.0f);
	int slot = PlanStoreWrite(w);
	PlanView v;
	PlanStoreRead(slot, &v);
	PlannerCounters* c = PlannerCountersGet();
	const LONG busy = c->preBusy, skip = c->preSkip, snap = c->preSkipSnap, wait = c->preSkipWait;
	int hit = PlanStorePreSnap(slot, v.epoch, 0, 1);
	Check(hit == PLAN_PRE_SNAP_HIT && c->preBusy == busy && c->preSkip == skip && !PlanStorePreBlocked(slot, v.epoch, 0),
	      "pre snap: a face goes on, nothing counted or marked");
	int retry = PlanStorePreSnap(slot, v.epoch, 0, -1);
	bool retryClean = retry == PLAN_PRE_SNAP_RETRY && c->preBusy == busy + 1 && c->preSkip == skip
	                  && c->preSkipSnap == snap && !PlanStorePreBlocked(slot, v.epoch, 0);
	Check(retryClean, "pre snap: a refused navmesh lock (-1) retries, counted in preBusy, no skip recorded, the leg unmarked");
	int block = PlanStorePreSnap(slot, v.epoch, 0, 0);
	Check(block == PLAN_PRE_SNAP_BLOCK && c->preSkip == skip + 1 && c->preSkipSnap == snap + 1 && c->preBusy == busy + 1
	      && PlanStorePreBlocked(slot, v.epoch, 0),
	      "pre snap: no face (0) after a retry is the leg's first skip, a counted SNAP block");
	int stray = PlanStorePreSnap(slot, v.epoch, 1, 5);
	Check(stray == PLAN_PRE_SNAP_BLOCK && c->preSkipSnap == snap + 2 && PlanStorePreBlocked(slot, v.epoch, 1),
	      "pre snap: any answer but 1 and -1 blocks as SNAP");
	PlanStorePreSkip(slot, v.epoch, 2, PPS_WAIT);
	PlanStorePreSkip(slot, v.epoch, 2, PPS_WAIT);
	Check(c->preSkipWait == wait + 1 && c->preSkip == skip + 3 && !PlanStorePreBlocked(slot, v.epoch, 2),
	      "pre skip: a wait skip counts on the leg's first skip only and never blocks");
	Fresh(PLANNER_OFF);
}

int main()
{
	StoreRows();
	OwnsRows();
	SendRows();
	HoldRows();
	PreFlightRows();
	PreSnapRows();
	return CheckExit("plan_store_units");
}
