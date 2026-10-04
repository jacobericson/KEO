// planner_report.cpp - The route planner's main-thread log lines: the per-order plan line, capped
// per session, the heartbeat of the plan store's counters on a timer, and in the session build the
// goal section's border line. Main thread only; no lock.
#include "planner/planner_tick.h"
#include "planner/plan_store.h"
#include "planner/planner_water_table.h"
#include "planner/planner_acid.h"
#include "planner/coarse_search.h"
#include "planner/coarse_graph.h"
#include "base/core.h"
#include <stdio.h>

namespace planner {

// The per-session cap on plan lines: DEV prints every order's plans of a session, PROD the first few.
#ifdef KEO_DEBUG
static const int PLAN_LINES_MAX = 512;
#else
static const int PLAN_LINES_MAX = 32;
#endif

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

// The sum of every counter: the heartbeat prints only when one of them moved. The per-reason counts
// are left out, since each of their increments moves drops or replans in the same call.
static LONG CounterSum(const PlannerCounters& c)
{
	return c.plans + c.direct + c.legged + c.noRoute + c.legs + c.arrivals + c.rungs + c.replans + c.drops
	     + c.roadPreempt + c.notConsulted + c.staleRerequest + c.snapFail + c.flips + c.waits
	     + c.slotFull + c.repeats + c.locFail + c.goalUnlocated + c.startUnlocated + c.notSite + c.staleAdvance + c.rung17
	     + c.ownedSkips + c.noLocation
	     + c.reissuedPlanned + c.heldPlanned + c.reissueRefused + c.snapFar + c.snapMax
	     + c.waterFail + c.waterGroups + (LONG)CoarseProbeRefusals()
	     + c.arrSection + c.aimCount
	     + c.merges + c.mergeJoins + c.mergeAlone + c.mergeMoved + c.mergeWalkOff;
}

void PlannerReportTick(double now)
{
#ifdef KEO_DEBUG
	const double interval = 30.0;
#else
	const double interval = 60.0;
#endif
	if (now - s_lastReport < interval)
		return;
	s_lastReport = now;
	const PlannerCounters& c = *PlannerCountersGet();
	PlanWaterTableStats ws;
	PlannerWaterTableStatsGet(&ws);
	PlanAcidStats as;
	PlannerAcidStatsGet(&as);
	LONG sum = CounterSum(c) + ws.writes + ws.leaves;
	if (sum == s_lastSum)
		return;
	s_lastSum = sum;
	char line[1536];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner: plans=%ld direct=%ld legged=%ld noRoute=%ld legs=%ld arrivals=%ld rungs=%ld"
	            " replans=%ld(w%ld/at%ld/ru%ld/age%ld/gl%ld/end%ld)"
	            " drops=%ld(np%ld/ko%ld/arr%ld/nd%ld/ord%ld/loc%ld)"
	            " roadPreempt=%ld notConsulted=%ld staleRerequest=%ld snapFail=%ld flips=%ld waits=%ld"
	            " slotFull=%ld repeats=%ld locFail=%ld goalUnlocated=%ld startUnlocated=%ld notSite=%ld staleAdvance=%ld"
	            " rung17=%ld ownedSkips=%ld noLocation=%ld"
	            " reissuedPlanned=%ld heldPlanned=%ld reissueRefused=%ld snapFar=%ld snapMax=%ld"
	            " waterFail=%ld waterGroups=%ld probeNoRoute=%ld"
	            " waterReq=%ld waterReqLeave=%ld waterReqLast=%.2f waterTable=%ld"
	            " acidCells=%d acidUnknown=%d"
	            " arrSection=%ld aimShift=%.1f"
	            " merges=%ld mergeJoins=%ld mergeAlone=%ld mergeMoved=%ld mergeWalkOff=%ld",
	            (long)c.plans, (long)c.direct, (long)c.legged, (long)c.noRoute, (long)c.legs, (long)c.arrivals,
	            (long)c.rungs,
	            (long)c.replans, (long)c.replansBy[1], (long)c.replansBy[2], (long)c.replansBy[3], (long)c.replansBy[4],
	            (long)c.replansBy[5], (long)c.replansBy[6],
	            (long)c.drops, (long)c.dropsBy[1], (long)c.dropsBy[2], (long)c.dropsBy[3], (long)c.dropsBy[4],
	            (long)c.dropsBy[5], (long)c.dropsBy[6],
	            (long)c.roadPreempt, (long)c.notConsulted,
	            (long)c.staleRerequest, (long)c.snapFail, (long)c.flips, (long)c.waits, (long)c.slotFull,
	            (long)c.repeats, (long)c.locFail, (long)c.goalUnlocated, (long)c.startUnlocated, (long)c.notSite,
	            (long)c.staleAdvance, (long)c.rung17, (long)c.ownedSkips, (long)c.noLocation,
	            (long)c.reissuedPlanned, (long)c.heldPlanned, (long)c.reissueRefused, (long)c.snapFar, (long)c.snapMax,
	            (long)c.waterFail, (long)c.waterGroups, CoarseProbeRefusals(), ws.writes, ws.leaves, (double)ws.last, ws.entries,
	            as.acidCells, as.unknown,
	            (long)c.arrSection, c.aimCount > 0 ? (double)c.aimShiftSum / (double)c.aimCount : 0.0,
	            (long)c.merges, (long)c.mergeJoins, (long)c.mergeAlone, (long)c.mergeMoved, (long)c.mergeWalkOff);
	LogMsg(line);
}

