// planner_prearrival.cpp - The pre-arrival leg request. A pre-call detour on CharMovement::update runs,
// a look-ahead before the portal a planned character walks to, the recheck the engine's parked call
// would run there, and makes the path request the engine would make after it, while the character
// still walks, so the result can land and be spliced in before the old path ends.
// The detour runs on the AI back thread (the main thread with characterMultithreading off), the only
// thread touching the character during the AI window: no mod lock, no allocation, no logging, counters
// interlocked; its planner writes are compare-exchanges at the epoch it read, and its two engine writes
// (edgeTarget, pathDestination) and the request follow a successful advance. Its engine calls are the
// snap (the engine's own try-shared +0x200, never blocking) and requestPath through its hooked entry.
// The install, the frame step and the tokens run on the main thread.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_prearrival.h"
#include "planner/plan_store.h"
#include "planner/plan_policy.h"
#include "planner/planner_config.h"
#include "planner/coarse_graph.h"
#include "movement/char_movement_fields.h"
#include "plugin/hook_manifest.h"
#include "base/clock.h"
#include "base/core.h"
#include "game/game.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <sstream>

namespace planner_prearrival_detail {
// The engine's snap filter, built on the stack as the engine builds it on its heap.
struct DoorHitFilterBytes { const void* vtable; unsigned refAndFlags; unsigned pad; };
} // namespace planner_prearrival_detail
using namespace planner_prearrival_detail;

