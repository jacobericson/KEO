// planner_water.h - The route planner's per-character water multiplier: the reads of each ordered
// character's speeds and the engine's own water value, the two engine readers bound at arm, and the
// order's pre-pass that plans a run-together squad on one value. Main thread only.
#pragma once
#include <stdint.h>

namespace planner {

const int PLAN_WATER_ORDER_MAX = 32;   // the order hook passes at most MAX_FORMATION_MEMBERS (30)

// Main thread, from PlannerTickArm: decides whether the request write runs (the planner on, the water
// engine key at match, both request rows installed) and arms the water mode through the coherence
// rule, then binds the two engine readers after checking their prologues; a mismatch leaves that
// reader unbound (every speed read then fails and the engine value alone applies) and the planner
// armed. True when both bound.
bool PlannerWaterArm(int mode);
// "ok", "refused(calculateSwimSpeed)" or "refused(getWaterLevel)" after the arm; "unarmed" before.
const char* PlannerWaterBindToken();
// "ok", "refused(isImmune)" or "refused(lookupFromPosition)" after the arm; "unarmed" before.
const char* PlannerWaterAcidToken();
// Main thread, from the order capture: the multiplier of each of chars[0..n) into mult and, when acid is
// not NULL, its acid factor into acid (the first PLAN_WATER_ORDER_MAX of them). A run-together order
// (n > 1, every member GROUPED) gives every member one multiplier and one factor and returns 1;
// otherwise each member gets its own and it returns 0.
int PlannerOrderWater(const uintptr_t* chars, int n, float* mult, float* acid);
// Main thread: whether chars[0..n) run together (more than one, every member GROUPED).
bool PlannerOrderRunTogether(const uintptr_t* chars, int n);
// Main thread, every planner tick: republishes the water table from the player characters, each at
// its plan's multiplier while it holds a plan, else its own from a read made now, else its last good
// value. Returns at once while the request write is not armed live.
void PlannerWaterRefresh();
// Main thread, at a save load's first frame: empties the table and the main thread's copy of it.
void PlannerWaterReset();
// The armed water mode's name, "floor(dynamic)" when dynamic armed as floor; and the request write's
// state: "match", "off", "off(observe)", "refused(requestPath)" or "refused(pathReqSubmit)".
const char* PlannerWaterModeToken();
const char* PlannerWaterEngineToken();

// Main thread, from the planner's frame step outside a load: one step of the acid cells' fill.
// Returns at once while the planner is off or the lookup is unbound.
void PlannerWaterAcidFrame(double now);
} // namespace planner
