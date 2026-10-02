// planner_water.h - The route planner's per-character water multiplier: the reads of each ordered
// character's speeds and the engine's own water value, the two engine readers bound at arm, and the
// order's pre-pass that plans a run-together squad on one value. Main thread only.
#pragma once
#include <stdint.h>

namespace planner {

const int PLAN_WATER_ORDER_MAX = 32;   // the order hook passes at most MAX_FORMATION_MEMBERS (30)

// Main thread, from PlannerTickArm: snapshots the mode (PlanWaterMode) and binds the two engine
// readers after checking their prologues; a mismatch leaves that reader unbound (every speed read
// then fails and the engine value alone applies) and the planner armed. True when both bound.
bool PlannerWaterArm(int mode);
// "ok", "refused(calculateSwimSpeed)" or "refused(getWaterLevel)" after the arm; "unarmed" before.
const char* PlannerWaterBindToken();
// Main thread, from the order capture: the multiplier of each of chars[0..n) into mult (the first
// PLAN_WATER_ORDER_MAX of them). A run-together order (n > 1, every member GROUPED) gives every member
// one value and returns 1; otherwise each member gets its own and it returns 0.
int PlannerOrderWater(const uintptr_t* chars, int n, float* mult);

} // namespace planner
