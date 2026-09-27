#ifndef KENSHI_ZONE_OPT_NM_ADJACENCY_H
#define KENSHI_ZONE_OPT_NM_ADJACENCY_H

// Navmesh adjacency exclusion (nm_adjacency_policy.h has the rule).
//
// A job is registered when it is claimed and stays registered until the path
// thread's drain has popped its task from done: a pass-through detour on
// NavMeshGenerator::update ends the window. Workers skip a job that conflicts
// with a registered one; the bg thread reserves the first such job and runs a
// later one, and waits (no lock held) only when nothing in the queue can run. navmeshAdjExclusion=false keeps the registry, the detour and the
// checker and only counts (adjWould=) what enforcement would defer.
//
// Every "Locked" call needs the generator's queue lock (+152) held by the
// caller; nothing here allocates or logs under it.

#include "base/config.h"
#include "navmesh/scheduling/nm_adjacency_policy.h"
#include <windows.h>
#include <stdint.h>


bool NmAdjActive();
bool NmAdjEnforcing();

// Reads a queued task's descriptor. The task must be in the queue (the caller
// holds +152), so it is live.
void NmAdjDescribeTask(uintptr_t task, NmJobDesc* out);

// The head the bg thread keeps in place; workers never take it and the
// prioritizer never moves it.
uintptr_t NmAdjPinnedLocked();

// Worker scan (WorkerTryDequeueAny).
void       NmAdjWorkerScanBeginLocked(NmAdjScan* s);
NmAdjOffer NmAdjWorkerOfferLocked(NmAdjScan* s, uintptr_t task, bool eligible, NmJobDesc* descOut);
// A node that is not a candidate, or any node after the take.
void       NmAdjWorkerObserveLocked(NmAdjScan* s, uintptr_t task, bool eligible);
// Returns false when the take was refused (no free entry, enforcing); the
// caller then leaves the job queued. Sets this thread's own job on a claim.
bool       NmAdjWorkerScanEndLocked(NmAdjScan* s, int workerId, const NmJobDesc* takenDesc, bool canReserve);
// After the unlock: wakes waiters if the scan changed the registry.
void       NmAdjAfterScan(const NmAdjScan* s);

// bg thread: one scan over the queue under +152 (nm_adjacency_policy.h has the
// order). Offer every node in queue order until the offer returns true.
// eligible: zone and content set, and not a type 0/1 job of the zone being
// unloaded. bgOnly: types 2/3/4 (a skip is always one).
void            NmAdjBgScanBeginLocked(NmAdjBgScan* s);
bool            NmAdjBgOfferLocked(NmAdjBgScan* s, uintptr_t task, bool eligible, bool bgOnly);
// CLAIM: *pick is registered and this thread owns it. A bg-only pick is pinned,
// and the caller moves it to the front for the original. WAIT: nothing runnable,
// something blocked. NONE: nothing eligible.
// episodeStart: the first scan of this dispatch's wait episode (counters).
NmAdjBgDecision NmAdjBgScanEndLocked(NmAdjBgScan* s, uintptr_t* pick, bool episodeStart);
// The head goes to the original with no stitch (no content): pinned.
void            NmAdjBgForwardLocked(uintptr_t head);
void            NmAdjBgReleaseLocked();
void            NmAdjBgUnpin();           // takes +152
// The bg thread found the queue empty without taking +152.
void            NmAdjBgLookedUnlocked();
enum NmAdjWaitResult { NMADJ_WAIT_RETRY = 0, NMADJ_WAIT_SLICE, NMADJ_WAIT_STOP };
// No lock held. sliceStart: 0 on the first call of this dispatch. RETRY: a
// claim was released (or the slice ran out, once): scan again. SLICE: the
// caller returns 0 so threadProc sleeps.
NmAdjWaitResult NmAdjBgWait(uintptr_t nmg, LONGLONG* sliceStart);
// Ends a wait episode (a no-op when none is open) and records it.
void            NmAdjBgWaitDone(LONGLONG* sliceStart, bool idle);

// This thread's claimed job is finished: published (or dropped; the drain
// observer frees a task it never finds). Takes +152.
void NmAdjOwnFinished();
// The claim ended before any stitch and nothing was pushed (vanilla's
// content-check return). Takes +152.
void NmAdjOwnDropped();


// Called by the stitch detour before the original: the always-on checker.
// splice: the call pairs two private stack instances.
void NmAdjCheckStitch(const void* a, const void* b, bool splice);
// The stitch detour installed (else the checker's fields print "?").
void NmAdjNoteCheckerPresent();

void InstallNavMeshAdjacency(int* installed, int*);
void NavMeshAdjTick(double now);

#endif // KENSHI_ZONE_OPT_NM_ADJACENCY_H