namespace planner {

typedef void (*charMovementUpdate_t)(void* cm, float dt);
typedef void (*requestPath_t)(void* havokChar, float* destination, int priority);
typedef int  (*getClosestPoint_t)(void* nm, const float* point, float radius, float inset,
                                  void* filter, float* out, unsigned* key);

static const int   PRE_PRIORITY = 2;       // the priority the engine gives a player character's request
static const float PRE_SAME_SQ  = 40.0f;   // the engine's no-op: an answer within sqrt(40) of pathDestination
static const float SNAP_RADIUS  = 300.0f;
static const float SNAP_INSET   = 5.0f;

static charMovementUpdate_t orig_charMovementUpdate = NULL;
static requestPath_t        fn_requestPath          = NULL;
static getClosestPoint_t    fn_getClosestPoint      = NULL;
static const void*          s_doorFilterVtable      = NULL;
static void* const*         s_navmeshGlobal         = NULL;   // &pauseState.navmesh
static const float*         s_gameSpeedField        = NULL;   // &GameWorld::frameSpeedMult; main thread only
static volatile LONG        s_gameSpeedBits         = 0;      // its value, published every frame
static volatile LONG        s_armed                 = 0;      // set last by the install step
static int                  s_latencyMs             = 0;      // plannerPreArrivalMs, captured at install
static int                  s_legSpan               = 2;      // plannerLegSpan, captured at install
static int                  s_legAim                = 0;      // plannerLegAim, captured at install
static int                  s_armState              = 0;      // 0 off, 1 off(observe), 2 armed, 3 refused
static const char*          s_bannerToken           = "off";  // a literal

// ---- The AI back thread ---------------------------------------------------------------------------

// The game speed the frame step published; 0 before the first frame.
static float GameSpeed()
{
	LONG bits = s_gameSpeedBits;
	float s;
	memcpy(&s, &bits, sizeof(s));
	return s;
}

// RoadPathBuilder::isValid, restated as the read it is: the state word set and neither 3 nor 4.
static bool RoadFollowerValid(uintptr_t rf)
{
	int state = *(const int*)(rf + OFF_ROAD_FOLLOWER_STATE);
	return state != 0 && state != 3 && state != 4;
}

static void RaiseMax(volatile LONG* field, LONG v)
{
	LONG cur = InterlockedCompareExchange(field, 0, 0);
	while (v > cur)
	{
		LONG seen = InterlockedCompareExchange(field, v, cur);
		if (seen == cur)
			break;
		cur = seen;
	}
}

// The in-flight word against this frame's states (PlanPreResolve). True when the step may go on (the
// word cleared), false while it stays.
static bool ResolveFlight(int slot, const PlanPreFlight& f, int ps, int cs, const float* pos)
{
	unsigned epoch = 0;
	int leg = -1;
	bool have = PlanStoreLeg(slot, &epoch, &leg);
	PlanPreResolveIn in;
	in.state = f.state;
	in.epochHolds = have && epoch == f.epoch;
	in.legIsTo = have && leg == f.to;
	in.pathState = ps;
	in.characterState = cs;
	in.age = QpcToMs(QpcNow() - f.issueQpc) / 1000.0;
	PlanPreResolveOut o;
	PlanPreResolve(in, &o);
	PlannerCounters* c = PlannerCountersGet();
	if (o.count == PPC_LAND) InterlockedIncrement(&c->preLand);
	else if (o.count == PPC_LATE) InterlockedIncrement(&c->preLate);
	else if (o.count == PPC_BROKEN) InterlockedIncrement(&c->preBroken);
	else if (o.count == PPC_FAILED) InterlockedIncrement(&c->preFailed);
	else if (o.count == PPC_LOST) InterlockedIncrement(&c->preLost);
	if (o.sample)
	{
		float dx = pos[0] - f.issueX, dz = pos[2] - f.issueZ;
		LONG d = (LONG)(sqrtf(dx * dx + dz * dz) + 0.5f);
		InterlockedIncrement(&c->preDCount);
		InterlockedExchangeAdd(&c->preDSum, d);
		RaiseMax(&c->preDMax, d);
	}
	if (o.stepBack)
		PlanStoreAdvance(slot, f.epoch, f.to, f.from);
	if (o.keep)
	{
		if (o.newState != f.state)
			PlanStorePreUpdate(slot, f, o.newState);
		return false;
	}
	PlanStorePreUpdate(slot, f, PLAN_PRE_NONE);
	return true;
}

// The step before the engine's update, in this order: the cheap field tests, the slot, the word's
// resolution, the fire test (walking with a complete path, within L of pathDestination), the plan's
// view, the decision, the snap, the advance, the two writes, the request, the word. A failing test
// returns with nothing written; a skip marks its leg tried, but a snap the navmesh lock refused marks
// nothing, so the next frame tries again.
static void PreArrivalStep(uintptr_t cm, float dt)
{
	if (!cm || *(const unsigned char*)KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE) == 0)
		return;
	if (*(const int*)KLIB_MEMBER(3, cm, CharMovement_movementMode, OFF_CMOV_MOVEMENT_MODE) != 0)
		return;
	if (*(const unsigned char*)KLIB_MEMBER(3, cm, CharMovement_animationOverride, OFF_CMOV_ANIMATION_OVERRIDE) != 0)
		return;
	uintptr_t rf = *(const uintptr_t*)KLIB_MEMBER(3, cm, AbstractMovementBase_roadFollower, OFF_CMOV_ROAD_FOLLOWER);
	if (rf && RoadFollowerValid(rf))
		return;
	uintptr_t hc = *(const uintptr_t*)KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR);
	if (!hc)
		return;
	int slot = PlanStoreFind(cm);
	if (slot < 0)
		return;
	int ps = *(const int*)KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE);
	int cs = *(const int*)KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL);
	const float* pos = (const float*)KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS);
	PlanPreFlight f;
	if (PlanStorePreRead(slot, &f) && !ResolveFlight(slot, f, ps, cs, pos))
		return;
	if (cs != PLAN_CHAR_FOLLOWING || ps != PLAN_PATH_COMPLETE)
		return;
	float* pd = (float*)KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST);
	float reach = PlanPreArrivalReach(*(const float*)KLIB_MEMBER(3, hc, HavokCharacter_desiredSpeed, OFF_HC_DESIRED_SPEED),
	                                  *(const float*)KLIB_MEMBER(3, hc, HavokCharacter_acceleration, OFF_HC_ACCELERATION),
	                                  GameSpeed(), s_latencyMs, dt);
	float dx = pos[0] - pd[0], dz = pos[2] - pd[2];
	if (!(dx * dx + dz * dz < reach * reach))
		return;
	PlanView v;
	if (!PlanStoreRead(slot, &v) || v.cm != cm || v.verdict != PV_LEGGED || v.legIndex < 0 || v.legIndex >= v.legCount)
		return;
	// The destination is read whole, height included, so the test is the three-dimensional one the
	// getZoneEdge detour steers by: the pre-call advances only a character that detour would steer.
	// The x-z test is for a destination read without its height.
	const float* dest = (const float*)KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST);
	if (!PlanDestIsPlans(dest, v.finalDest, v.resend, v.resendCount) || PlanStorePreBlocked(slot, v.epoch, v.legIndex))
		return;
	PlanEdgeIn in;
	memset(&in, 0, sizeof(in));
	in.site = PES_RECHECK;
	in.pos[0] = pd[0]; in.pos[1] = pd[1]; in.pos[2] = pd[2];
	in.legIndex = v.legIndex; in.loadedMask = v.loadedMask; in.legSpan = s_legSpan;
	in.routeTruncated = v.routeTruncated; in.aim = s_legAim; in.exteriorSlots = CG_EXTERIOR_SLOTS;
	in.holdInteriorPortal = v.holdInteriorPortal;
	PlanPreOut o;
	PlanPreArrival(v.legs, v.legCount, in, &o);
	if (o.skip == PPS_NOT_MINE)
		return;
	if (o.skip != PPS_NONE)
	{
		PlanStorePreSkip(slot, v.epoch, v.legIndex, o.skip);
		return;
	}
	void* nm = s_navmeshGlobal ? *s_navmeshGlobal : NULL;
	DoorHitFilterBytes filter = { s_doorFilterVtable, 0xFFFF0001u, 0 };
	float snapped[3];
	unsigned key = 0;
	// No navmesh yet answers as the lock's refusal: counted in preBusy and tried again next frame.
	int got = nm ? fn_getClosestPoint(nm, o.point, SNAP_RADIUS, SNAP_INSET, &filter, snapped, &key) : -1;
	if (PlanStorePreSnap(slot, v.epoch, v.legIndex, got) != PLAN_PRE_SNAP_HIT)
		return;
	float sx = snapped[0] - pd[0], sy = snapped[1] - pd[1], sz = snapped[2] - pd[2];
	if (sx * sx + sy * sy + sz * sz <= PRE_SAME_SQ)
	{
		PlanStorePreSkip(slot, v.epoch, v.legIndex, PPS_SAME);
		return;
	}
	if (!PlanStoreAdvance(slot, v.epoch, v.legIndex, o.target))
		return;
	PlanStoreSetWaiting(slot, v.epoch, 0);
	PlanStoreNoteArrival(slot, v.epoch);
	InterlockedIncrement(&PlannerCountersGet()->arrivals);
	InterlockedIncrement(&PlannerCountersGet()->pre);
	*(int*)KLIB_MEMBER(3, cm, CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER) = 0;
	pd[0] = snapped[0]; pd[1] = snapped[1]; pd[2] = snapped[2];
	fn_requestPath((void*)hc, pd, PRE_PRIORITY);
	PlanStorePreIssue(slot, v.epoch, v.legIndex, o.target, pos[0], pos[2], QpcNow());
}

