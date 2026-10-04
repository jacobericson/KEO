// planner_merge.cpp - A run-together order's merge onto one route, the game side: the order capture's
// pass (the anchor, the biased searches, each member's join, the gather point and the Planner merge:
// line), the gather point and the members left alone that the formation reads, the re-plan once the
// group has gathered, and the tick's reading of a gather walk that left its biased prefix.
// Main thread only. The one game lock is the locator's try-shared world lock (+0x200); no mod lock.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_merge.h"
#include "planner/planner_tick_internal.h"
#include "planner/planner_search_ops.h"
#include "planner/plan_merge.h"
#include "planner/plan_store.h"
#include "planner/planner_config.h"
#include "planner/planner_tick.h"
#include "planner/planner_water.h"
#include "planner/coarse_graph.h"
#include "movement/formation.h"
#include "game/game.h"
#include "base/core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace planner {

static_assert(PLAN_MERGE_MEMBERS >= MAX_FORMATION_MEMBERS, "the order hook passes at most MAX_FORMATION_MEMBERS characters");

using namespace planner_tick_detail;

namespace planner_merge_detail {

const int    MERGE_RECORDS = 8;     // one per formation group slot
const int    MERGE_PREFIX  = 64;    // a joining member's biased prefix, sampled
const double MERGE_MATCH   = 1.0;   // the formation reads a record within this of its order

// One order merged in on: what the formation reads at its creation, and the tick's walk reading.
struct MergeRecord
{
	int           order;                      // the capture's order number; 0 for a free record
	double        time;
	int           n;
	int           anchor;
	uintptr_t     chars[PLAN_MERGE_MEMBERS];  // selection order
	int           join[PLAN_MERGE_MEMBERS];   // the anchor-route index; -1 alone; the anchor 0
	unsigned      prefix[PLAN_MERGE_MEMBERS][MERGE_PREFIX];
	int           prefixCount[PLAN_MERGE_MEMBERS];
	unsigned char walkOff[PLAN_MERGE_MEMBERS];
	float         gather[3];
	float         gatherDist;                 // the farthest gathering member's route length to gather; 0 at the anchor
	float         dest[3];                    // the order's destination, each member's prices and the order's
	float         m[PLAN_MERGE_MEMBERS];      // outdoors bit, for a member whose plan dropped while it gathered
	float         a[PLAN_MERGE_MEMBERS];
	int           orderOutdoors;
	int           resumed;                    // the group gathered: the walk reading stopped
};

} // namespace planner_merge_detail
using namespace planner_merge_detail;

// The records and the capture's scratch. Main thread.
static MergeRecord  s_records[MERGE_RECORDS];
static int          s_nextRecord = 0;
static CoarseRoute  s_anchorRoute;
static CoarseRoute  s_memberRoute;
static PlanMergeSet s_set;
static float        s_xz[PLAN_MERGE_MEMBERS][2];
static int          s_join[PLAN_MERGE_MEMBERS];
static int          s_detour[PLAN_MERGE_MEMBERS];
static unsigned     s_prefix[PLAN_MERGE_MEMBERS][MERGE_PREFIX];
static int          s_prefixCount[PLAN_MERGE_MEMBERS];

// Member k's own optimum, its biased search, re-cost and join; s_join[k] stays -1 (alone) when it
// cannot be located, has no route either way, or its detour is over the cap.
static void JoinMember(uintptr_t character, int k, const Located& goal, const float dest[3],
                       const PlanSearchParams& p, const CoarseGraphOps& inner)
{
	s_join[k] = -1;
	float pos[3];
	CharPos(character, pos);
	Located start;
	if (!Locate(pos, &start))
		return;
	const Built* own = SearchAndBuild(start, goal, dest, p);
	if (!own->found)
		return;
	float ownCost = own->cost;
	PlanMergeBias bias = { &inner, &s_set, (float)g_plannerCfg.mergeBias };
	CoarseGraphOps biased;
	PlanMergeBiasOps(&bias, &biased);
	if (CoarseSearch(biased, start.key, goal.key, COARSE_SCRATCH_MAX, SearchScratch(), &s_memberRoute) != CS_FOUND)
		return;
	float real = PlanMergeRecost(inner, s_memberRoute.nodes, s_memberRoute.count);
	s_detour[k] = PlanMergeDetourPercent(real, ownCost);
	int step = 0;
	int join = PlanMergeJoinIndex(s_anchorRoute.nodes, s_anchorRoute.count, s_set, s_memberRoute.nodes,
	                              s_memberRoute.count, &step);
	if (join < 0 || !PlanMergeJoins(real, ownCost, g_plannerCfg.mergeDetour))
		return;
	s_join[k] = join;
	s_prefixCount[k] = PlanMergeSample(s_memberRoute.nodes, step + 1, s_prefix[k], MERGE_PREFIX);
}

