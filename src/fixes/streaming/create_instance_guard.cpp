#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/create_instance_guard.h"
#include "fixes/streaming/create_instance_guard_policy.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <intrin.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

KLIB_ASSERT_OFFSET(NavMesh_generator, OFF_CI_NAVMESH_GENERATOR);
KLIB_ASSERT_OFFSET(NavMeshGenerator_done, OFF_CI_NMG_DONE_FRONT);
KLIB_ASSERT_OFFSET(NavMeshGenerator__Task_zone, OFF_CI_TASK_ZONE);
KLIB_ASSERT_OFFSET(NavMeshGenerator__Task_mesh, OFF_CI_TASK_MESH);
KLIB_ASSERT_OFFSET(NavMeshGenerator__Task_output, OFF_CI_TASK_OUTPUT);
KLIB_ASSERT_OFFSET(NavMeshGenerator__Task_flags, OFF_CI_TASK_FLAGS);
KLIB_ASSERT_OFFSET(NavInstance_instance, OFF_CI_NI_INSTANCE);
KLIB_ASSERT_OFFSET(NavInstance_uid, OFF_CI_NI_UID);
KLIB_ASSERT_OFFSET(ZoneMap_coordinates, OFF_CI_ZONE_COORDS);
static_assert(OFF_CI_NAVMESH_ADDLIST_COUNT == OFF_RDY_SM_PENDING_SECTIONS, "add-list count offset");
static_assert(OFF_CI_NMI_RUNTIME_ID == OFF_RDY_NMI_RUNTIME_INDEX, "instance runtime id offset");

// Every caller of createInstance runs inside NavMesh::update on the path
// thread, and every add-list writer runs there too; the function itself takes
// changeMutex exclusively around its own erase, so no caller holds it at
// entry. The scan below is a read on the only writing thread and takes no
// lock. A call from one of the three known return addresses is on that
// thread by construction and refreshes its id; an unknown caller is judged
// only on that thread, otherwise passed through unjudged and counted.
//
// calls == fromGen + fromZoneSector + fromZoneInterior + fromUnknown
// calls == passed + skipped
// passed == fresh + unjudged + selfNull + observed
// fired  == skipped + observed == selfLive + liveReg
//        == stitch + regen + genNoTask + zone + unknown
// stitch == stitchStale + stitchSame
// liveOutside >= liveReg
static volatile LONG s_calls       = 0;
static volatile LONG s_fromGen          = 0;
static volatile LONG s_fromZoneSector   = 0;
static volatile LONG s_fromZoneInterior = 0;
static volatile LONG s_fromUnknown      = 0;
static volatile LONG s_passed      = 0;
static volatile LONG s_skipped     = 0;
static volatile LONG s_fresh       = 0;  // not queued by pointer
static volatile LONG s_unjudged    = 0;
static volatile LONG s_offThread   = 0;  //   of which: not the path thread
static volatile LONG s_selfNull    = 0;  // queued by pointer with no instance; runs, and frees
static volatile LONG s_observed    = 0;  // would have skipped (observe mode)
static volatile LONG s_fired       = 0;
static volatile LONG s_selfLive    = 0;  //   n queued by pointer with a live instance
static volatile LONG s_liveReg     = 0;  //   n not queued, its instance already in the world
static volatile LONG s_stitch      = 0;  //   the drain was finishing a stitch task for n
static volatile LONG s_stitchStale = 0;  //     which stitched a mesh n no longer holds
static volatile LONG s_stitchSame  = 0;  //     which stitched n's current mesh
static volatile LONG s_regen       = 0;  //   a generation task (types 0-3)
static volatile LONG s_genNoTask   = 0;  //   the drain, but done.front was not n's task
static volatile LONG s_zone        = 0;  //   createZone
static volatile LONG s_unknown     = 0;  //   an unrecognised return address
static volatile LONG s_uidOther    = 0;  // info: another object with n's uid was queued
static volatile LONG s_liveOutside = 0;  // info: n had an instance but was not queued
static volatile LONG s_lastUid     = -1;

