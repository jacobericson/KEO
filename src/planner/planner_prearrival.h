// planner_prearrival.h - The pre-arrival leg request: a pre-call detour on CharMovement::update that
// makes the next leg's path request a look-ahead before the portal a planned character walks to, its
// install step, the main thread's frame step and the tokens.
#pragma once
#include <string>

namespace planner {

// Main thread, from InstallPlannerHooks once the planner's two rows armed: captures the key and the leg
// settings, installs the charMovementUpdate row while it is wanted (plannerMode=on and
// plannerPreArrivalMs > 0) and arms the detour last. A failed install leaves pre-arrival off for the
// session with one ErrorLog line; it never refuses the planner.
void InstallPlannerPreArrival(int* installed);
// Main thread, every frame from the planner's tick: publishes the game speed the detour reads. Returns
// at once unless armed.
void PlannerPreArrivalFrame();
// Main thread: the arm line's "<T>ms", "off", "off(observe)" or "refused(charMovementUpdate)".
std::string PlannerPreArrivalArmToken();
// Main thread: the banner's "on", "off" or "refused".
const char* PlannerPreArrivalBannerToken();

} // namespace planner
