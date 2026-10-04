// planner_water.cpp - The per-character water multiplier's game side: the prologue-checked bindings
// of CharStats::calculateSwimSpeed and Character::getWaterLevel, each member's reads, the order's
// pre-pass, and the per-player-character water table's refresh and order amend. Main thread only (the
// order capture, the planner's tick and the arm); no lock, no allocation, no log. The two engine
// readers write nothing and take no lock; the engine's own GUI calls them on this thread.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_water.h"
#include "planner/plan_policy.h"
#include "planner/plan_store.h"
#include "planner/planner_config.h"
#include "planner/planner_water_table.h"
#include "plugin/hook_manifest.h"
#include "game/game.h"
#include "base/core.h"
#include "movement/islands.h"
#include "movement/formation.h"
#include <float.h>
#include <string.h>

namespace planner {

static_assert(PLAN_WATER_ORDER_MAX >= MAX_FORMATION_MEMBERS, "the order hook passes at most MAX_FORMATION_MEMBERS characters");

typedef float (*calculateSwimSpeed_t)(void* stats);
typedef int   (*getWaterLevel_t)(void* character);

static const unsigned char kSwimSpeedPrologue[13] =
	{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9 };
static const unsigned char kWaterLevelPrologue[13] =
	{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x83, 0xB9, 0xF8, 0x02, 0x00, 0x00, 0x00 };

// Written by the arm, then read by the main thread alone.
static calculateSwimSpeed_t fn_calculateSwimSpeed = NULL;
static getWaterLevel_t      fn_getWaterLevel      = NULL;
static int                  s_waterMode           = PWC_FLOOR;
static const char*          s_bindToken           = "unarmed";
// The water mode as configured, before the coherence rule. Main thread.
static int s_configuredMode = PWC_FLOOR;
// The request write's state for the arm line. Main thread.
static const char* s_engineToken = "unarmed";
// The main thread's copy of the published water table. Main thread.
static PlanWaterEntry s_local[PLAN_WATER_TABLE_MAX];
// The copy's entry count. Main thread.
static int s_localCount = 0;

// The order pre-pass's scratch, one entry per member. Main thread.
static PlanWaterInputs      s_members[PLAN_WATER_ORDER_MAX];

bool PlannerWaterArm(int mode)
{
	bool pathRow = HookRowInstalled(HOOK_REQUEST_PATH);
	bool submitRow = HookRowInstalled(HOOK_PATH_REQ_SUBMIT);
	bool keyOn = g_plannerCfg.waterEngine == PWE_MATCH;
	bool on = g_plannerCfg.mode == PLANNER_ON;
	int live = (keyOn && on && pathRow && submitRow) ? 1 : 0;
	s_engineToken = live ? "match" : (!keyOn ? "off" : (!on ? "off(observe)"
	              : (!pathRow ? "refused(requestPath)" : "refused(pathReqSubmit)")));
	s_configuredMode = mode;
	s_waterMode = PlanWaterEffectiveMode(mode, live);
	PlannerWaterTableArm(live, s_waterMode);
	const void* swim = GameAddr(RVA_CHARSTATS_CALC_SWIM_SPEED);
	const void* level = GameAddr(RVA_CHARACTER_GET_WATER_LEVEL);
	bool swimOk = memcmp(swim, kSwimSpeedPrologue, sizeof(kSwimSpeedPrologue)) == 0;
	bool levelOk = memcmp(level, kWaterLevelPrologue, sizeof(kWaterLevelPrologue)) == 0;
	fn_calculateSwimSpeed = swimOk ? (calculateSwimSpeed_t)swim : NULL;
	fn_getWaterLevel = levelOk ? (getWaterLevel_t)level : NULL;
	s_bindToken = !swimOk ? "refused(calculateSwimSpeed)" : (!levelOk ? "refused(getWaterLevel)" : "ok");
	return swimOk && levelOk;
}

const char* PlannerWaterBindToken()
{
	return s_bindToken;
}

static bool PositiveFinite(float v)
{
	return _finite(v) != 0 && v > 0.0f;
}

static uintptr_t MovementOf(uintptr_t character)
{
	return character ? *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT)) : 0;
}

