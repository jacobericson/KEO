// planner_acid.cpp - The acidic-water cell table, its fill through the caller's lookup, and the
// isImmune binding. Main thread only (the planner's frame step, the arm, the order pre-pass and the
// search); no lock, no allocation, no log.
#include <string.h>
#include "planner/planner_acid.h"
#include "planner/plan_policy.h"

namespace planner {

static const unsigned char kIsImmunePrologue[16] =
	{ 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D, 0x44, 0x24, 0x38, 0x48, 0x8D, 0x54 };

static const unsigned char ACID_UNKNOWN = 0;
static const unsigned char ACID_DRY     = 1;
static const unsigned char ACID_WET     = 2;

static unsigned char  s_cell[PLAN_ACID_CELLS];
static int            s_unknown      = PLAN_ACID_CELLS;
static bool           s_worldStarted = false;
static int            s_passes       = 0;
static double         s_firstPass    = 0.0;
static double         s_lastPass     = 0.0;
static PlanIsImmuneFn s_isImmune     = NULL;

void PlannerAcidNewWorld()
{
	memset(s_cell, ACID_UNKNOWN, sizeof(s_cell));
	s_unknown = PLAN_ACID_CELLS;
	s_worldStarted = true;
	s_passes = 0;
	s_firstPass = 0.0;
	s_lastPass = 0.0;
}

int PlannerAcidFillStep(double now, PlanAcidLookupFn lookup, void* ctx)
{
	if (!lookup || !s_worldStarted)
		return 0;
	if (s_passes > 0 && (s_unknown == 0 || now - s_firstPass > PLAN_ACID_RETRY_SECONDS
	                     || now - s_lastPass < PLAN_ACID_RETRY_INTERVAL))
		return 0;
	if (s_passes == 0)
		s_firstPass = now;
	s_lastPass = now;
	++s_passes;
	int calls = 0;
	for (int dir = 0; dir < PLAN_ACID_CELLS; ++dir)
	{
		if (s_cell[dir] != ACID_UNKNOWN)
			continue;
		float x = 0.0f, z = 0.0f, acid = 0.0f;
		PlanCellCentre(dir % 64, dir / 64, &x, &z);
		++calls;
		if (!lookup(ctx, x, z, &acid))
			continue;
		s_cell[dir] = acid > 0.0f ? ACID_WET : ACID_DRY;
		--s_unknown;
	}
	return calls;
}

int PlannerAcidCellIs(int dir)
{
	return (dir >= 0 && dir < PLAN_ACID_CELLS && s_cell[dir] == ACID_WET) ? 1 : 0;
}

void PlannerAcidStatsGet(PlanAcidStats* out)
{
	int wet = 0;
	for (int dir = 0; dir < PLAN_ACID_CELLS; ++dir)
		if (s_cell[dir] == ACID_WET)
			++wet;
	out->acidCells = wet;
	out->unknown = s_unknown;
}

bool PlannerAcidBind(const void* code, PlanIsImmuneFn fn)
{
	bool ok = code && fn && memcmp(code, kIsImmunePrologue, sizeof(kIsImmunePrologue)) == 0;
	s_isImmune = ok ? fn : NULL;
	return ok;
}

int PlannerAcidImmune(void* race)
{
	if (!s_isImmune || !race)
		return 1;
	return s_isImmune(race, PLAN_WA_ACID) ? 1 : 0;
}

} // namespace planner