static volatile LONG s_pathTid   = 0;
static volatile LONG s_fireLines = 0;
static const LONG kMaxFireLines  = 32;

// 0 = not attempted, 1 = installed, -1 = refused.
static int s_state = 0;
static const char* s_why = "";
static double s_lastBeat = -1.0;
static const double kBeatSeconds = 60.0;

// Read once at install. true skips the two redundant calls; false counts them
// and calls the original unchanged.
static bool s_actMode = true;

typedef void (*createInstance_t)(void* navMesh, void* n);
static createInstance_t orig_createInstance = NULL;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// Havok.log prints uids as bare lowercase hex; matching it lets a fire line be
// found next to the log's own "already in add list" sequence.
static void FlbUid(FixedLogBuf* o, unsigned int v)
{
	char t[8];
	int n = 0;
	do { int d = (int)(v & 15); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
	while (v && n < 8);
	while (n)
		FlbChar(o, t[--n]);
}

static const GuardCounter kBeatRows[] =
{
	{ "calls",            GF_COUNT, &s_calls,            0 },
	{ "fromGen",          GF_COUNT, &s_fromGen,          0 },
	{ "fromZoneSector",   GF_COUNT, &s_fromZoneSector,   0 },
	{ "fromZoneInterior", GF_COUNT, &s_fromZoneInterior, 0 },
	{ "fromUnknown",      GF_COUNT, &s_fromUnknown,      0 },
	{ "passed",           GF_COUNT, &s_passed,           0 },
	{ "skipped",          GF_COUNT, &s_skipped,          0 },
	{ "fresh",            GF_COUNT, &s_fresh,            0 },
	{ "unjudged",         GF_COUNT, &s_unjudged,         0 },
	{ "offThread",        GF_COUNT, &s_offThread,        0 },
	{ "selfNull",         GF_COUNT, &s_selfNull,         0 },
	{ "observed",         GF_COUNT, &s_observed,         0 },
};

static const GuardCounter kFiredRows[] =
{
	{ "fired",       GF_COUNT,    &s_fired,       0 },
	{ "selfLive",    GF_COUNT,    &s_selfLive,    0 },
	{ "liveReg",     GF_COUNT,    &s_liveReg,     0 },
	{ "stitch",      GF_COUNT,    &s_stitch,      0 },
	{ "stitchStale", GF_COUNT,    &s_stitchStale, 0 },
	{ "stitchSame",  GF_COUNT,    &s_stitchSame,  0 },
	{ "regen",       GF_COUNT,    &s_regen,       0 },
	{ "genNoTask",   GF_COUNT,    &s_genNoTask,   0 },
	{ "zone",        GF_COUNT,    &s_zone,        0 },
	{ "unknown",     GF_COUNT,    &s_unknown,     0 },
	{ "uidOther",    GF_COUNT,    &s_uidOther,    0 },
	{ "liveOutside", GF_COUNT,    &s_liveOutside, 0 },
	{ "lines",       GF_COUNT_OF, &s_fireLines,   kMaxFireLines },
};

static void EmitHeartbeat()
{
	const bool live = s_state == 1;
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "CreateInstanceGuard running:");
	if (!live)
	{
		FlbStr(&o, " installed=no("); FlbStr(&o, s_why); FlbStr(&o, ")");
	}
	FlbStr(&o, " mode="); FlbStr(&o, s_actMode ? "guard" : "observe");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), live);
	LogMsgDeferrable(FlbDone(&o));

	// The attribution half, on its own line so neither overruns the buffer.
	GuardHeartbeatBegin(&o, "CreateInstanceGuard fired:");
	GuardFields(&o, kFiredRows, (int)ARRAYSIZE(kFiredRows), live);
	FlbStr(&o, " lastUid=");
	LONG lastUid = Read(&s_lastUid);
	if (!live || lastUid == -1)
		FlbChar(&o, '?');
	else
		FlbUid(&o, (unsigned int)lastUid);
	LogMsgDeferrable(FlbDone(&o));
}