static int SpeedModeOf(uintptr_t cm)
{
	return *(int*)(KLIB_MEMBER(3, cm, AbstractMovementBase_speedOrders, OFF_CMOV_SPEED_MODE));
}

// The order's speed bound from the movement's speed mode: WALK its walk speed, JOG the engine's 55,
// RUN and GROUPED none.
static float SpeedCapOf(uintptr_t cm)
{
	int speed = SpeedModeOf(cm);
	if (speed == 0)
		return *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_walkSpeed, OFF_CMOV_WALK_SPEED));
	return speed == 1 ? PLAN_JOG_SPEED : 0.0f;
}

// One member's inputs. The engine value is its HavokCharacter's (1 without one). In engine mode
// nothing else is read; otherwise the land speed is read only while the character is dry, and the
// swim speed only for a race that swims. readOk stays 0 when a reader is unbound, the race or stats
// are missing, the character is in water, or a speed is not finite and positive.
static void ReadMember(uintptr_t character, uintptr_t cm, PlanWaterInputs* in)
{
	memset(in, 0, sizeof(*in));
	in->mode = s_waterMode;
	uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
	in->engineValue = hc ? *(float*)(KLIB_MEMBER(3, hc, HavokCharacter_waterModifier, OFF_HC_WATER_MODIFIER)) : 1.0f;
	if (s_waterMode == PWC_ENGINE)
		return;
	in->speedCap = SpeedCapOf(cm);
	uintptr_t race = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_myRace, OFF_CHAR_RACE));
	uintptr_t stats = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_stats, OFF_CHAR_STATS));
	if (!race || !stats || !fn_calculateSwimSpeed || !fn_getWaterLevel)
		return;
	if (fn_getWaterLevel((void*)character) != 0)
		return;
	in->landSpeed = *(float*)(KLIB_MEMBER(3, stats, CharStats_moveSpeed, OFF_STATS_MOVE_SPEED));
	in->swims = *(bool*)(KLIB_MEMBER(3, race, RaceData_swims, OFF_RACE_SWIMS)) ? 1 : 0;
	in->raceWalkSpeed = *(float*)(KLIB_MEMBER(3, race, RaceData_walkSpeed, OFF_RACE_WALK_SPEED));
	in->waterSpeed = in->swims ? fn_calculateSwimSpeed((void*)stats) : 0.0f;
	bool speedsOk = PositiveFinite(in->landSpeed)
	             && (in->swims ? PositiveFinite(in->waterSpeed) : PositiveFinite(in->raceWalkSpeed));
	in->readOk = speedsOk ? 1 : 0;
}

// The formation's test: more than one member, and every member's speed mode GROUPED.
static bool RunTogether(const uintptr_t* chars, int n)
{
	if (n <= 1)
		return false;
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm || SpeedModeOf(cm) != MOVESPEED_GROUPED)
			return false;
	}
	return true;
}

// The main thread's last published value for a character, 0 when it has none.
static float LocalFind(uintptr_t hc)
{
	for (int i = 0; i < s_localCount; ++i)
		if (s_local[i].havokChar == hc)
			return s_local[i].mult;
	return 0.0f;
}

static uintptr_t HavokOf(uintptr_t cm)
{
	return *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
}

// A character's multiplier for its requests: its plan's while it holds one, else its own from a
// read made now, else (the read failed: in water, or a reader unbound) its last good value.
static float CharacterWater(uintptr_t character, uintptr_t cm, uintptr_t hc)
{
	PlanView v;
	int slot = PlanStoreFind(cm);
	if (slot >= 0 && PlanStoreRead(slot, &v))
		return v.waterMult;
	PlanWaterInputs in;
	ReadMember(character, cm, &in);
	if (in.readOk || s_waterMode == PWC_ENGINE)
		return PlanWaterMultiplier(in);
	return LocalFind(hc);
}

