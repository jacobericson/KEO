#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/search/cluster_cross_cost.h"
#include "fixes/search/cluster_cross_cost_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"
#include "base/config_values.h"
#include "planner/coarse_graph_live.h"

// The collection's graph-instance connect copies each cross-tile link's stored cost into the
// new instance's owned edges and their reciprocals in the neighbours. That stored cost is the
// distance between the two clusters' tile-local positions, about one tile width too long per
// boundary. After the connect returns, the detour rewrites every cross owned edge it left, on
// both sides, with the world-frame distance between the two cluster centres: the distance the
// connect computes for a negative stored cost, to within one unit of the stored half-precision
// value (the connect takes a reciprocal square root with one Newton step, the detour sqrtf).
// Only the live collection's connects are rewritten; other callers connect instances in
// temporary worlds. While the route planner is set, the same post-call also copies the
// registering section into the planner's store (coarse_graph_live.cpp); the two halves follow
// their own keys.

typedef unsigned __int64 (*graphInstanceConnect_t)(void* inst, void* coll);
static graphInstanceConnect_t orig_graphInstanceConnect = NULL;

static const size_t OFF_SM_WORLD         = 0x88;   // SectionManager -> hkaiWorld*
static const size_t OFF_WORLD_COLLECTION = 0x20;   // hkaiWorld -> hkaiStreamingCollection*

// calls == notLive + the live connects. Each link counts once in rewritten or skipped (a link
// rewritten on its own side whose reciprocal is absent counts skipped); an owned range skipped
// whole counts one skip and no link.
static volatile LONG s_calls     = 0;
static volatile LONG s_links     = 0;
static volatile LONG s_rewritten = 0;
static volatile LONG s_skipped   = 0;
static volatile LONG s_notLive   = 0;  // a temporary world's collection, or a NULL argument

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Set once at install, on the main thread, before the detour can run.
static int s_installed = 0;
static int s_costOn    = 0;   // clusterCrossCost: rewrite the cross-tile costs
static int s_liveOn    = 0;   // plannerMode is set: copy each registering section for the planner

// The live collection: the section manager's world's streaming collection. The path thread
// reads it inside NavMesh::update's exclusive world lock, where it cannot change.
static void* LiveCollection()
{
	uintptr_t mgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!mgr) return NULL;
	uintptr_t world = *(uintptr_t*)(KLIB_MEMBER(4, mgr, NavMesh_world, OFF_SM_WORLD));
	return world ? *(void**)(world + OFF_WORLD_COLLECTION) : NULL;
}

static const GuardCounter kBeatRows[] =
{
	{ "calls",     GF_COUNT, &s_calls,     0 },
	{ "links",     GF_COUNT, &s_links,     0 },
	{ "rewritten", GF_COUNT, &s_rewritten, 0 },
	{ "skipped",   GF_COUNT, &s_skipped,   0 },
	{ "notLive",   GF_COUNT, &s_notLive,   0 },
};

static void EmitHeartbeat()
{
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "ClusterCrossCost: mode=on");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Path thread, inside NavMesh::update's exclusive changeMutex (+0x200), after the connect has
// written both sides' owned edges. No allocation, and no lock of its own: the only lock is the
// deferred-log leaf that LogMsgDeferrable's off-main path enters, which takes nothing under it.
// The heartbeat is on a timer alone, since this site runs once per registered instance. The
// planner's copy adds no lock, no allocation and no log of its own.
static unsigned __int64 hook_graphInstanceConnect(void* inst, void* coll)
{
	unsigned __int64 r = orig_graphInstanceConnect(inst, coll);
	InterlockedIncrement(&s_calls);
	if (!inst || !coll || coll != LiveCollection())
		InterlockedIncrement(&s_notLive);
	else
	{
		if (s_costOn)
		{
			CrossCostCounts c = { 0, 0, 0 };
			CrossCostRewrite(inst, coll, &c);
			InterlockedExchangeAdd(&s_links, c.links);
			InterlockedExchangeAdd(&s_rewritten, c.rewritten);
			InterlockedExchangeAdd(&s_skipped, c.skipped);
		}
		if (s_liveOn)
			planner::CgLiveOnConnect(inst, coll);
	}
	if (s_costOn && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
	return r;
}

void InstallClusterCrossCost(int* installed, int*)
{
	if (!HookRowWanted(HOOK_GRAPH_INSTANCE_CONNECT))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;
	s_costOn = fixes::g_fixesCfg.clusterCrossCostOn != 0;
	s_liveOn = planner::g_plannerCfg.mode != planner::PLANNER_OFF;

	const char* why = HookInstall(HOOK_GRAPH_INSTANCE_CONNECT, hook_graphInstanceConnect,
	                              &orig_graphInstanceConnect, installed, true);
	if (!why)
	{
		s_installed = 1;
		if (s_costOn)
			LogMsg("Cluster cross cost: installed (links rewritten as instances register)");
	}
	else
	{
		orig_graphInstanceConnect = NULL;
		ErrorLog(std::string("Cluster cross cost: not installed (") + why
		         + "); cross-tile links keep their tile-local cost");
	}
}

const char* ClusterCrossCostToken()
{
	return (s_installed && s_costOn) ? "ON" : "OFF";
}
