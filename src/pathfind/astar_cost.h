// astar_cost.h — A* cost logging
//
// Diagnostics only: no change to search behaviour. hook_findPathFull
// (pathfind_hooks.cpp) builds one AstarCostSample per call, immediately
// around orig_findPathFull, and hands it to AstarCostNote here and to
// PathPoolNoteSearch (path_pool.h) -- one sample, two consumers, so the two
// modules cannot disagree about what a search cost.

#ifndef KEO_ASTAR_COST_H
#define KEO_ASTAR_COST_H

#include "base/core.h"
#include "base/config.h"
#include "pathfind/astar_cost_policy.h"


// One findPathFull call. Lock-free/allocation-free to build and to fold into
// AstarCostNote's accumulators: called on the path thread, NavMesh worker
// threads, the main thread and (per the six known callers) a NavMesh
// generation thread. No CRT string work anywhere on this path.
struct AstarCostSample
{
	LONGLONG ticks;         // QPC ticks around orig_findPathFull
	int      iterations;    // FindPathOutput +0x30 (m_numIterations)
	int      status;        // FindPathOutput +0x3C
	int      cause;         // FindPathOutput +0x3D (meaningful for status 3)
	int      boosted;       // 1 if the 4x budget boost wrote this search's byte budgets
	int      playerByReq;   // -1 unknown, 0 npc, 1 player; the request-derived tag, not the sticky one
	void*    returnAddr;    // _ReturnAddress() taken at hook_findPathFull's entry
	unsigned startFaceKey;  // FindPathInput +0x30
	unsigned goalFaceKey;   // FindPathInput's first goal face key (+0x38 array), 0xFFFFFFFF if none
	float    goalDist3D;    // |goal - start|, -1.0f if either point was unavailable
};

// Resolves returnAddr against the game module's own image (gameBase, its
// SizeOfImage) and classifies it (astar_cost_policy.h). Exposed so
// hook_findPathFull can compute the class once and stamp it into both this
// module's sample and PathSearchSample.callerClass (path_pool.h) -- one
// sample, two consumers, so they cannot disagree about what a search was.
AstarCallerClass AstarResolveCallerClass(void* returnAddr, int playerByReq);

// Any thread; see AstarCostSample's comment above. Buckets latency/
// iterations and folds the sample into the running per-class histograms and
// the character cause=3/other cap counters. callerClass is the value
// AstarResolveCallerClass already returned for this call (passed in, not
// recomputed, so it matches whatever PathPoolNoteSearch was given).
void AstarCostNote(const AstarCostSample* s, AstarCallerClass callerClass);

// Main thread only, called from PathPoolTickMain next to PrintAstarCostLine
// (same window cadence). Prints:
//   AstarCap:   cumulative character cause=3 searches (session totals, not
//               windowed), by player/NPC and by whether the 4x
//               budget boost was active for that search, plus how many
//               non-character (gate) searches carried the boost anyway --
//               always printed, this is the decision-relevant line.
//   AstarSlow:  the slowest capped searches seen since the last print, then
//               reset for the next window.
//   AstarClass: one line per caller class with searches: status counts,
//               and, split by success / cause=3-unboosted / cause=3-boosted
//               / other, a count, a total service time and the non-empty
//               latency/iteration histogram buckets. Gated on
//               pathCostLinesEnabled (verbosity only).
void AstarCostTick();

// Main thread only: TransitionCompleteIfPending's one hook-in (transition_hook.cpp),
// called once a bracket that was not superseded has closed. Increments the
// completed-transition denominator AstarCap:'s rate divides by.
void AstarCostOnTransitionClosed();

// The manifest row records whether hook_findPathFull installed successfully,
// independent of pathfindDiagEnabled, which a sibling failure can clear.
// Every printer here, and PathBusy: in path_pool_report.cpp, uses this answer.
bool AstarCostHookInstalled();

#endif // KEO_ASTAR_COST_H
