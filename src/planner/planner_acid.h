// planner_acid.h - The route planner's acidic-water cells and its read of a race's acid immunity. The
// cell table holds, per exterior directory cell, whether the biome there has acidic water that hurts a
// swimmer; the main thread fills it through a lookup callback after each world start and the search
// reads it. The immunity read calls RaceData::isImmune through a binding checked against its first 16
// bytes. No game header; main thread only.
#pragma once
#include <stdint.h>

namespace planner {

const int    PLAN_ACID_CELLS          = 4096;   // the exterior directory: gy * 64 + gx
const double PLAN_ACID_RETRY_SECONDS  = 60.0;   // unknown cells are retried this long after the first pass
const double PLAN_ACID_RETRY_INTERVAL = 1.0;    // at most one retry pass per this many seconds
const int    PLAN_WA_ACID             = 2;      // WeatherAffecting::WA_ACID

// The fill's lookup at a world point: 1 when the biome there is known, with *acidWater its acidic-water
// value (0 when its acid does not apply); 0 while the world has no biome there yet.
typedef int (*PlanAcidLookupFn)(void* ctx, float x, float z, float* acidWater);

// Main thread: every cell unknown, and the next fill step is a world's first pass. A world start.
void PlannerAcidNewWorld();
// Main thread, from the planner's frame step outside a load: a world's first step looks every cell up at
// its centre (PlanCellCentre); later steps look the still-unknown cells up again, at most once per
// PLAN_ACID_RETRY_INTERVAL and for PLAN_ACID_RETRY_SECONDS after the first pass. Returns the lookups made.
int  PlannerAcidFillStep(double now, PlanAcidLookupFn lookup, void* ctx);
// Main thread: 1 when the directory cell's acidic water hurts; 0 for a dry or unknown cell, an interior
// section (dir >= PLAN_ACID_CELLS: its water byte is 0) and an index out of range.
int  PlannerAcidCellIs(int dir);
struct PlanAcidStats { int acidCells; int unknown; };
void PlannerAcidStatsGet(PlanAcidStats* out);

// RaceData::isImmune(WeatherAffecting): rcx the race, edx the weather kind.
typedef bool (*PlanIsImmuneFn)(void* race, int weatherAffecting);
// Main thread, at the arm: binds fn when the 16 bytes at code are isImmune's prologue, else leaves it
// unbound. True when bound. In the game code and fn are one address; a host suite passes them apart.
bool PlannerAcidBind(const void* code, PlanIsImmuneFn fn);
// Main thread: 1 when the race is immune to acid, when race is NULL, and while unbound (the planner then
// never prices acid above water); 0 otherwise.
int  PlannerAcidImmune(void* race);

} // namespace planner
