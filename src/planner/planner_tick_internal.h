// planner_tick_internal.h - Names the planner's tick (planner_tick.cpp), its order capture
// (planner_capture.cpp) and its merge (planner_merge.cpp) share: the located point, the memoised
// search, the locator, the snapshot, the search, the plan write and the drop. Main thread only; the
// locator and the snapshot take the section manager's world lock (+0x200) try-shared and release it
// on every path.
#ifndef KEO_PLANNER_PLANNER_TICK_INTERNAL_H
#define KEO_PLANNER_PLANNER_TICK_INTERNAL_H

#include "planner/plan_build.h"
#include "planner/plan_policy.h"
#include "planner/coarse_search.h"
#include <stdint.h>

namespace planner {
namespace planner_tick_detail {

// A located point: its store node and section, and whether the engine's own face lookup found it.
struct Located { unsigned key; int dir; int uid; int node; int exact; };

// One search's legs and figures, kept for the other characters of the same order.
struct Built
{
	unsigned __int64 memoKey;
	int      found;
	int      legCount, truncated;
	float    cost;
	int      expanded;
	double   ms;
	float    waterMult, waterShare;   // the search's water multiplier; the route's wet share, 0..1
	float    acidMult, acidShare;     // the search's acid factor; the route's share of water in acid cells, 0..1
	PlanLeg  legs[PLAN_MAX_LEGS];
};

void CharPos(uintptr_t character, float out[3]);
uintptr_t MovementOf(uintptr_t character);
// Refreshes the loaded-set snapshot; a busy world lock keeps the last one.
void TakeSnapshot();
// The point's store node: the engine's face lookup, else the footprints.
bool Locate(const float p[3], Located* out);
// Forgets the memoised searches; each order and each re-plan starts empty.
void ClearMemo();
const Built* SearchAndBuild(const Located& start, const Located& goal, const float dest[3], const PlanSearchParams& p);
// Writes the character's plan and feeds its route's next tiles; -1 when the store is full. orderOutdoors
// is the order's own bit (the engine's building argument was NULL), carried for the re-plans.
int WritePlan(uintptr_t cm, const float pos[3], const Located& goal, const float dest[3],
              const Built& b, double now, int* verdictOut, int keepSends, const PlanSearchParams& p,
              int orderOutdoors);
// Drops the character's plan by reason; its edge-branch consultations, -1 when it had none.
int DropPlan(uintptr_t cm, PlanDropWhy why);
// Whether the player list is longer than the tick scans; the tick and the order capture both refuse
// then, and the session build logs the first time once.
bool PlayersOverCap();
// The search scratch, and the point where a route crosses from node from into node to (the cross
// arc's portal; to's centre when the arc no longer resolves). planner_tick.cpp; the full-price
// operations themselves are planner_search_ops.h's AdapterOps, and a node's centre its AdapterPosition.
CoarseScratch* SearchScratch();
void CrossingPoint(unsigned from, unsigned to, float out[3]);
// The merge of a run-together order (planner_merge.cpp): from the order capture after the water
// pre-pass, the index in chars of the member the formation leads with (0 unless on merged the order);
// from the tick, the gather walks' off-route reading.
int MergeOrder(const uintptr_t* chars, int n, const Located& goal, const float dest[3],
               const PlanSearchParams& p, int order, double now);
void MergeTick(double now);

} // namespace planner_tick_detail
} // namespace planner

#endif