// The order's values into the main thread's copy (amended, or appended while there is room), then
// one publish, so the order's own first request already carries them.
static void PublishOrder(const uintptr_t* chars, int n, const float* mult)
{
	if (!PlannerWaterTableLive())
		return;
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		uintptr_t hc = cm ? HavokOf(cm) : 0;
		if (!hc)
			continue;
		int i = 0;
		while (i < s_localCount && s_local[i].havokChar != hc)
			++i;
		if (i == s_localCount)
		{
			if (s_localCount == PLAN_WATER_TABLE_MAX)
				continue;
			s_local[s_localCount++].havokChar = hc;
		}
		s_local[i].mult = mult[k];
	}
	PlannerWaterTablePublish(s_local, s_localCount);
}

// Main thread, from the order capture before the original runs; takes no lock and allocates
// nothing. Calls Character::getWaterLevel and CharStats::calculateSwimSpeed for each member read. In
// a run-together order an unconscious or carried member is left out of the speeds (its engine value
// still counts) and is no failure; any other member whose speed read failed counts one waterFail.
// Engine mode reads no speeds and counts no waterFail. Off reads nothing and gives every member 1.
int PlannerOrderWater(const uintptr_t* chars, int n, float* mult)
{
	if (!chars || !mult || n <= 0)
		return 0;
	if (n > PLAN_WATER_ORDER_MAX)
		n = PLAN_WATER_ORDER_MAX;
	if (s_waterMode == PWC_OFF)
	{
		for (int k = 0; k < n; ++k)
			mult[k] = 1.0f;
		return 0;
	}
	bool grouped = RunTogether(chars, n);
	PlannerCounters* c = PlannerCountersGet();
	for (int k = 0; k < n; ++k)
	{
		PlanWaterInputs& in = s_members[k];
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm)
		{
			memset(&in, 0, sizeof(in));
			in.mode = s_waterMode;
			continue;
		}
		bool skipped = grouped && (IslandK7IsUnconcious(chars[k])
		                           || *(bool*)(KLIB_MEMBER(3, chars[k], Character__isBeingCarried, 0x3D4)));
		ReadMember(chars[k], cm, &in);
		if (skipped)
			in.readOk = 0;
		else if (!in.readOk && s_waterMode != PWC_ENGINE)
			InterlockedIncrement(&c->waterFail);
	}
	if (grouped)
	{
		float m = PlanWaterGroupMultiplier(s_members, n);
		for (int k = 0; k < n; ++k)
			mult[k] = m;
		InterlockedIncrement(&c->waterGroups);
		PublishOrder(chars, n, mult);
		return 1;
	}
	for (int k = 0; k < n; ++k)
		mult[k] = PlanWaterMultiplier(s_members[k]);
	PublishOrder(chars, n, mult);
	return 0;
}

void PlannerWaterRefresh()
{
	if (!PlannerWaterTableLive())
		return;
	PlanWaterEntry next[PLAN_WATER_TABLE_MAX];
	int n = 0;
	uintptr_t pi = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned count = pi ? GetPlayerCharCount(pi) : 0;
	uintptr_t* stuff = pi ? GetPlayerCharStuff(pi) : NULL;
	if (stuff && count <= (unsigned)PLAN_WATER_TABLE_MAX)
	{
		for (unsigned i = 0; i < count; ++i)
		{
			uintptr_t cm = MovementOf(stuff[i]);
			uintptr_t hc = cm ? HavokOf(cm) : 0;
			if (!hc)
				continue;
			next[n].havokChar = hc;
			next[n].mult = CharacterWater(stuff[i], cm, hc);
			++n;
		}
	}
	memcpy(s_local, next, sizeof(next[0]) * n);
	s_localCount = n;
	PlannerWaterTablePublish(s_local, s_localCount);
}

void PlannerWaterReset()
{
	s_localCount = 0;
	PlannerWaterTableClear();
}

const char* PlannerWaterModeToken()
{
	return s_waterMode != s_configuredMode ? "floor(dynamic)" : PlanWaterModeName(s_waterMode);
}

const char* PlannerWaterEngineToken()
{
	return s_engineToken;
}

} // namespace planner