void CreateInstanceGuardTick(double now)
{
	if (s_state == 0)
		return;
	if (now - s_lastBeat < kBeatSeconds)
		return;
	s_lastBeat = now;
	EmitHeartbeat();
}

static const char* CallerName(CreateInstanceCaller c)
{
	switch (c)
	{
	case CI_CALLER_GENERATOR:     return "generator";
	case CI_CALLER_ZONE_SECTOR:   return "createZone-sector";
	case CI_CALLER_ZONE_INTERIOR: return "createZone-interior";
	default:                      return "unknown";
	}
}

static const char* ArmName(CreateInstanceArm arm)
{
	switch (arm)
	{
	case CI_ARM_SELF_LIVE:       return "self-duplicate";
	case CI_ARM_LIVE_REGISTERED: return "registered";
	default:                     return "self-duplicate-null";
	}
}

static void EmitFireLine(const CreateInstanceCall* call, CreateInstanceCaller caller,
                         const CreateInstanceTask* task, const void* n, bool skipped)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "CreateInstanceGuard FIRED: ");
	FlbStr(&o, ArmName(call->arm));
	FlbStr(&o, " uid=");      FlbUid(&o, call->uid);
	FlbStr(&o, " n=");        FlbHex(&o, (unsigned __int64)n);
	FlbStr(&o, " instance="); FlbHex(&o, (unsigned __int64)call->instance);
	if (call->arm == CI_ARM_LIVE_REGISTERED)
	{
		FlbStr(&o, " slot="); FlbDec(&o, call->runtimeId);
	}
	FlbStr(&o, " index=");    FlbDec(&o, call->index);
	FlbStr(&o, "/");          FlbDec(&o, call->count);
	FlbStr(&o, " caller=");   FlbStr(&o, CallerName(caller));
	FlbStr(&o, " task=");
	if (task->have) FlbDec(&o, task->type); else FlbChar(&o, '?');
	FlbStr(&o, " mesh=");
	if (task->have) FlbStr(&o, task->meshSame ? "same" : "stale"); else FlbChar(&o, '?');
	FlbStr(&o, " zone=");
	if (task->haveZone)
	{
		FlbDec(&o, task->zoneX); FlbChar(&o, ','); FlbDec(&o, task->zoneY);
	}
	else
	{
		FlbChar(&o, '?');
	}
	if (call->arm == CI_ARM_SELF_NULL)
		FlbStr(&o, "; no instance to keep -- the original runs and frees it. fired=");
	else if (!skipped)
		FlbStr(&o, "; not acted on (observe mode) -- the original runs. fired=");
	else if (call->arm == CI_ARM_LIVE_REGISTERED)
		FlbStr(&o, "; already in the world -- skipped, n keeps its instance. fired=");
	else
		FlbStr(&o, "; already queued with a live instance -- skipped. fired=");
	FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " tid="); FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

static void CountFiredCaller(CreateInstanceCaller caller, const CreateInstanceTask* task)
{
	if (caller == CI_CALLER_GENERATOR)
	{
		if (!task->have)
			InterlockedIncrement(&s_genNoTask);
		else if (task->type == CI_TASK_TYPE_STITCH)
		{
			InterlockedIncrement(&s_stitch);
			InterlockedIncrement(task->meshSame ? &s_stitchSame : &s_stitchStale);
		}
		else
			InterlockedIncrement(&s_regen);
	}
	else if (caller == CI_CALLER_UNKNOWN)
		InterlockedIncrement(&s_unknown);
	else
		InterlockedIncrement(&s_zone);
}