// The anchor's route, then each other member's join; false when the anchor has no route (every member
// then gathers at the anchor, as before).
static bool SearchJoins(const uintptr_t* chars, int n, int anchor, const float anchorPos[3], const Located& goal,
                        const float dest[3], const PlanSearchParams& p)
{
	PlanSearchParams params = p;
	CoarseGraphOps inner;
	AdapterOps(&params, &inner);
	Located start;
	if (!Locate(anchorPos, &start)
	    || CoarseSearch(inner, start.key, goal.key, COARSE_SCRATCH_MAX, SearchScratch(), &s_anchorRoute) != CS_FOUND)
		return false;
	PlanMergeSetFrom(s_anchorRoute.nodes, s_anchorRoute.count, &s_set);
	for (int k = 0; k < n; ++k)
		if (k != anchor)
			JoinMember(chars[k], k, goal, dest, params, inner);
	return true;
}

// The entry point of the anchor route's node j > 0: the portal crossed into it from another section,
// else its centre; out unchanged when neither resolves.
static void GatherPointAt(int j, float out[3])
{
	unsigned prev = s_anchorRoute.nodes[j - 1], node = s_anchorRoute.nodes[j];
	if (CgNodeDir(prev) != CgNodeDir(node))
	{
		CrossingPoint(prev, node, out);
		return;
	}
	float c[3];
	if (AdapterPosition(NULL, node, c))
		memcpy(out, c, sizeof(c));
}

// Adds the x-z length from *at to node's centre and moves *at there; a node that no longer resolves adds
// nothing.
static void StepXz(unsigned node, float at[2], float* len)
{
	float c[3];
	if (!AdapterPosition(NULL, node, c))
		return;
	float dx = c[0] - at[0], dz = c[2] - at[1];
	*len += sqrtf(dx * dx + dz * dz);
	at[0] = c[0];
	at[1] = c[2];
}

// The gather walk's length: over the anchor and each joining member, the largest x-z polyline from the
// member's position through its sampled biased prefix (ending on its join node), then the anchor route's
// node centres from the join index up to the gather index, ending at the gather point. Node positions
// are the centres AdapterPosition gives; 0 for a gather index of 0.
static float GatherDistance(int n, int anchor, int gatherIndex, const float gather[3])
{
	if (gatherIndex <= 0)
		return 0.0f;
	float best = 0.0f;
	for (int k = 0; k < n; ++k)
	{
		int join = (k == anchor) ? 0 : s_join[k];
		if (join < 0)
			continue;
		float at[2] = { s_xz[k][0], s_xz[k][1] };
		float len = 0.0f;
		for (int i = 0; k != anchor && i < s_prefixCount[k]; ++i)
			StepXz(s_prefix[k][i], at, &len);
		for (int j = join; j < gatherIndex; ++j)
			StepXz(s_anchorRoute.nodes[j], at, &len);
		float dx = gather[0] - at[0], dz = gather[2] - at[1];
		len += sqrtf(dx * dx + dz * dz);
		if (len > best)
			best = len;
	}
	return best;
}

static void Record(const uintptr_t* chars, int n, int anchor, const float gather[3], float gatherDist,
                   const float dest[3], const float* memberM, const float* memberA, int orderOutdoors, int order,
                   double now)
{
	MergeRecord& r = s_records[s_nextRecord];
	s_nextRecord = (s_nextRecord + 1) % MERGE_RECORDS;
	memset(&r, 0, sizeof(r));
	r.order = order;
	r.time = now;
	r.n = n;
	r.anchor = anchor;
	memcpy(r.gather, gather, sizeof(r.gather));
	r.gatherDist = gatherDist;
	memcpy(r.dest, dest, sizeof(r.dest));
	r.orderOutdoors = orderOutdoors;
	for (int k = 0; k < n; ++k)
	{
		r.chars[k] = chars[k];
		r.m[k] = k < PLAN_WATER_ORDER_MAX ? memberM[k] : 1.0f;
		r.a[k] = k < PLAN_WATER_ORDER_MAX ? memberA[k] : 1.0f;
		r.join[k] = s_join[k];
		r.prefixCount[k] = s_prefixCount[k];
		memcpy(r.prefix[k], s_prefix[k], sizeof(unsigned) * (size_t)s_prefixCount[k]);
	}
}

