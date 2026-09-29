#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/search/graph_visitor_guard.h"
#include "fixes/search/graph_visitor_guard_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// The site records a search node's cost and, for a node it has not estimated
// yet, the distance from that node to the goal. Getting the node's position
// means turning a packed key into a point through a per-section graph
// instance, and the engine dereferences that instance without checking it. A
// section whose instance is absent makes it NULL, and the load of the position
// array through it is an access violation on the search thread.
//
// Everything the fault depends on is an argument at entry, so the test is a
// function-entry detour rather than a patch inside the body.
//
// Runs on the navmesh and content-stream threads inside a search: no
// allocation, nothing held across the original, and the only lock is the
// deferred-log leaf that LogMsgDeferrable's off-main path enters.

// visited + checked + noNode == calls, and the three reasons sum to fired:
// every call takes exactly one classification arm, and checked counts the new
// nodes whose instance was tested, fired ones included. A stats line whose
// numbers do not close did not come from this code.
static volatile LONG s_calls      = 0;  // detour entries
static volatile LONG s_visited    = 0;  // the estimate was cached; the heuristic went unread
static volatile LONG s_checked    = 0;  // a new node, so the instance was tested
static volatile LONG s_noNode     = 0;  // no node record; handed to the original
static volatile LONG s_fired      = 0;  // calls the guard answered itself
static volatile LONG s_noHeur     = 0;  //   because the heuristic argument was NULL
static volatile LONG s_noHolder   = 0;  //   because its cache holder was NULL
static volatile LONG s_noInstance = 0;  //   because the section's instance was NULL
static volatile LONG s_lastSec    = -1; // the last section that fired, and its index:
static volatile LONG s_lastIdx    = -1; //   the counters outlive the capped lines below
static volatile LONG s_firstSec   = -1;

static volatile LONG s_fireLines  = 0;
static const LONG kMaxFireLines   = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Fixed-buffer formatting: search thread, so no CRT stream and no allocation.
typedef FixedLogBufN<320> VBuf;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static const GuardCounter kBeatRows[] =
{
	{ "calls",      GF_COUNT, &s_calls,      0 },
	{ "visited",    GF_COUNT, &s_visited,    0 },
	{ "checked",    GF_COUNT, &s_checked,    0 },
	{ "noNode",     GF_COUNT, &s_noNode,     0 },
	{ "fired",      GF_COUNT, &s_fired,      0 },
	{ "noHeur",     GF_COUNT, &s_noHeur,     0 },
	{ "noHolder",   GF_COUNT, &s_noHolder,   0 },
	{ "noInstance", GF_COUNT, &s_noInstance, 0 },
	{ "firstSec",   GF_COUNT, &s_firstSec,   0 },
	{ "lastSec",    GF_COUNT, &s_lastSec,    0 },
	{ "lastIdx",    GF_COUNT, &s_lastIdx,    0 },
};

static void EmitHeartbeat()
{
	VBuf o;
	GuardHeartbeatBegin(&o, "GraphVisitorGuard running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer. A guard that spoke only when it fired would make
// "never fired" and "never installed on a live site" the same silence. The
// clock is read on the first call and then once every 1024, because this site
// runs per improved search node rather than per teardown.
static void MaybeHeartbeat(LONG calls)
{
	if (GuardBeatSample(calls) && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

static const char* ArmName(GraphVisitorArm arm)
{
	switch (arm)
	{
	case GRAPH_VISITOR_NO_HEURISTIC: return "heuristic";
	case GRAPH_VISITOR_NO_HOLDER:    return "holder";
	default:                         return "instance";
	}
}

// Which section and position the search wanted, and through what. A count
// alone cannot say whether the absent sections are ones being retired -- a
// lifetime problem -- or resident ones, which would put the fault in the
// caller's per-section cache instead.
static void EmitFireLine(const GraphVisitorCall* call)
{
	VBuf o; o.n = 0;
	FlbStr(&o, "GraphVisitorGuard FIRED: no ");
	FlbStr(&o, ArmName(call->arm));
	FlbStr(&o, " for section "); FlbDec(&o, (__int64)(long)call->section);
	FlbStr(&o, " index ");       FlbDec(&o, (__int64)(long)call->index);
	FlbStr(&o, "; the node keeps no estimate. fired="); FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " heur=");   FlbHex(&o, (unsigned __int64)call->heuristic);
	FlbStr(&o, " holder="); FlbHex(&o, (unsigned __int64)call->holder);
	FlbStr(&o, " tid=");    FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

typedef unsigned __int64 (*searchSetNodeCost_t)(void* state, void* heuristic,
                                                unsigned int packedKey, float cost);
static searchSetNodeCost_t orig_searchSetNodeCost = NULL;

static unsigned __int64 hook_searchSetNodeCost(void* state, void* heuristic,
                                               unsigned int packedKey, float cost)
{
	LONG calls = InterlockedIncrement(&s_calls);

	GraphVisitorCall call;
	InspectGraphVisitorCall(state, heuristic, packedKey, &call);

	if (!GraphVisitorArmSubstitutes(call.arm))
	{
		if (call.arm == GRAPH_VISITOR_VISITED)
			InterlockedIncrement(&s_visited);
		else if (call.arm == GRAPH_VISITOR_NO_NODE)
			InterlockedIncrement(&s_noNode);
		else
			InterlockedIncrement(&s_checked);
		// Emitted before the call that can fault, so a session that dies in
		// it still carries the totals.
		MaybeHeartbeat(calls);
		return orig_searchSetNodeCost(state, heuristic, packedKey, cost);
	}

	InterlockedIncrement(&s_checked);
	InterlockedIncrement(&s_fired);
	if (call.arm == GRAPH_VISITOR_NO_HEURISTIC)
		InterlockedIncrement(&s_noHeur);
	else if (call.arm == GRAPH_VISITOR_NO_HOLDER)
		InterlockedIncrement(&s_noHolder);
	else
		InterlockedIncrement(&s_noInstance);
	InterlockedCompareExchange(&s_firstSec, (LONG)call.section, -1);
	InterlockedExchange(&s_lastSec, (LONG)call.section);
	InterlockedExchange(&s_lastIdx, (LONG)call.index);

	ApplyGraphVisitorNoEstimate(&call, cost);

	if (InterlockedIncrement(&s_fireLines) <= kMaxFireLines)
		EmitFireLine(&call);
	MaybeHeartbeat(calls);

	// The one caller discards this; the original returns the position array it
	// read, which on this path was never reached.
	return 0;
}

void InstallGraphVisitorGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_SEARCH_SET_NODE_COST))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	const char* why = HookInstall(HOOK_SEARCH_SET_NODE_COST, hook_searchSetNodeCost,
			&orig_searchSetNodeCost, installed, true);

	if (!why)
	{
		LogMsg("Graph visitor guard: installed (a heartbeat line follows the first minute of searching)");
	}
	else
	{
		orig_searchSetNodeCost = NULL;
		ErrorLog(std::string("Graph visitor guard: not installed (") + why
		         + "); a search that meets a section with no graph instance still faults");
	}
}
