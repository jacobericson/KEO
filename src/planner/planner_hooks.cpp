// planner_hooks.cpp - The route planner's two detours, the flip read, the install step and the banner token.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_hooks.h"
#include "planner/plan_store.h"
#include "planner/planner_config.h"
#include "planner/coarse_graph_base.h"
#include "planner/coarse_graph.h"
#include "pathfind/astar_hier_policy.h"
#include "base/config_values.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <sstream>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

namespace planner_hooks_detail {
// The call setDestination_Vec3 is running on this thread. Written and read only by this thread.
struct PlannerTls
{
	uintptr_t cm;          // the CharMovement*, 0 outside a call
	float     dest[3];     // the call's destination, copied at entry
	int       haveDest;
	int       slot;        // the character's plan slot, -1 when unplanned or unarmed
	int       islandSeen;  // isInIsland ran inside this call
	int       flipped;     // the flip answered false (on) or would have (observe)
	int       edgeSeen;    // getZoneEdge ran inside this call
};
// The engine's snap filter, built on the stack as the engine builds it on its heap.
struct DoorHitFilterBytes { const void* vtable; unsigned refAndFlags; unsigned pad; };
} // namespace planner_hooks_detail
using namespace planner_hooks_detail;

namespace planner {

static __declspec(thread) PlannerTls t_plan;

typedef void   (*setDestinationVec3_t)(void* cm, const float* dest, int priority, bool notVertical);
typedef float* (*getZoneEdge_t)(void* nm, float* retstr, const float* start, const float* target, float offset);
typedef int    (*getClosestPoint_t)(void* nm, const float* point, float radius, float inset,
                                    void* filter, float* out, unsigned* key);
static setDestinationVec3_t orig_setDestinationVec3 = NULL;
static getZoneEdge_t        orig_getZoneEdge        = NULL;
static getClosestPoint_t    fn_getClosestPoint      = NULL;
static const void*          s_doorFilterVtable      = NULL;
static uintptr_t            s_retRecheck = 0, s_retCompute = 0;   // gameBase + the two RVAs
static volatile LONG        s_armed = 0;                         // set last by the install step
static int                  s_mode  = PLANNER_OFF;
static int                  s_legSpan = 2;                       // plannerLegSpan, captured at install
static int                  s_advanceSection = 0;                // plannerAdvanceSection, captured at install
static int                  s_legAim = 0;                        // plannerLegAim, captured at install
static const char*          s_refusedWhy = NULL;                 // a literal: the refusal's reason

// AI back thread (the main thread with characterMultithreading off). Publishes the character and a
// copy of its destination for this call and restores the previous value on return; no lock, no
// allocation, no logging.
static void hook_setDestinationVec3(void* cm, const float* dest, int priority, bool notVertical)
{
	PlannerTls saved = t_plan;
	t_plan.cm = (uintptr_t)cm;
	t_plan.haveDest = dest != NULL;
	if (dest) { t_plan.dest[0] = dest[0]; t_plan.dest[1] = dest[1]; t_plan.dest[2] = dest[2]; }
	t_plan.slot = InterlockedCompareExchange(&s_armed, 0, 0) ? PlanStoreFind((uintptr_t)cm) : -1;
	t_plan.islandSeen = t_plan.flipped = t_plan.edgeSeen = 0;
	if (t_plan.slot >= 0 && cm && *(const int*)((const char*)cm + OFF_CMOV_EDGE_COUNTER) >= 17)
		InterlockedIncrement(&PlannerCountersGet()->rung17);
	orig_setDestinationVec3(cm, dest, priority, notVertical);
	if (t_plan.slot >= 0)
	{
		if (!t_plan.islandSeen)
			InterlockedIncrement(&PlannerCountersGet()->roadPreempt);
		else if (t_plan.flipped && !t_plan.edgeSeen)
			InterlockedIncrement(&PlannerCountersGet()->staleRerequest);
	}
	t_plan = saved;
}

// AI back thread, after the original computed its point. The planner's point for a planned
// character's own edge-mode call in on; the original's point otherwise. Lock-free (the snap's
// try-shared +0x200 is the engine's own, and it never blocks), allocation-free, no logging.
// Every step past the pass writes the slot's waiting word, so an advance clears it.
static float* hook_getZoneEdge(void* nm, float* retstr, const float* start, const float* target, float offset)
{
	float* r = orig_getZoneEdge(nm, retstr, start, target, offset);
	PlannerTls& t = t_plan;
	if (t.slot < 0 || !start || (uintptr_t)start - OFF_CMOV_POS != t.cm)
		return r;
	t.edgeSeen = 1;
	PlanStoreNoteConsulted(t.slot);
	uintptr_t ra = (uintptr_t)_ReturnAddress();
	int site = ra == s_retRecheck ? PES_RECHECK : (ra == s_retCompute ? PES_COMPUTE : PES_OTHER);
	if (site == PES_OTHER) { InterlockedIncrement(&PlannerCountersGet()->notSite); return r; }
	PlanView v;
	if (!PlanStoreRead(t.slot, &v) || v.cm != t.cm)
		return r;
	if (!PlanEdgeSteers(s_mode, v.verdict, t.haveDest && PlanDestIsPlans(t.dest, v.finalDest, v.resend, v.resendCount)))
		return r;
	PlanEdgeIn in;
	in.site = site; in.offset = offset; in.pos[0] = start[0]; in.pos[1] = start[1]; in.pos[2] = start[2];
	in.legIndex = v.legIndex; in.loadedMask = v.loadedMask; in.legSpan = s_legSpan;
	in.routeTruncated = v.routeTruncated;
	in.advanceSection = s_advanceSection; in.aim = s_legAim; in.exteriorSlots = CG_EXTERIOR_SLOTS;
	in.holdInteriorPortal = v.holdInteriorPortal;
	PlanEdgeOut o;
	PlanEdgeStep(v.legs, v.legCount, in, &o);
	if (o.action == PEA_PASS) return r;
	if (o.aimed && site == PES_COMPUTE)
	{
		InterlockedIncrement(&PlannerCountersGet()->aimCount);
		InterlockedExchangeAdd(&PlannerCountersGet()->aimShiftSum, (LONG)(o.aimShift + 0.5f));
	}
	if (o.newLegIndex != v.legIndex)
	{
		if (!PlanStoreAdvance(t.slot, v.epoch, v.legIndex, o.newLegIndex)) return r;
		InterlockedIncrement(&PlannerCountersGet()->arrivals);
		if (o.bySection) InterlockedIncrement(&PlannerCountersGet()->arrSection);
		PlanStoreNoteArrival(t.slot, v.epoch);
	}
	PlanStoreSetWaiting(t.slot, v.epoch, o.waiting);
	if (o.rung) { PlanStoreAddRung(t.slot, v.epoch); InterlockedIncrement(&PlannerCountersGet()->rungs); }
	DoorHitFilterBytes f = { s_doorFilterVtable, 0xFFFF0001u, 0 };
	float snapped[3];
	unsigned key = 0;
	if (fn_getClosestPoint(nm, o.point, 300.0f, 5.0f, &f, snapped, &key) == 1)
	{
		retstr[0] = snapped[0]; retstr[1] = snapped[1]; retstr[2] = snapped[2];
		PlannerNoteSnap(PlanSnapDistance(o.point, snapped));
	}
	else
	{
		retstr[0] = o.point[0]; retstr[1] = o.point[1]; retstr[2] = o.point[2];
		InterlockedIncrement(&PlannerCountersGet()->snapFail);
	}
	return retstr;
}

// Any thread, from the island hook inside the entry detour's call: reads only this thread's
// published call and the slot through the lock-free read.
PlanFlipAnswer PlannerIslandVerdict()
{
	if (!InterlockedCompareExchange(&s_armed, 0, 0)) return PFA_NOT_MINE;
	PlannerTls& t = t_plan;
	t.islandSeen = 1;
	PlanFlipIn in;
	memset(&in, 0, sizeof(in));
	in.mode = s_mode;
	in.haveChar = t.cm != 0;
	PlanView v;
	if (t.cm && t.slot >= 0 && t.haveDest && PlanStoreRead(t.slot, &v) && v.cm == t.cm
	    && v.legIndex >= 0 && v.legIndex < v.legCount)
	{
		in.haveSlot = 1;
		in.verdict = v.verdict;
		in.legIsDestination = v.legs[v.legIndex].isDestination;
		memcpy(in.dest, t.dest, sizeof(in.dest));
		memcpy(in.finalDest, v.finalDest, sizeof(in.finalDest));
		memcpy(in.resend, v.resend, sizeof(in.resend));
		in.resendCount = v.resendCount;
	}
	if (PlanFlipRuleOn(in) == PFA_FALSE) { t.flipped = 1; InterlockedIncrement(&PlannerCountersGet()->flips); }
	return PlanFlipRule(in);
}

// Main thread, startPlugin, once. The arm flag is set only after both rows installed, so a
// partial install leaves both detours passing through.
void InstallPlannerHooks(int* installed, int*)
{
	if (!HookRowWanted(HOOK_SET_DESTINATION_VEC3)) return;
	PlanArm arm = PlanArmDecide(g_plannerCfg.mode, pathfind::g_pathfindCfg.playerHierarchicalMode == AHIER_ON);
	if (arm == PLAN_ARM_REFUSE_PREREQ)
	{
		g_plannerCfg.mode = PLANNER_OFF;
		s_refusedWhy = "playerHierarchical";
		ErrorLog("Planner: refused to arm (plannerMode=on needs playerHierarchical=on); the planner is off for this session");
		return;
	}
	if (arm != PLAN_ARM_GO) return;

	fn_getClosestPoint = (getClosestPoint_t)GameAddr(RVA_NAVMESH_GET_CLOSEST_POINT);
	s_doorFilterVtable = GameAddr(RVA_DOOR_HIT_FILTER_VTABLE);
	s_retRecheck = (uintptr_t)GameAddr(RVA_SETDEST_RET_EDGE_RECHECK);
	s_retCompute = (uintptr_t)GameAddr(RVA_SETDEST_RET_EDGE_COMPUTE);
	s_mode = g_plannerCfg.mode;
	s_legSpan = g_plannerCfg.legSpan;
	s_advanceSection = g_plannerCfg.advanceSection;
	s_legAim = g_plannerCfg.legAim;

	HookRowId failed = HOOK_ROW_COUNT;
	const char* why = HookInstall(HOOK_SET_DESTINATION_VEC3, hook_setDestinationVec3, &orig_setDestinationVec3, installed, true);
	if (why)
		failed = HOOK_SET_DESTINATION_VEC3;
	else
	{
		why = HookInstall(HOOK_GET_ZONE_EDGE, hook_getZoneEdge, &orig_getZoneEdge, installed, true);
		if (why)
			failed = HOOK_GET_ZONE_EDGE;
	}
	if (failed != HOOK_ROW_COUNT)
	{
		g_plannerCfg.mode = PLANNER_OFF;
		s_refusedWhy = g_hookPrologues[failed].name;
		ErrorLog(std::string("Planner: ") + s_refusedWhy + " not installed (" + why
		         + "); the planner is off for this session");
		return;
	}
	InterlockedExchange(&s_armed, 1);
	LogMsg(std::string("Planner: hooks installed (mode=") + PlannerModeName(s_mode) + ")");
}

// Main thread, the startup banner. The mode is the one captured when the hooks installed; a later
// start-step clear (a failed arm) leaves it stale, and the arm line's mode= is the truth.
std::string PlannerBannerToken()
{
	int tiles = 0, total = 0;
	PlannerBaseProgress(&tiles, &total);
	int hooks = (HookRowInstalled(HOOK_SET_DESTINATION_VEC3) ? 1 : 0) + (HookRowInstalled(HOOK_GET_ZONE_EDGE) ? 1 : 0);
	std::ostringstream os;
	if (s_refusedWhy)
		os << "refused(" << s_refusedWhy << ")";
	else
		os << PlannerModeName(s_mode);
	os << " base=" << tiles << "/" << total << " hooks=" << hooks << "/2";
	return os.str();
}

} // namespace planner