static void ReportMerge(int order, int mode, int anchor, float sum, const char* route, int n, int gatherIndex,
                        const float gather[3], double ms)
{
	char line[768];
	int len = _snprintf_s(line, sizeof(line), _TRUNCATE,
	                      "Planner merge: order=%d mode=%s anchor=%d sum=%.0f bias=%d cap=%d route=%s gather=%d"
	                      " at=(%.0f,%.0f) ms=%.2f members=",
	                      order, PlannerModeName(mode), anchor, sum, g_plannerCfg.mergeBias, g_plannerCfg.mergeDetour,
	                      route, gatherIndex, gather[0], gather[2], ms);
	for (int k = 0; k < n && len >= 0 && (size_t)len < sizeof(line); ++k)
	{
		int w;
		if (k == anchor)
			w = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d:anchor", k);
		else if (s_join[k] >= 0)
			w = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d:%d/%d%%", k, s_join[k], s_detour[k]);
		else
			w = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d:alone/%d%%", k, s_detour[k]);
		if (w < 0)
			break;
		len += w;
	}
	PlannerReportPlan(line);
}

int planner_tick_detail::MergeOrder(const uintptr_t* chars, int n, const Located& goal, const float dest[3],
                                    const PlanSearchParams& p, const float* memberM, const float* memberA,
                                    int orderOutdoors, bool formationForms, int order, double now)
{
	if (!formationForms || PlanStoreMode() == PLANNER_OFF || n < 2 || n > PLAN_MERGE_MEMBERS || !PlannerOrderRunTogether(chars, n))
		return 0;
	LONGLONG t0 = QpcNow();
	int mode = PlanStoreMode();
	for (int k = 0; k < n; ++k)
	{
		float pos[3];
		CharPos(chars[k], pos);
		s_xz[k][0] = pos[0];
		s_xz[k][1] = pos[2];
		s_join[k] = 0;
		s_detour[k] = 0;
		s_prefixCount[k] = 0;
	}
	float sum = 0.0f;
	int anchor = PlanMergeMedoid(s_xz, n, &sum);
	float gather[3];
	CharPos(chars[anchor], gather);
	bool searched = g_plannerCfg.mergeBias > 1 && SearchJoins(chars, n, anchor, gather, goal, dest, p);
	if (!searched)
		for (int k = 0; k < n; ++k)
			s_join[k] = 0;
	s_join[anchor] = 0;
	int gatherIndex = searched ? PlanMergeGatherIndex(s_join, n) : 0;
	if (gatherIndex > 0)
		GatherPointAt(gatherIndex, gather);
	PlannerCounters* c = PlannerCountersGet();
	InterlockedIncrement(&c->merges);
	for (int k = 0; searched && k < n; ++k)
		if (k != anchor)
			InterlockedIncrement(s_join[k] >= 0 ? &c->mergeJoins : &c->mergeAlone);
	if (mode == PLANNER_ON)
	{
		if (gatherIndex > 0)
			InterlockedIncrement(&c->mergeMoved);
		Record(chars, n, anchor, gather, GatherDistance(n, anchor, gatherIndex, gather), dest, memberM, memberA,
		       orderOutdoors, order, now);
	}
	ReportMerge(order, mode, anchor, sum, searched ? "searched" : (g_plannerCfg.mergeBias > 1 ? "none" : "unbiased"),
	            n, gatherIndex, gather, QpcToMs(QpcNow() - t0));
	return PlanMergeLeader(mode, anchor);
}

static bool InPlayerList(const uintptr_t* stuff, unsigned count, uintptr_t character)
{
	for (unsigned i = 0; i < count; ++i)
		if (stuff[i] == character)
			return true;
	return false;
}

// The nearest x-z distance from p to the centres of nodes[0..n); a node that no longer resolves is skipped.
static float NearestXz(const unsigned* nodes, int n, const float p[3])
{
	float best = -1.0f;
	for (int i = 0; i < n; ++i)
	{
		float c[3];
		if (!AdapterPosition(NULL, nodes[i], c))
			continue;
		float dx = c[0] - p[0], dz = c[2] - p[2];
		float d = sqrtf(dx * dx + dz * dz);
		if (best < 0.0f || d < best)
			best = d;
	}
	return best;
}

