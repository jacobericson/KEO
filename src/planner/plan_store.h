// plan_store.h - The route planner's per-character plan slots and its counters. The main thread
// writes a slot; any thread reads it without a lock (a sequence word: the slot's epoch and its
// current leg share one 64-bit word, odd while the main thread rewrites). No KenshiLib or game
// header.
#ifndef KENSHI_ZONE_OPT_PLANNER_PLAN_STORE_H
#define KENSHI_ZONE_OPT_PLANNER_PLAN_STORE_H

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
	double    now;
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
	PlanLeg   legs[PLAN_MAX_LEGS];
};
// The main thread's own fields for one slot (never read off the main thread).
struct PlanMainState { double planTime, waitSince, completeSince; int goalByFootprint, routeTruncated, consultedAtPlan; unsigned loadedGenAtPlan; };

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

// Main thread: the tracker's query. The character's wait belongs to the planner (PlanOwnsWait on
// the slot's current state and the position the caller already read). Unarmed: false at once.
bool PlannerOwnsWait(uintptr_t cm, float posX, float posZ);

// Host tests only: a callback run inside a rewrite while the epoch is odd, and one run by a read
// between its two legWord loads; NULL in the game.
void PlanStoreTestPauseInRewrite(void (*fn)(void* ctx), void* ctx);
void PlanStoreTestPauseInRead(void (*fn)(void* ctx), void* ctx);

struct PlannerCounters
{
	volatile LONG plans, direct, legged, noRoute, legs, arrivals, rungs, replans, drops;
	volatile LONG roadPreempt, notConsulted, staleRerequest, snapFail, flips, waits;
	volatile LONG slotFull, queued, locFail, notSite, staleAdvance, rung17, ownedSkips, noLocation;
};
PlannerCounters* PlannerCountersGet();   // any thread; the fields are interlocked

} // namespace planner

#endif
