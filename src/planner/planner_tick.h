// planner_tick.h - The route planner's main-thread half: the order capture, the tick (validation,
// drops, re-plans, the preload feed, the loaded-set snapshot) and the reports. Every entry returns
// at once while the planner is not armed.
#pragma once
#include <stdint.h>

namespace planner {

// Main thread, from hook_addOrderSelected's task-29 branch before the original: one plan per
// selected character chars[0..n) toward location, or, for a shift or addDontClear order, a drop.
void PlannerNoteOrder(const uintptr_t* chars, int n, const float* location, void* destIndoors,
                      bool shift, bool addDontClear);
// Main thread: drop a character's plan (a non-move order from the player).
void PlannerDrop(uintptr_t character);
// Main thread, every frame after IslandTick (never during a save load).
void PlannerTick(void* zoneMgr, double now);
// Main thread: in on, whether the character's legged plan replaces the straight-line ahead enqueue.
bool PlannerRouteReplacesAhead(uintptr_t character);
// Main thread, from the store's start step once the plan store is armed: allocates the search
// scratch and snapshots the tick's settings; false on an allocation failure.
bool PlannerTickArm();
// Main thread, planner_report.cpp: the capped per-order line and the heartbeat.
void PlannerReportPlan(const char* line);
void PlannerReportTick(double now);

} // namespace planner