void planner_tick_detail::MergeTick(double now)
{
	if (PlanStoreMode() != PLANNER_ON)
		return;
	uintptr_t pi = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned count = pi ? GetPlayerCharCount(pi) : 0;
	uintptr_t* stuff = pi ? GetPlayerCharStuff(pi) : NULL;
	if (!stuff || count == 0 || count > 200)
		return;
	for (int i = 0; i < MERGE_RECORDS; ++i)
	{
		MergeRecord& r = s_records[i];
		if (!r.order || r.resumed || now - r.time > PLAN_HOLD_SECONDS)
			continue;
		for (int k = 0; k < r.n; ++k)
		{
			if (r.join[k] < 0 || r.walkOff[k] || r.prefixCount[k] == 0 || !InPlayerList(stuff, count, r.chars[k]))
				continue;
			float pos[3];
			CharPos(r.chars[k], pos);
			if (NearestXz(r.prefix[k], r.prefixCount[k], pos) > PLAN_MERGE_WALK_OFF)
			{
				r.walkOff[k] = 1;
				InterlockedIncrement(&PlannerCountersGet()->mergeWalkOff);
			}
		}
	}
}

bool PlannerMergeGather(const uintptr_t* chars, int n, float gather[3], unsigned char* alone, float* gatherDist)
{
	if (PlanStoreMode() != PLANNER_ON || !chars || n <= 0 || !gather || !alone || !gatherDist)
		return false;
	double now = ElapsedSec();
	const MergeRecord* best = NULL;
	for (int i = 0; i < MERGE_RECORDS; ++i)
	{
		const MergeRecord& r = s_records[i];
		if (r.order && now - r.time <= MERGE_MATCH && r.chars[r.anchor] == chars[0] && (!best || r.order > best->order))
			best = &r;
	}
	if (!best)
		return false;
	memcpy(gather, best->gather, sizeof(best->gather));
	*gatherDist = best->gatherDist;
	for (int m = 0; m < n; ++m)
	{
		alone[m] = 0;
		for (int k = 0; k < best->n; ++k)
		{
			if (best->chars[k] != chars[m])
				continue;
			alone[m] = best->join[k] < 0 ? 1 : 0;
			break;
		}
	}
	return true;
}

static bool Contains(const uintptr_t* chars, int n, uintptr_t character)
{
	for (int m = 0; m < n; ++m)
		if (chars[m] == character)
			return true;
	return false;
}

// The newest unresumed record holding character whose anchor is one of the group's gathered members
// chars[0..n) (a record of an earlier order whose group has gone names no live anchor here); *k its
// index there. NULL when none.
static const MergeRecord* GroupRecordOf(const uintptr_t* chars, int n, uintptr_t character, int* kOut)
{
	const MergeRecord* best = NULL;
	for (int i = 0; i < MERGE_RECORDS; ++i)
	{
		const MergeRecord& r = s_records[i];
		if (!r.order || r.resumed || (best && r.order < best->order) || !Contains(chars, n, r.chars[r.anchor]))
			continue;
		for (int k = 0; k < r.n; ++k)
			if (r.chars[k] == character)
			{
				best = &r;
				*kOut = k;
				break;
			}
	}
	return best;
}

void PlannerResumeFromGather(const uintptr_t* chars, int n, double now)
{
	if (PlanStoreMode() != PLANNER_ON || !chars || n <= 0)
		return;
	TakeSnapshot();
	ClearMemo();
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		int slot = cm ? PlanStoreFind(cm) : -1;
		PlanView v;
		memset(&v, 0, sizeof(v));
		bool haveSlot = slot >= 0 && PlanStoreRead(slot, &v) && v.cm == cm;
		int rk = -1;
		const MergeRecord* r = cm ? GroupRecordOf(chars, n, chars[k], &rk) : NULL;
		int from = PlanMergeResumeFrom(r != NULL, r != NULL && r->join[rk] < 0, haveSlot,
		                               haveSlot && PlanStoreMain(slot)->haveHold);
		if (from == PMR_NONE)
			continue;
		const float* dest = (from == PMR_PLAN) ? v.finalDest : r->dest;
		PlanSearchParams p;
		p.m = (from == PMR_PLAN) ? v.waterMult : r->m[rk];
		p.a = (from == PMR_PLAN) ? v.acidMult : r->a[rk];
		int outdoors = (from == PMR_PLAN) ? v.orderOutdoors : r->orderOutdoors;
		float pos[3];
		CharPos(chars[k], pos);
		Located start, goal;
		if (!Locate(dest, &goal) || !Locate(pos, &start))
			continue;
		const Built* b = SearchAndBuild(start, goal, dest, p);
		int verdict = PV_NONE;
		WritePlan(cm, pos, goal, dest, *b, now, &verdict, 0, p, outdoors);
	}
	for (int i = 0; i < MERGE_RECORDS; ++i)
		for (int k = 0; s_records[i].order && k < s_records[i].n; ++k)
			for (int m = 0; m < n; ++m)
				if (s_records[i].chars[k] == chars[m])
					s_records[i].resumed = 1;
}

} // namespace planner