static void hook_createInstance(void* navMesh, void* n)
{
	InterlockedIncrement(&s_calls);

	const size_t retRva = (size_t)((uintptr_t)_ReturnAddress() - gameBase);
	const CreateInstanceCaller caller = ClassifyCreateInstanceCaller(retRva);
	switch (caller)
	{
	case CI_CALLER_GENERATOR:     InterlockedIncrement(&s_fromGen); break;
	case CI_CALLER_ZONE_SECTOR:   InterlockedIncrement(&s_fromZoneSector); break;
	case CI_CALLER_ZONE_INTERIOR: InterlockedIncrement(&s_fromZoneInterior); break;
	default:                      InterlockedIncrement(&s_fromUnknown); break;
	}

	const LONG tid = (LONG)GetCurrentThreadId();
	if (caller != CI_CALLER_UNKNOWN)
		InterlockedExchange(&s_pathTid, tid);
	if (!CreateInstanceJudged(caller, (unsigned long)tid, (unsigned long)Read(&s_pathTid)))
	{
		InterlockedIncrement(&s_unjudged);
		InterlockedIncrement(&s_offThread);
		InterlockedIncrement(&s_passed);
		orig_createInstance(navMesh, n);
		return;
	}

	CreateInstanceCall call;
	InspectCreateInstanceCall(navMesh, n, &call);

	if (call.uidOther)    InterlockedIncrement(&s_uidOther);
	if (call.liveOutside) InterlockedIncrement(&s_liveOutside);

	if (call.arm == CI_ARM_NEW)
	{
		InterlockedIncrement(&s_fresh);
		InterlockedIncrement(&s_passed);
		orig_createInstance(navMesh, n);
		return;
	}
	if (call.arm == CI_ARM_UNJUDGED_ARGS || call.arm == CI_ARM_UNJUDGED_LIST)
	{
		InterlockedIncrement(&s_unjudged);
		InterlockedIncrement(&s_passed);
		orig_createInstance(navMesh, n);
		return;
	}

	// n is queued by pointer, or its instance is already in the world. Who
	// called is attribution only; the decision below rests on the scan alone.
	CreateInstanceTask task;
	if (caller == CI_CALLER_GENERATOR)
	{
		InspectCreateInstanceTask(navMesh, n, &task);
	}
	else
	{
		task.have = false; task.type = -1; task.meshSame = false;
		task.haveZone = false; task.zoneX = 0; task.zoneY = 0;
	}
	InterlockedExchange(&s_lastUid, (LONG)call.uid);

	if (call.arm == CI_ARM_SELF_NULL)
	{
		InterlockedIncrement(&s_selfNull);
		InterlockedIncrement(&s_passed);
		if (GuardFireClaim(&s_fireLines, kMaxFireLines))
			EmitFireLine(&call, caller, &task, n, false);
		orig_createInstance(navMesh, n);
		return;
	}

	InterlockedIncrement(&s_fired);
	InterlockedIncrement(call.arm == CI_ARM_LIVE_REGISTERED ? &s_liveReg : &s_selfLive);
	CountFiredCaller(caller, &task);

	const bool act = !CreateInstanceCallsOriginal(call.arm, s_actMode);
	if (act)
		InterlockedIncrement(&s_skipped);
	else
	{
		InterlockedIncrement(&s_observed);
		InterlockedIncrement(&s_passed);
	}

	if (GuardFireClaim(&s_fireLines, kMaxFireLines))
		EmitFireLine(&call, caller, &task, n, act);

	if (!act)
		orig_createInstance(navMesh, n);
}

void InstallCreateInstanceGuard(int* installed, int*)
{
	// The key chooses skip or observe, never whether the site is watched.
	s_actMode = fixes::g_fixesCfg.createInstanceGuardEnabled;

	const char* why = HookInstall(HOOK_NAVMESH_CREATE_INSTANCE, hook_createInstance,
			&orig_createInstance, installed, true);

	if (!why)
	{
		s_state = 1;
		LogMsg(std::string("Create-instance guard: installed, mode=")
		       + (s_actMode ? "guard" : "observe")
		       + " (a heartbeat line follows every minute)");
	}
	else
	{
		orig_createInstance = NULL;
		s_state = -1;
		s_why = why;
		LogError(std::string("Create-instance guard: not installed (") + why
		         + "); a NavInstance handed to createInstance while already queued is still freed and re-queued");
	}
	// The baseline line at zero calls; the tick carries on from here.
	EmitHeartbeat();
	s_lastBeat = ElapsedSec();
}
