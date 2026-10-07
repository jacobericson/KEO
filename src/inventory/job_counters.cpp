// job_counters.cpp - DEV diagnostics for player job trips: a pass-through post-hook on
// Task_EmptyMachine's haul-amount slot that counts the amounts player haulers are allowed, and the
// Jobs: heartbeat with the operator, food, dialogue and haul counters. The install step and the
// heartbeat run on the main thread; the post-hook runs in the AI back thread's task slot. A PROD
// build compiles both functions empty.
#include "inventory/job_counters.h"

#ifdef KEO_DEBUG

#include "inventory/operator_policy.h"
#include "inventory/operator_trips.h"
#include "inventory/backpack_food.h"
#include "plugin/hook_manifest.h"
#include "fixes/guard_report.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include <string>

namespace job_counters_detail {
typedef __int64 (__fastcall *haulAmount_t)(void* task, void* who, void* item, void* source);
typedef void*   (__fastcall *getter_t)(void* self);
} // namespace job_counters_detail
using namespace job_counters_detail;

namespace keo_inventory {

// RootObjectBase vtable +0x58 getFaction; Faction::isPlayer +0x250 (read from the IDB 2026-09-30).
static const size_t kVtGetFaction = 0x58, kFactionIsPlayer = 0x250;
static const int kBeatSeconds = 60;

static haulAmount_t      orig_haulAmount = NULL;
static volatile LONG     s_haul[HAUL_BUCKETS], s_haulCalls, s_haul5;
static LONGLONG          s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static __int64           s_lastTotal = -1;   // main thread: the counters' sum at the last line

static void* VGet(void* self, size_t slot)
{
	return ((getter_t)(*(void***)self)[slot / 8])(self);
}

// AI back thread (a task slot). Counts a player hauler's allowed amount; vanilla's answer
// unchanged. No lock, no allocation, no logging.
static __int64 __fastcall hook_haulAmount(void* task, void* who, void* item, void* source)
{
	const __int64 r = orig_haulAmount(task, who, item, source);
	void* faction = who ? VGet(who, kVtGetFaction) : NULL;
	if (!faction || !*(void* const*)((const char*)faction + kFactionIsPlayer))
		return r;
	InterlockedIncrement(&s_haulCalls);
	InterlockedIncrement(&s_haul[HaulBucket(r)]);
	if (r == 5)
		InterlockedIncrement(&s_haul5);
	return r;
}

static long ReadCounter(volatile LONG* p)
{
	return InterlockedCompareExchange(p, 0, 0);
}

void InstallJobCounters(int* installed, int*)
{
	if (!HookRowWanted(HOOK_HAUL_AMOUNT)) return;
	const char* why = HookInstall(HOOK_HAUL_AMOUNT, hook_haulAmount, &orig_haulAmount, installed, true);
	if (!why)
	{
		LogMsg("JobCounters: installed");
	}
	else
	{
		orig_haulAmount = NULL;
		ErrorLog(std::string("JobCounters: not installed (") + why + "); haul sizes are not counted");
	}
}

void OperatorTripsTick(double, bool)
{
	if (!s_qpf)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		s_qpf = f.QuadPart;
	}
	if (!GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds))
		return;

	long reasons[OR_COUNT];
	OperatorTripsCountersRead(reasons, OR_COUNT);
	long foodZeroed = 0, dialogCalls = 0, dialogBackpack = 0;
	BackpackFoodCountersRead(&foodZeroed, &dialogCalls, &dialogBackpack);
	const long hauls = ReadCounter(&s_haulCalls);
	const long haul5 = ReadCounter(&s_haul5);
	long hist[HAUL_BUCKETS];
	for (int i = 0; i < HAUL_BUCKETS; ++i)
		hist[i] = ReadCounter(&s_haul[i]);

	// The counters only rise, so an unchanged sum means nothing moved since the last line.
	__int64 total = (__int64)foodZeroed + dialogCalls + dialogBackpack + hauls + haul5;
	for (int i = 0; i < OR_COUNT; ++i)
		total += reasons[i];
	for (int i = 0; i < HAUL_BUCKETS; ++i)
		total += hist[i];
	if (total == s_lastTotal || (s_lastTotal < 0 && total == 0))
		return;
	s_lastTotal = total;

	// FixedLogBuf's 352 bytes would cut the haulHist= tail once the cumulative counts grow.
	FixedLogBufN<640> o;
	FlbInit(&o);
	FlbStr(&o, "Jobs:");
	for (int i = 0; i < OR_COUNT; ++i)
	{
		FlbChar(&o, ' ');
		FlbStr(&o, OperatorReasonName((OperatorReason)i));
		FlbChar(&o, '=');
		FlbDec(&o, reasons[i]);
	}
	FlbStr(&o, " foodZeroed=");     FlbDec(&o, foodZeroed);
	FlbStr(&o, " dialogCalls=");    FlbDec(&o, dialogCalls);
	FlbStr(&o, " dialogBackpack="); FlbDec(&o, dialogBackpack);
	FlbStr(&o, " hauls=");          FlbDec(&o, hauls);
	FlbStr(&o, " haul5=");          FlbDec(&o, haul5);
	FlbStr(&o, " haulHist=");
	for (int i = 0; i < HAUL_BUCKETS; ++i)
	{
		if (i)
			FlbChar(&o, ',');
		FlbDec(&o, hist[i]);
	}
	LogMsg(FlbDone(&o));
}

} // namespace keo_inventory

#else  // !KEO_DEBUG

namespace keo_inventory {
// PROD: no haul row exists and nothing logs a Jobs: line.
void InstallJobCounters(int*, int*) {}
void OperatorTripsTick(double, bool) {}
} // namespace keo_inventory

#endif // KEO_DEBUG