#ifdef KEO_DEBUG
static const int BORDER_LINES_MAX = 32;
static const int BORDER_OPP_MAX   = 6;
static int       s_borderLines    = 0;

static const char* SourceName(int source)
{
	return source == CG_LIVE ? "live" : (source == CG_SAVE ? "save" : "base");
}

// a's borders toward uid by the class the cross resolution gives them against nb: mirrored,
// one-sided, dropped.
static void ClassifyToward(const CgBlock* a, int uid, const CgBlock* nb, int counts[3])
{
	counts[0] = counts[1] = counts[2] = 0;
	for (int i = 0; i < a->borderCount; ++i)
		if (a->borders[i].oppUid == uid)
			++counts[CgClassifyBorder(a, a->borders[i], nb)];
}

static void Append(char* line, size_t size, size_t* len, const char* text)
{
	if (*len >= size)
		return;
	int w = _snprintf_s(line + *len, size - *len, _TRUNCATE, "%s", text);
	*len = w < 0 ? size : *len + (size_t)w;
}
#endif

// The goal block's borders, a run per opposite section (they sort by opposite uid), the first six
// shown: toward it, then from its current block back; "<-" when it has no block.
void PlannerReportBorders(int order, int goalDir)
{
#ifdef KEO_DEBUG
	if (s_borderLines > BORDER_LINES_MAX)
		return;
	if (s_borderLines == BORDER_LINES_MAX)
	{
		LogMsg("Planner borders: further plans counted, not printed");
		++s_borderLines;
		return;
	}
	CgView gv;
	if (!CgRead(goalDir, &gv))
		return;
	const CgBlock* a = gv.block;
	char line[512];
	char part[64];
	int head = _snprintf_s(line, sizeof(line), _TRUNCATE, "Planner borders: order=%d sec=%x src=%s nodes=%d borders=%d",
	                       order, (unsigned)a->uid, SourceName(a->source), a->nodeCount, a->borderCount);
	size_t len = head < 0 ? sizeof(line) : (size_t)head;
	int shown = 0, more = 0;
	for (int i = 0; i < a->borderCount; ++i)
	{
		int opp = a->borders[i].oppUid;
		if (i > 0 && a->borders[i - 1].oppUid == opp)
			continue;
		if (shown >= BORDER_OPP_MAX)
		{
			++more;
			continue;
		}
		++shown;
		int dir = CgIndexOfUid(opp);
		CgView ov;
		const CgBlock* nb = (dir >= 0 && CgRead(dir, &ov)) ? ov.block : NULL;
		int there[3], back[3];
		ClassifyToward(a, opp, nb, there);
		if (nb)
		{
			ClassifyToward(nb, a->uid, a, back);
			_snprintf_s(part, sizeof(part), _TRUNCATE, " %x=%d/%d/%d<%d/%d/%d", (unsigned)opp, there[0], there[1],
			            there[2], back[0], back[1], back[2]);
		}
		else
			_snprintf_s(part, sizeof(part), _TRUNCATE, " %x=%d/%d/%d<-", (unsigned)opp, there[0], there[1], there[2]);
		Append(line, sizeof(line), &len, part);
	}
	if (more)
	{
		_snprintf_s(part, sizeof(part), _TRUNCATE, " more=%d", more);
		Append(line, sizeof(line), &len, part);
	}
	LogMsg(line);
	++s_borderLines;
#endif
}

} // namespace planner
