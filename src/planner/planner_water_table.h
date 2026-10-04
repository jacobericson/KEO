// planner_water_table.h - The per-player-character water multiplier the engine's path requests take,
// and the request write. The main thread publishes the table; any thread reads it without a lock.
// The requester passes from the requestPath detour to the submit detour in a thread-local. No game
// header.
#pragma once
#include <stdint.h>

namespace planner {

const int PLAN_WATER_TABLE_MAX = 200;   // the planner's player scan cap

// One player character's multiplier; mult 0 means none, and its requests keep the engine's value.
struct PlanWaterEntry { uintptr_t havokChar; float mult; };

// Main thread, once at the planner's arm, before any world exists. live: the request write may run;
// mode: the water mode armed (PlanWaterMode).
void PlannerWaterTableArm(int live, int mode);
// Any thread: whether the write was armed live.
int PlannerWaterTableLive();
// Main thread: replaces the published table with e[0..n), n clamped to 0..PLAN_WATER_TABLE_MAX.
void PlannerWaterTablePublish(const PlanWaterEntry* e, int n);
// Main thread: publishes an empty table.
void PlannerWaterTableClear();
// Any thread, lock-free: the character's multiplier, or 0 when it is absent or both reads were torn.
float PlannerWaterTableFind(uintptr_t havokChar);

// Any thread, from the requestPath detour: the requesting HavokCharacter* before the original, NULL
// after it. Returns at once while the write is not armed live.
void PlannerWaterNoteRequester(void* havokChar);
// Any thread, from the submit detour before its original, with the fresh request's water field: writes
// the requester's value when PlanWaterRequestValue gives one, else leaves the field. Returns at once
// while the write is not armed live. No lock, no allocation, no log.
void PlannerWaterOnSubmit(float* waterField);

struct PlanWaterTableStats { long writes; long leaves; long entries; float last; };
// Any thread: the write and leave counts, the published entry count and the last value written.
void PlannerWaterTableStatsGet(PlanWaterTableStats* out);

// Host tests only: a callback run inside a publish while the sequence word is odd; NULL in the game.
void PlannerWaterTableTestPauseInPublish(void (*fn)(void* ctx), void* ctx);

} // namespace planner
