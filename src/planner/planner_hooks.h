// planner_hooks.h - The route planner's two detours (setDestination_Vec3's entry, getZoneEdge's
// post-call), the flip the island hook consults, the install step and the banner token.
#pragma once
#include <string>
#include "planner/plan_policy.h"

namespace planner {

// Main thread, a startup install step (before the store's start step): decides the arm, installs
// both rows while plannerMode is set, and clears plannerMode on a refusal.
void InstallPlannerHooks(int* installed, int* total);
// Any thread, from the island hook only: the calling character's plan's answer. Lock-free,
// allocation-free, no logging; PFA_NOT_MINE unless armed with a matching plan (in observe, always).
PlanFlipAnswer PlannerIslandVerdict();
// Main thread: "<off|observe|on|refused(<why>)> base=<tiles>/<total> hooks=<n>/2 pre=<on|off|refused>".
std::string PlannerBannerToken();

} // namespace planner
