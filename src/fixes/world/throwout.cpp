// throwout.cpp - The throw-out fix's drop side: the getDropped post-hook, the two task-slot
// detours, the clock, the main-thread tick with its heartbeat, and the install.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/throwout.h"
#include "fixes/world/throwout_internal.h"
#include "fixes/world/throwout_hold.h"
#include "fixes/world/throwout_policy.h"
#include "fixes/guard_report.h"
#include "game/game.h"
#include "game/hand_key.h"
#include "base/core.h"
#include "base/config.h"
#include "navmesh/nm_workers.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <string.h>
#include <string>
#include <sstream>
#include "base/klib_include.h"
#include <Debug.h>
#include <kenshi/RootObject.h>
#include "base/klib_include_end.h"

namespace throwout_detail {
// GameWorld::getTimeStamp_inGameHours's result: one double, in-game hours.
struct TimeOfDayPod { double time; };
} // namespace throwout_detail
using namespace throwout_detail;

namespace fixes {

typedef void (__fastcall *getDropped_t)(void* body, bool ragdollHim, bool hull);
typedef void (__fastcall *takeOutsideTick_t)(void* task, void* carrier);
typedef char (__fastcall *takeOutsidePathImpossible_t)(void* task, void* body);
typedef int  (__fastcall *gateCodeAt_t)(void* gates, const float* pos);
// A member returning a struct: `this` in rcx (unread), the result written through rdx.
typedef TimeOfDayPod* (__fastcall *gwTimestampHours_t)(void* gameWorld, TimeOfDayPod* out);
typedef void* (__fastcall *handGetCharacter_t)(const ThrowoutHand* hand);
typedef const void* (__fastcall *isIndoors_t)(void* character);
typedef void (__fastcall *setInsideTownWalls_t)(void* character, int code);
typedef bool (__fastcall *isUnconcious_t)(void* character);

static getDropped_t                orig_getDropped                = NULL;
static takeOutsideTick_t           orig_takeOutsideTick           = NULL;
static takeOutsidePathImpossible_t orig_takeOutsidePathImpossible = NULL;
static gateCodeAt_t                fn_gateCodeAt                  = NULL;
static gwTimestampHours_t          fn_gwTimestampHours            = NULL;
static handGetCharacter_t          fn_handGetCharacter            = NULL;

// The character virtuals the fix calls, by vtable offset.
static const size_t kIsIndoorsSlot       = 0x1D8;
static const size_t kSetInsideWallsSlot  = 0x120;
static const size_t kIsUnconciousSlot    = 0x30;

// The two callees the drop side calls without a manifest row, checked at install.
static const unsigned char kGateCodeAtBytes[10] =
	{ 0x83,0x79,0x58,0x00,0x74,0x04,0x83,0xC8,0xFF,0xC3 };
static const unsigned char kTimestampBytes[21] =
	{ 0x48,0x8B,0x05,0x39,0x32,0xAC,0x01,0x48,0x8B,0x88,0xA0,0x00,0x00,0x00,0x48,0x8B,0xC2,0x48,0x89,0x0A,0xC3 };

// Set by a throw-out task slot around its original, read by the getDropped post-hook that runs
// inside it on the same thread.
static __declspec(thread) int   t_reason = TR_NONE;
static __declspec(thread) void* t_tickTask = NULL;
static __declspec(thread) int   t_townTask = 0;   // the slot's task is Task_TakeIntruderOutsideTown

static volatile LONG s_drops          = 0;
static volatile LONG s_flag0          = 0;  // a written code 0: outside
static volatile LONG s_flag1          = 0;  // a written code above 0: inside
static volatile LONG s_unresolved     = 0;
static volatile LONG s_indoors        = 0;
static volatile LONG s_noNavmesh      = 0;  // not asked: no gates singleton, or the navmesh absent or stopped
static volatile LONG s_reachedOut     = 0;
static volatile LONG s_reachedIn      = 0;
static volatile LONG s_timeout        = 0;
static volatile LONG s_pathImpossible = 0;
static volatile LONG s_otherTask      = 0;  // a slot drop by the building task: no hold
static volatile LONG s_holds          = 0;
static volatile LONG s_holdLost       = 0;
static volatile LONG s_clearMissed    = 0;
static volatile LONG s_woke           = 0;
static volatile LONG s_cap            = 0;
static volatile LONG s_gone           = 0;
static volatile LONG s_stale          = 0;  // an expiry further ahead than the cap: released
static volatile LONG s_installed      = 0;  // rows in

static const GuardCounter kThrowoutRows[] =
{
	{ "drops",          GF_COUNT, &s_drops,          0 },
	{ "flag0",          GF_COUNT, &s_flag0,          0 },
	{ "flag1",          GF_COUNT, &s_flag1,          0 },
	{ "unresolved",     GF_COUNT, &s_unresolved,     0 },
	{ "indoors",        GF_COUNT, &s_indoors,        0 },
	{ "noNavmesh",      GF_COUNT, &s_noNavmesh,      0 },
	{ "reachedOut",     GF_COUNT, &s_reachedOut,     0 },
	{ "reachedIn",      GF_COUNT, &s_reachedIn,      0 },
	{ "timeout",        GF_COUNT, &s_timeout,        0 },
	{ "pathImpossible", GF_COUNT, &s_pathImpossible, 0 },
	{ "otherTask",      GF_COUNT, &s_otherTask,      0 },
	{ "holds",          GF_COUNT, &s_holds,          0 },
	{ "holdLost",       GF_COUNT, &s_holdLost,       0 },
	{ "clearMissed",    GF_COUNT, &s_clearMissed,    0 },
	{ "woke",           GF_COUNT, &s_woke,           0 },
	{ "cap",            GF_COUNT, &s_cap,            0 },
	{ "gone",           GF_COUNT, &s_gone,           0 },
	{ "stale",          GF_COUNT, &s_stale,          0 },
};

// Main-thread tick state.
static bool   s_modeDecided = false;
static bool   s_chain       = false;
static bool   s_wasLoading  = false;
static double s_nextScan    = 0.0;
static double s_nextBeat    = 0.0;
static const double kScanSeconds = 1.0;
static const double kBeatSeconds = 60.0;

static const float* BodyPos(void* body)
{
	return &((RootObjectBase*)body)->pos.x;
}

// isIndoors() names the building a character is in; a null handle is outdoors.
static bool BodyIndoors(void* body)
{
	const isIndoors_t isIndoors = *(const isIndoors_t*)(*(char**)body + kIsIndoorsSlot);
	return !game::HandKeyIsNull(game::HandKeyFromHand(isIndoors(body)));
}

static void SetInsideWalls(void* body, int code)
{
	const setInsideTownWalls_t set = *(const setInsideTownWalls_t*)(*(char**)body + kSetInsideWallsSlot);
	set(body, code);
}

static bool IsUnconcious(void* character)
{
	const isUnconcious_t ko = *(const isUnconcious_t*)(*(char**)character + kIsUnconciousSlot);
	return ko(character);
}

static void CountFlag(ThrowoutFlag flag, int code)
{
	if (flag == TF_INDOORS)
		InterlockedIncrement(&s_indoors);
	else if (flag == TF_UNRESOLVED)
		InterlockedIncrement(&s_unresolved);
	else
		InterlockedIncrement(code == 0 ? &s_flag0 : &s_flag1);
}

static void CountDrop(ThrowoutDrop drop)
{
	switch (drop)
	{
	case TD_REACHED_OUTSIDE: InterlockedIncrement(&s_reachedOut);     break;
	case TD_REACHED_INSIDE:  InterlockedIncrement(&s_reachedIn);      break;
	case TD_TIMEOUT:         InterlockedIncrement(&s_timeout);        break;
	case TD_PATH_IMPOSSIBLE: InterlockedIncrement(&s_pathImpossible); break;
	default:                 break;
	}
}

double ThrowoutNowHours()
{
	TimeOfDayPod t;
	fn_gwTimestampHours(GameAddr(RVA_GLOBAL_GAMEWORLD), &t);
	return t.time;
}

// Both slots also serve the building throw-out task; the town task is told apart by its
// vftable, which a task object carries at offset 0.
static int IsTownTask(void* task)
{
	return (task && *(void* const*)task == GameAddr(RVA_TAKE_OUTSIDE_TOWN_VFTABLE)) ? 1 : 0;
}

// The carrier's update thread. Marks the drop the original makes as the tick slot's.
static void __fastcall hook_takeOutsideTick(void* task, void* carrier)
{
	const int r = t_reason;
	void* const k = t_tickTask;
	const int town = t_townTask;
	t_reason = TR_TICK;
	t_tickTask = task;
	t_townTask = IsTownTask(task);
	orig_takeOutsideTick(task, carrier);
	t_reason = r;
	t_tickTask = k;
	t_townTask = town;
}

// The carrier's update thread. Marks the drop the original makes as a path-impossible one.
static char __fastcall hook_takeOutsidePathImpossible(void* task, void* body)
{
	const int r = t_reason;
	void* const k = t_tickTask;
	const int town = t_townTask;
	t_reason = TR_PATH_IMPOSSIBLE;
	t_tickTask = NULL;
	t_townTask = IsTownTask(task);
	const char result = orig_takeOutsidePathImpossible(task, body);
	t_reason = r;
	t_tickTask = k;
	t_townTask = town;
	return result;
}

// The body's update thread. The original first, as vanilla; then the walls flag from the gate
// code at the body's position (never -1), asked only while the navmesh exists and has not been
// stopped; then, for the town throw-out task's drop, the hold.
static void __fastcall hook_getDropped(void* body, bool ragdollHim, bool hull)
{
	orig_getDropped(body, ragdollHim, hull);
	if (!body)
		return;
	InterlockedIncrement(&s_drops);
	const bool indoors = BodyIndoors(body);   // isIndoors() (vt+0x1D8) names a non-null handle
	void* gates = *(void**)GameAddr(RVA_GLOBAL_GATES);
	const bool stopped = NavMeshStopSeen();
	void* navmesh = *(void**)GameAddr(RVA_GLOBAL_SECTION_MGR);
	int code = -1;
	if (ThrowoutMayAskGateCode(indoors, gates != NULL, navmesh != NULL, stopped))
		code = fn_gateCodeAt(gates, BodyPos(body));   // RootObjectBase::pos
	else if (!indoors)
		InterlockedIncrement(&s_noNavmesh);
	const ThrowoutFlag flag = ThrowoutFlagDecide(indoors, code);
	if (flag == TF_WRITE)
		SetInsideWalls(body, code);              // setInsideTownWalls, vt+0x120
	CountFlag(flag, code);
	if (t_reason == TR_NONE)
		return;
	if (!ThrowoutTownDrop(t_reason, t_townTask != 0))
	{
		InterlockedIncrement(&s_otherTask);
		return;
	}
	const double now = ThrowoutNowHours();
	const bool timedOut = t_reason == TR_TICK && t_tickTask
	                   && ThrowoutTimedOut(*(const double*)((const char*)t_tickTask + 0x78), now);
	CountDrop(ThrowoutClassifyDrop(t_reason, timedOut, flag, code));
	ThrowoutHand hand;
	memcpy(hand.bytes, (const char*)body + KLIB_OFF_RootObjectBase_handle, sizeof(hand.bytes));
	if (ThrowoutHoldAdd(game::HandKeyOfObject(body), hand, ThrowoutHoldExpiry(now, g_fixesCfg.throwOutHoldMinutes)))
		InterlockedIncrement(&s_holds);
	else
		InterlockedIncrement(&s_holdLost);
}

// Main thread, once a minute: the rows, the mode, the live entries, then every counter.
static void EmitHeartbeat()
{
	FixedLogBufN<768> o;
	GuardHeartbeatBegin(&o, "Throwout:");
	FlbStr(&o, " installed=");
	FlbDec(&o, InterlockedCompareExchange(&s_installed, 0, 0));
	FlbStr(&o, "/4 chain=");
	FlbDec(&o, s_chain ? 1 : 0);
	FlbStr(&o, " live=");
	FlbDec(&o, ThrowoutHoldLive());
	GuardFields(&o, kThrowoutRows, (int)ARRAYSIZE(kThrowoutRows), true);
	int finderRows = 0;
	const GuardCounter* finder = ThrowoutFinderCounterRows(&finderRows);
	GuardFields(&o, finder, finderRows, true);
	LogMsg(FlbDone(&o));
}

void ThrowoutTick(double now, bool saveLoading)
{
	if (!g_fixesCfg.throwOutFixEnabled) return;
	if (!s_modeDecided)
	{
		// After every plugin's startPlugin: one loaded later is already present here.
		s_modeDecided = true;
		const bool other = GetModuleHandleA("KenshiQOL.dll") != NULL;
		s_chain = ThrowoutFinderModeFor(other) == TFM_CHAIN;
		if (s_chain)
		{
			const bool live = ThrowoutFinderDecideFallback();
			ThrowoutFinderChainOriginal(true);
			LogMsg(live ? "Throwout: finder mode=chain (another plugin hooks the finder; a foreign detour below is live)"
			            : "Throwout: finder mode=chain (another plugin hooks the finder; a foreign detour below is not live)");
		}
		else
		{
			ThrowoutFinderChainOriginal(false);
			LogMsg("Throwout: finder mode=replace");
		}
	}
	if (saveLoading && !s_wasLoading)
		InterlockedExchangeAdd(&s_clearMissed, ThrowoutHoldClear());
	s_wasLoading = saveLoading;
	if (saveLoading)
		return;

	if (now >= s_nextScan && fn_handGetCharacter)
	{
		s_nextScan = now + kScanSeconds;
		// The finder asks only about unconscious bodies, so a wake is seen here. The copy is
		// 8-aligned by its type and passed to the game as the hand.
		ThrowoutHand h;
		game::HandKey k;
		double e;
		const double cap = g_fixesCfg.throwOutHoldMinutes / 60.0;
		for (int i = 0; i < THROWOUT_HOLD_SLOTS; ++i)
		{
			if (!ThrowoutHoldRead(i, &k, &h, &e))
				continue;
			void* c = fn_handGetCharacter(&h);
			if (!c)
			{
				if (ThrowoutHoldRelease(i, k, e))
					InterlockedIncrement(&s_gone);
				continue;
			}
			const ThrowoutHold d = ThrowoutHoldDecide(ThrowoutNowHours(), e, IsUnconcious(c), cap);
			if (d == TH_WOKE && ThrowoutHoldRelease(i, k, e))
				InterlockedIncrement(&s_woke);
			else if (d == TH_CAP && ThrowoutHoldRelease(i, k, e))
				InterlockedIncrement(&s_cap);
			else if (d == TH_STALE && ThrowoutHoldRelease(i, k, e))
				InterlockedIncrement(&s_stale);
		}
	}

	if (now >= s_nextBeat)
	{
		s_nextBeat = now + kBeatSeconds;
		EmitHeartbeat();
	}
}

// Main thread, at install. IsTownTask compares a task's vftable with the town task's; that
// vftable's path-impossible (+0x28) and tick (+0x38) entries must reach the two slot rows'
// functions, directly or through one E9 thunk, or the slot detours would mark the wrong task.
static bool TownTaskSlotsResolve()
{
	const unsigned char* vft = (const unsigned char*)GameAddr(RVA_TAKE_OUTSIDE_TOWN_VFTABLE);
	unsigned __int64 e5 = 0, e7 = 0;
	unsigned char b5[5], b7[5];
	if (!ThrowoutSafeRead(vft + 0x28, &e5, sizeof(e5)) || !ThrowoutSafeRead(vft + 0x38, &e7, sizeof(e7))
	 || !ThrowoutSafeRead((const void*)(uintptr_t)e5, b5, sizeof(b5))
	 || !ThrowoutSafeRead((const void*)(uintptr_t)e7, b7, sizeof(b7)))
		return false;
	return ThrowoutSlotResolves(b5, e5, (unsigned __int64)(uintptr_t)GameAddr(RVA_TAKE_OUTSIDE_PATH_IMPOSSIBLE))
	    && ThrowoutSlotResolves(b7, e7, (unsigned __int64)(uintptr_t)GameAddr(RVA_TAKE_OUTSIDE_TICK));
}

void InstallThrowout(int* installed, int*)
{
	if (!HookRowWanted(HOOK_CHAR_GET_DROPPED)) return;

	const char* why = NULL;
	if (memcmp((const void*)GameAddr(RVA_GATE_CODE_AT), kGateCodeAtBytes, sizeof(kGateCodeAtBytes)) != 0)
		why = "bytes: gate code";
	else if (memcmp((const void*)GameAddr(RVA_GW_TIMESTAMP_HOURS), kTimestampBytes, sizeof(kTimestampBytes)) != 0)
		why = "bytes: getTimeStamp_inGameHours";
	else
	{
		fn_gateCodeAt = (gateCodeAt_t)GameAddr(RVA_GATE_CODE_AT);
		fn_gwTimestampHours = (gwTimestampHours_t)GameAddr(RVA_GW_TIMESTAMP_HOURS);
		if ((const void*)KlibRealAddress(&hand::getCharacter) != GameAddr(RVA_HAND_GET_CHARACTER))
			why = "addresses";
		else
			fn_handGetCharacter = (handGetCharacter_t)GameAddr(RVA_HAND_GET_CHARACTER);
	}
	// The finder first: its row reads the clock resolved above. Its mode is set before the row
	// goes in, so no call reaches it undecided; the tick's first call settles it.
	if (!why)
	{
		ThrowoutFinderChainOriginal(ThrowoutFinderModeBeforeTick() == TFM_CHAIN);
		why = ThrowoutFinderInstall(installed);
	}
	if (!why)
		why = HookInstall(HOOK_CHAR_GET_DROPPED, hook_getDropped, &orig_getDropped, installed, true);
	if (!why && !TownTaskSlotsResolve())
		why = "bytes: town task vftable";
	if (!why)
		why = HookInstall(HOOK_TAKE_OUTSIDE_TICK, hook_takeOutsideTick, &orig_takeOutsideTick, installed, true);
	if (!why)
		why = HookInstall(HOOK_TAKE_OUTSIDE_PATH_IMPOSSIBLE, hook_takeOutsidePathImpossible,
		                  &orig_takeOutsidePathImpossible, installed, true);

	LONG rows = 0;
	rows += HookRowInstalled(HOOK_CHAR_GET_DROPPED) ? 1 : 0;
	rows += HookRowInstalled(HOOK_TAKE_OUTSIDE_TICK) ? 1 : 0;
	rows += HookRowInstalled(HOOK_TAKE_OUTSIDE_PATH_IMPOSSIBLE) ? 1 : 0;
	rows += HookRowInstalled(HOOK_FIND_KO_INTRUDER_TOWN) ? 1 : 0;
	InterlockedExchange(&s_installed, rows);

	if (!why)
	{
		std::ostringstream ss;
		ss << "Throwout: hooks installed (4/4, hold " << g_fixesCfg.throwOutHoldMinutes << " in-game minutes)";
		LogMsg(ss.str());
		return;
	}
	std::ostringstream ss;
	ss << "Throwout: not installed (" << why << "; " << rows
	   << "/4 rows in); the rows already in stay in and the rest run as vanilla";
	ErrorLog(ss.str());
}

} // namespace fixes
