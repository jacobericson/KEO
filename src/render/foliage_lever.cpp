#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/foliage_budget.h"
#include "game/game.h"
#include "base/core.h"
#include <float.h>
#include <iomanip>
#include <sstream>

// PagedGeometry::update runs on the main thread, once per foliage layer of
// every zone in the game's active set, from FoliageSystem::update inside the
// camera-zone update. Each call loads and builds the pages entering its range
// to completion before it returns, and every timer it keeps (page age, cache
// interval, grass wind) advances by the wall-clock time since its own last
// call. A call left out for a frame therefore leaves no page half built; the
// next call covers the longer interval.
//
// With foliagePageBudgetMs above 0 every call is timed. Above
// foliageBudgetSpeed, once a frame's calls have spent the budget the rest are
// left out, and the next budgeted frame starts where this one stopped.

typedef void* (*PagedGeometryUpdate_t)(void* pg);

static PagedGeometryUpdate_t s_orig      = NULL;
static const float*          s_gameSpeed = NULL;   // GameWorld::frameSpeedMult
static double                s_msPerTick = 0.0;
static bool                  s_installed = false;

// Main thread only.
static FoliageBudget s_budget;
static LONG          s_calls   = 0;
static LONG          s_skipped = 0;

static bool BudgetActive()
{
	float speed = *s_gameSpeed;
	return _finite(speed) && speed > g_renderCfg.foliageBudgetSpeed;
}

static void* hook_PagedGeometryUpdate(void* pg)
{
	float budget = g_renderCfg.foliagePageBudgetMs;
	if (!(budget > 0.0f) || !IsMainThread())
		return s_orig(pg);
	++s_calls;
	if (!FoliageBudgetAdmit(&s_budget, BudgetActive(), budget))
	{
		++s_skipped;
		return pg;
	}
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);
	void* r = s_orig(pg);
	QueryPerformanceCounter(&t1);
	FoliageBudgetSpend(&s_budget, (double)(t1.QuadPart - t0.QuadPart) * s_msPerTick);
	return r;
}

void Foliage_MainThreadTick()
{
	if (s_installed)
		FoliageBudgetEndFrame(&s_budget);
}

std::string FoliageStatsToken()
{
	LONG calls = s_calls;
	LONG skipped = s_skipped;
	double msMax = s_budget.frameMaxMs;
	s_calls = 0;
	s_skipped = 0;
	s_budget.frameMaxMs = 0.0;
	if (!s_installed || !(g_renderCfg.foliagePageBudgetMs > 0.0f))
		return std::string();
	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << " foliage=" << calls << "/" << skipped << "/" << std::setprecision(2) << msMax;
	return ss.str();
}

bool InstallFoliageBudget()
{
	LARGE_INTEGER freq;
	if (!QueryPerformanceFrequency(&freq) || !freq.QuadPart)
		return false;
	s_msPerTick = 1000.0 / (double)freq.QuadPart;
	FoliageBudgetReset(&s_budget);
	s_gameSpeed = (const float*)((const char*)GameAddr(RVA_GLOBAL_GAMEWORLD) +
	                              OFF_GAMEWORLD_FRAME_SPEED_MULT);
	if (!VerifyPrologueByRva(RVA_PAGED_GEOMETRY_UPDATE))
		return false;
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(
	        GameAddr(RVA_PAGED_GEOMETRY_UPDATE), hook_PagedGeometryUpdate, &s_orig))
		return false;
	s_installed = true;
	return true;
}