// Before the engine's update of this character: the pre-arrival step, then the original exactly once
// with its arguments.
static void hook_charMovementUpdate(void* cm, float dt)
{
	if (s_armed)
		PreArrivalStep((uintptr_t)cm, dt);
	orig_charMovementUpdate(cm, dt);
}

// ---- The main thread ------------------------------------------------------------------------------

void InstallPlannerPreArrival(int* installed)
{
	if (!HookRowWanted(HOOK_CHARMOVEMENT_UPDATE))
	{
		s_armState = g_plannerCfg.mode == PLANNER_OBSERVE ? 1 : 0;
		return;
	}
	s_latencyMs = g_plannerCfg.preArrivalMs;
	s_legSpan = g_plannerCfg.legSpan;
	s_legAim = g_plannerCfg.legAim;
	fn_requestPath = (requestPath_t)GameAddr(RVA_REQUEST_PATH);
	fn_getClosestPoint = (getClosestPoint_t)GameAddr(RVA_NAVMESH_GET_CLOSEST_POINT);
	s_doorFilterVtable = GameAddr(RVA_DOOR_HIT_FILTER_VTABLE);
	s_navmeshGlobal = (void* const*)GameAddr(RVA_GLOBAL_SECTION_MGR);
	s_gameSpeedField = (const float*)((const char*)GameAddr(RVA_GLOBAL_GAMEWORLD) + OFF_GAMEWORLD_FRAME_SPEED_MULT);
	const char* why = HookInstall(HOOK_CHARMOVEMENT_UPDATE, hook_charMovementUpdate, &orig_charMovementUpdate,
	                              installed, true);
	if (why)
	{
		s_armState = 3;
		s_bannerToken = "refused";
		LogError(std::string("Planner: charMovementUpdate not installed (") + why
		         + "); pre-arrival is off for this session");
		return;
	}
	PlanStoreSetPreHold((LONGLONG)(PLAN_PRE_HOLD_SECONDS * (double)qpcFrequency.QuadPart));
	s_armState = 2;
	s_bannerToken = "on";
	InterlockedExchange(&s_armed, 1);
}

void PlannerPreArrivalFrame()
{
	if (!s_armed)
		return;
	float s = *s_gameSpeedField;
	LONG bits;
	memcpy(&bits, &s, sizeof(bits));
	InterlockedExchange(&s_gameSpeedBits, bits);
}

std::string PlannerPreArrivalArmToken()
{
	if (s_armState == 3) return "refused(charMovementUpdate)";
	if (s_armState == 1) return "off(observe)";
	if (s_armState != 2) return "off";
	std::ostringstream os;
	os << s_latencyMs << "ms";
	return os.str();
}

const char* PlannerPreArrivalBannerToken()
{
	return s_bannerToken;
}

} // namespace planner
