// coarse_graph_base.h - The whole-map base: the builder thread, the disk cache, the save-path
// snapshot and the save-tile replace; and the planner's frame step. Every entry returns at once
// while plannerMode is off.
#pragma once

namespace planner {

// Main thread, the last startup install step: creates the store and starts the builder when
// plannerMode is not off (and plannerBaseBuild is set). No hook.
void PlannerBaseStartStep(int* installed, int* total);
// Main thread, every frame from CameraZoneSaveLoad right after PreloadCheckSaveLoad: the ZM+8
// edge (a new store generation and a save-path snapshot), the paths.count poll, the live
// promotion and the retire drain.
void PlannerOnFrame(void* zoneMgr, bool saveLoading);
// Any thread: tiles published to the base so far, and tile files found (0/0 before the build).
void PlannerBaseProgress(int* tiles, int* total);

} // namespace planner
