// plan_store.h - The route planner's per-character plan slots and its counters. The main thread
// writes a slot; any thread reads it without a lock (a sequence word: the slot's epoch and its
// current leg share one 64-bit word, odd while the main thread rewrites). No KenshiLib or game
// header.
#ifndef KEO_PLANNER_PLAN_STORE_H
#define KEO_PLANNER_PLAN_STORE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include "planner/plan_policy.h"

namespace planner {

const int PLAN_SLOTS = 64;

// What the main thread writes for one character's order.
struct PlanWrite
{
	uintptr_t cm;               // the CharMovement*, the slot's key
	int       verdict;          // PlanVerdict
	int       legCount;         // 0..PLAN_MAX_LEGS
	int       firstLeg;         // the first target (PlanLegTarget from leg 0)
	int       routeTruncated;   // the route had more portals than PLAN_MAX_LEGS
	int       goalByFootprint;
	unsigned  loadedMask;
	float     finalDest[3];
	float     destAtPlan[3];    // the character's movement destination as the order's plan is written (x, 0, z)
	double    now;
	int       keepSends;        // 1: a re-plan of the same character keeps the mod's recorded sends and
	                            //   the slot's destAtPlan
	float     waterMult;        // the water multiplier the plan was searched at; its re-plans reuse it
	float     acidMult;         // the acid factor the plan was searched at; its re-plans reuse it
	int       orderOutdoors;      // the order's building argument was NULL; a re-plan passes the view's back
	int       holdInteriorPortal; // PlanHoldInteriorPortal of this plan's goal, set by WritePlan
	PlanLeg   legs[PLAN_MAX_LEGS];
};
// A consistent copy for any thread.
struct PlanView
{
	int       slot;
	unsigned  epoch;            // even; the advance and the word updates are conditioned on it
	uintptr_t cm;
	int       verdict, legIndex, legCount, waiting, rungs, routeTruncated;
	unsigned  loadedMask;
	float     finalDest[3];
	float     destAtPlan[3];
	float     resend[PLAN_RESEND_POINTS][3]; int resendCount;
	float     waterMult;
	float     acidMult;
	int       orderOutdoors, holdInteriorPortal;
	PlanLeg   legs[PLAN_MAX_LEGS];
};
// The main thread's own fields for one slot (never read off the main thread).
struct PlanMainState { double planTime, waitSince, completeSince; int goalByFootprint, routeTruncated, consultedAtPlan; unsigned loadedGenAtPlan; double holdTime; float holdDest[3]; int haveHold; };

// Main thread.
void PlanStoreArm(int mode);                 // PlannerMode; PLANNER_OFF disarms
int  PlanStoreMode();                        // any thread
int  PlanStoreWrite(const PlanWrite& w);     // the character's slot, replacing its plan, or a free
                                             //   one; -1 when all 64 are in use (slotFull counted)
int  PlanStoreDrop(uintptr_t cm);            // the dropped plan's getZoneEdge consultations,
                                             //   -1 when the character had no plan; drops counted by the caller
void PlanStoreReset();                       // every slot freed (a save load)
void PlanStoreSetLoaded(int slot, unsigned mask);
PlanMainState* PlanStoreMain(int slot);
uintptr_t PlanStoreKey(int slot);            // 0 for a free slot

// Any thread, lock-free.
int  PlanStoreFind(uintptr_t cm);            // -1 when unplanned or the store is unarmed
bool PlanStoreRead(int slot, PlanView* out); // false while the slot is rewritten, free, or twice torn
bool PlanStoreAdvance(int slot, unsigned epoch, int fromLeg, int toLeg);   // the 64-bit CAS
void PlanStoreSetWaiting(int slot, unsigned epoch, int waiting);           // ignored after a rewrite
void PlanStoreAddRung(int slot, unsigned epoch);
void PlanStoreNoteArrival(int slot, unsigned epoch);    // sets the arrival word the tick consumes
int  PlanStoreTakeArrival(int slot);                    // main thread: reads and clears it
void PlanStoreNoteConsulted(int slot);

// Main thread: the tracker's query. The character's stop at its current portal belongs to the planner
// (PlanOwnsWait on the slot's current state, the position and waypoint the caller already read, and
// whether the movement destination, read through the installed reader, is the plan's in x-z).
// Unarmed, or no reader installed: false at once.
bool PlannerOwnsWait(uintptr_t cm, float posX, float posZ, float wpX, float wpZ);
// Main thread: the movement's last requested destination (x and z; y zero), installed when the planner
// arms; NULL until then.
typedef void (*PlanMoveDestReader)(uintptr_t cm, float out[3]);
void PlanStoreSetMoveDestReader(PlanMoveDestReader fn);
// Main thread, when the planner arms: whether the owned-wait test measures to a portal's whole border
// edge (plannerLegAim) rather than its midpoint; 0 until then.
void PlanStoreSetLegAim(int on);

// The slot's pre-arrival request in flight. Two writers: the thread updating the character (the AI
// back thread, or the main thread with characterMultithreading off) issues and resolves it, and the
// main thread's slot clear and save-load reset zero it; a rewrite leaves it. A word that survives a
// clear carries the old epoch, and every reader drops a word whose epoch is not the slot's. The main
// thread reads it only in PlannerOwnsWait.
struct PlanPreFlight { unsigned epoch; int state, from, to; float issueX, issueZ; LONGLONG issueQpc; };
// Stores the issue position and QPC time, then publishes the word (state ISSUED) in one exchange.
void PlanStorePreIssue(int slot, unsigned epoch, int from, int to, float x, float z, LONGLONG qpc);
// The word as stored; false when none is set.
bool PlanStorePreRead(int slot, PlanPreFlight* out);
// Replaces the word read as f with state (PLAN_PRE_NONE clears it) by a compare-exchange: a word
// changed since f was read is left.
void PlanStorePreUpdate(int slot, const PlanPreFlight& f, int state);
// The slot's current epoch and leg; false while it is rewritten.
bool PlanStoreLeg(int slot, unsigned* epoch, int* leg);
// A pre-arrival skip of (epoch, leg): 1 when it is the leg's first skip (the caller counts it); with
// blocks set, the leg is not tried again (a wait skip is tried again every frame).
int  PlanStorePreNoteSkip(int slot, unsigned epoch, int leg, int blocks);
bool PlanStorePreBlocked(int slot, unsigned epoch, int leg);
// A pre-arrival skip of the slot's leg for a PlanPreSkip reason: counted, by reason, on the leg's first
// skip only; every reason but PPS_WAIT blocks the leg from another try (the recheck at its portal
// serves it). Interlocked only.
void PlanStorePreSkip(int slot, unsigned epoch, int leg, int why);
// The pre-arrival snap's answer for the slot's leg, got being getClosestPoint's return (-1 also for no
// navmesh). 1, a face: PLAN_PRE_SNAP_HIT. -1, the navmesh lock refused: PLAN_PRE_SNAP_RETRY, counted in
// preBusy with the slot untouched, so the next frame tries again. Any other value, no face within the
// radius: PLAN_PRE_SNAP_BLOCK, a PPS_SNAP skip. Interlocked only.
enum PlanPreSnap { PLAN_PRE_SNAP_HIT = 0, PLAN_PRE_SNAP_RETRY, PLAN_PRE_SNAP_BLOCK };
int  PlanStorePreSnap(int slot, unsigned epoch, int leg, int got);
// Main thread, at the pre-arrival install: the owned-wait clause's age cap in QPC ticks; 0, the
// default, leaves the clause off.
void PlanStoreSetPreHold(LONGLONG ticks);

enum PlanSendKind { PLAN_SEND_RESEND = 1, PLAN_SEND_HOLD = 2 };
// Main thread, the movement module's send paths, before the order is sent. PLAN_SEND_RESEND: the
// order's destination re-sent (nudged past the engine's two-unit drop); it joins the slot's record of
// the two newest distinct re-sends unless the plan already accepts it, and a call carrying it is the
// plan's. PLAN_SEND_HOLD: a detour that keeps the plan for PLAN_HOLD_SECONDS of game time without
// steering it; its `now` is game time, the clock the tick ages the hold on.
// 1: the character holds a plan (the send is counted); 0: no plan, or unarmed; -1: a re-send farther
// than PLAN_RESEND_REACH from the plan's destination (refused and counted; the plan then drops).
int PlannerNoteModSend(uintptr_t cm, const float sent[3], int kind, double now);
// Any thread, from the getZoneEdge detour: the snap's raw-to-snapped distance; interlocked only.
void PlannerNoteSnap(float distance);

// Host tests only: a callback run inside a rewrite while the epoch is odd, and one run by a read
// between its two legWord loads; NULL in the game.
void PlanStoreTestPauseInRewrite(void (*fn)(void* ctx), void* ctx);
void PlanStoreTestPauseInRead(void (*fn)(void* ctx), void* ctx);

const int PLAN_DROP_REASONS = 7; const int PLAN_REPLAN_REASONS = 7;

struct PlannerCounters
{
	volatile LONG plans, direct, legged, noRoute, legs, arrivals, rungs, replans, drops;
	volatile LONG roadPreempt, notConsulted, staleRerequest, snapFail, flips, waits;
	volatile LONG slotFull, repeats, locFail, goalUnlocated, startUnlocated, notSite, staleAdvance, rung17, ownedSkips, noLocation;
	volatile LONG reissuedPlanned, heldPlanned, reissueRefused, snapFar, snapMax;
	volatile LONG waterFail, waterGroups;   // members whose speed read failed; orders planned run-together
	volatile LONG aimCount, aimShiftSum;   // aimed recomputes, their summed shift in units
	volatile LONG merges, mergeJoins, mergeAlone, mergeMoved, mergeWalkOff;   // orders merged; members joined, alone; gathers moved; walks off
	volatile LONG interiorHeld;   // plans written holding an interior goal at its building's portal
	volatile LONG pre, preLand, preLate, preBroken, preFailed, preLost;   // pre-arrival advances and their outcomes
	volatile LONG preSkip, preSkipWait, preSkipSnap, preSkipSame, preSkipHeld;   // legs skipped, the first skip each, by reason
	volatile LONG preBusy;   // pre-arrival snaps the navmesh lock refused, each retried the next frame
	volatile LONG preDCount, preDSum, preDMax;   // landings sampled; units walked from the issue, summed and the most
	volatile LONG dropsBy[PLAN_DROP_REASONS], replansBy[PLAN_REPLAN_REASONS];   // by PlanDropWhy / PlanReplanWhy
};
PlannerCounters* PlannerCountersGet();   // any thread; the fields are interlocked

} // namespace planner

#endif
