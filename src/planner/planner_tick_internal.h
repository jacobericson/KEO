// planner_tick_internal.h - Names the planner's tick (planner_tick.cpp) and its order capture
// (planner_capture.cpp) share: the located point, the memoised search, the locator, the snapshot,
// the search, the plan write and the drop. Main thread only; the locator and the snapshot take the
// section manager's world lock (+0x200) try-shared and release it on every path.
#ifndef KENSHI_ZONE_OPT_PLANNER_PLANNER_TICK_INTERNAL_H
#define KENSHI_ZONE_OPT_PLANNER_PLANNER_TICK_INTERNAL_H

#include "planner/plan_build.h"
#include "planner/plan_policy.h"
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
const Built* SearchAndBuild(const Located& start, const Located& goal, const float dest[3], float m);
// Writes the character's plan and feeds its route's next tiles; -1 when the store is full.
int WritePlan(uintptr_t cm, const float pos[3], const Located& goal, const float dest[3],
              const Built& b, double now, int* verdictOut, int keepSends, float m);
// Drops the character's plan by reason; its edge-branch consultations, -1 when it had none.
int DropPlan(uintptr_t cm, PlanDropWhy why);
// Whether the player list is longer than the tick scans; the tick and the order capture both refuse
// then, and the session build logs the first time once.
bool PlayersOverCap();

} // namespace planner_tick_detail
} // namespace planner

#endif
