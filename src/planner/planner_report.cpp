// planner_report.cpp - The route planner's main-thread log lines: the per-order plan line, capped
// per session, and the heartbeat of the plan store's counters on a timer. Main thread only; no lock.
#include "planner/planner_tick.h"
#include "planner/plan_store.h"
#include "base/core.h"
#include <stdio.h>

namespace planner {

static const int PLAN_LINES_MAX = 32;

static int    s_planLines  = 0;
static double s_lastReport = 0.0;
static LONG   s_lastSum    = 0;

void PlannerReportPlan(const char* line)
{
	if (s_planLines < PLAN_LINES_MAX)
		LogMsg(line);
	else if (s_planLines == PLAN_LINES_MAX)
		LogMsg("Planner plan: further plans counted, not printed");
	else
		return;
	++s_planLines;
}

// The sum of every counter: the heartbeat prints only when one of them moved.
static LONG CounterSum(const PlannerCounters& c)
{
	return c.plans + c.direct + c.legged + c.noRoute + c.legs + c.arrivals + c.rungs + c.replans + c.drops
	     + c.roadPreempt + c.notConsulted + c.staleRerequest + c.snapFail + c.flips + c.waits
	     + c.slotFull + c.repeats + c.locFail + c.goalUnlocated + c.startUnlocated + c.notSite + c.staleAdvance + c.rung17
	     + c.ownedSkips + c.noLocation;
}

void PlannerReportTick(double now)
{
#ifdef ZONEOPT_DEBUG
	const double interval = 30.0;
#else
	const double interval = 60.0;
#endif
	if (now - s_lastReport < interval)
		return;
	s_lastReport = now;
	const PlannerCounters& c = *PlannerCountersGet();
	LONG sum = CounterSum(c);
	if (sum == s_lastSum)
		return;
	s_lastSum = sum;
	char line[640];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner: plans=%ld direct=%ld legged=%ld noRoute=%ld legs=%ld arrivals=%ld rungs=%ld replans=%ld"
	            " drops=%ld roadPreempt=%ld notConsulted=%ld staleRerequest=%ld snapFail=%ld flips=%ld waits=%ld"
	            " slotFull=%ld repeats=%ld locFail=%ld goalUnlocated=%ld startUnlocated=%ld notSite=%ld staleAdvance=%ld"
	            " rung17=%ld ownedSkips=%ld noLocation=%ld",
	            (long)c.plans, (long)c.direct, (long)c.legged, (long)c.noRoute, (long)c.legs, (long)c.arrivals,
	            (long)c.rungs, (long)c.replans, (long)c.drops, (long)c.roadPreempt, (long)c.notConsulted,
	            (long)c.staleRerequest, (long)c.snapFail, (long)c.flips, (long)c.waits, (long)c.slotFull,
	            (long)c.repeats, (long)c.locFail, (long)c.goalUnlocated, (long)c.startUnlocated, (long)c.notSite,
	            (long)c.staleAdvance, (long)c.rung17, (long)c.ownedSkips, (long)c.noLocation);
	LogMsg(line);
}

} // namespace planner
