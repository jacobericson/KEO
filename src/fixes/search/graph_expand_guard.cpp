#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/search/graph_expand_guard.h"
#include "fixes/search/graph_expand_guard_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// One A* iteration: pop the cheapest node from the open set, work out where it
// is, collect its neighbours, relax their costs, close it. Working out where
// it is means reading the node record and the node position out of the graph
// instance of the node's own section, which the iteration takes from a
// per-section cache on the visitor and dereferences unchecked. A section with
// no graph instance leaves that pointer NULL, and the two loads through it --
// the node array and the position array -- are access violations on the search
// thread.
//
// The condition is not in the arguments: the key belongs to the node the pop
// is about to return. But the pop reads that key out of the front of the open
// set before it disturbs the heap, so the same value can be peeked without
// consuming anything, and a function-entry detour can judge the node the
// iteration is about to expand.
//
// On the arm it answers the guard pops the node itself, through the engine's
// own pop, and returns the key the iteration would have returned. The node is
// therefore dropped from the search and the search carries on with every other
// node in the open set.
//
// Runs on the navmesh and content-stream threads inside a search. The detour
// allocates nothing, holds nothing across the original, and takes two locks on
// the arm it fires and one on the arm it speaks: the engine's pop takes none
// (it is a leaf that only sifts the caller's own heap), and LogMsgDeferrable's
// off-main path enters the deferred-log leaf lock.

// judged + unjudged + fired == calls, and each group's reasons sum to it:
// every call takes exactly one classification arm. Unjudged is counted apart
// from judged on purpose -- a call where nothing could be tested must never be
// read as a call where the instance was tested and was there.
static volatile LONG s_calls       = 0;  // detour entries
static volatile LONG s_judged      = 0;  // the instance was tested and was in place
static volatile LONG s_unjudged    = 0;  // nothing could be tested; handed to the original
static volatile LONG s_noOpenSet   = 0;  //   no open set, or empty
static volatile LONG s_noVisitor   = 0;  //   no visitor
static volatile LONG s_noArray     = 0;  //   the collection had no instance array
static volatile LONG s_fired       = 0;  // calls the guard answered itself
static volatile LONG s_badSection  = 0;  //   the key named a section the collection lacks
static volatile LONG s_noInstance  = 0;  //   the section had no graph instance
static volatile LONG s_noNodes     = 0;  //   it had one, with no node array
static volatile LONG s_noPositions = 0;  //   it had one, with no position array
static volatile LONG s_fireCache   = 0;  // of the firings, those whose pointer came
static volatile LONG s_fireLookup  = 0;  //   from the cached slot, and from a fresh lookup
static volatile LONG s_firstSec    = -1; // which sections fired, and the last index:
static volatile LONG s_lastSec     = -1; //   the counters outlive the capped lines below
static volatile LONG s_lastIdx     = -1;

static volatile LONG s_fireLines  = 0;
static const LONG kMaxFireLines   = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Fixed-buffer formatting: search thread, so no CRT stream and no allocation.
typedef FixedLogBufN<384> XBuf;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static const GuardCounter kBeatRows[] =
{
	{ "calls",       GF_COUNT, &s_calls,       0 },
	{ "judged",      GF_COUNT, &s_judged,      0 },
	{ "unjudged",    GF_COUNT, &s_unjudged,    0 },
	{ "noOpenSet",   GF_COUNT, &s_noOpenSet,   0 },
	{ "noVisitor",   GF_COUNT, &s_noVisitor,   0 },
	{ "noArray",     GF_COUNT, &s_noArray,     0 },
	{ "fired",       GF_COUNT, &s_fired,       0 },
	{ "badSection",  GF_COUNT, &s_badSection,  0 },
	{ "noInstance",  GF_COUNT, &s_noInstance,  0 },
	{ "noNodes",     GF_COUNT, &s_noNodes,     0 },
	{ "noPositions", GF_COUNT, &s_noPositions, 0 },
	{ "fireCache",   GF_COUNT, &s_fireCache,   0 },
	{ "fireLookup",  GF_COUNT, &s_fireLookup,  0 },
	{ "firstSec",    GF_COUNT, &s_firstSec,    0 },
	{ "lastSec",     GF_COUNT, &s_lastSec,     0 },
	{ "lastIdx",     GF_COUNT, &s_lastIdx,     0 },
};

static void EmitHeartbeat()
{
	XBuf o;
	GuardHeartbeatBegin(&o, "GraphExpandGuard running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer. A guard that spoke only when it fired would make
// "never fired" and "never installed on a live site" the same silence. The
// clock is read on the first call and then once every 1024, because this site
// runs once per expanded search node.
static void MaybeHeartbeat(LONG calls)
{
	if (GuardBeatSample(calls) && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

static const char* ArmName(GraphExpandArm arm)
{
	switch (arm)
	{
	case GRAPH_EXPAND_BAD_SECTION:  return "no such section";
	case GRAPH_EXPAND_NO_INSTANCE:  return "no graph instance";
	case GRAPH_EXPAND_NO_NODES:     return "no node array";
	case GRAPH_EXPAND_NO_POSITIONS: return "no position array";
	default:                        return "unclassified";
	}
}

// Which section and node the search wanted, and whether the absence came from
// a fresh lookup or from the slot the visitor cached earlier. A fresh lookup
// says the collection itself has nothing for that section -- a lifetime
// problem. A cached one says the collection may well have it now and the
// visitor is serving a NULL it stored when it did not.
static void EmitFireLine(const GraphExpandCall* call)
{
	XBuf o; o.n = 0;
	FlbStr(&o, "GraphExpandGuard FIRED: ");
	FlbStr(&o, ArmName(call->arm));
	FlbStr(&o, " for section "); FlbDec(&o, (__int64)(long)call->section);
	FlbStr(&o, " node ");        FlbDec(&o, (__int64)(long)call->index);
	FlbStr(&o, " (");            FlbStr(&o, call->fromCache ? "cached" : "looked up");
	FlbStr(&o, "); the node is dropped from the search. fired=");
	FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " coll=");  FlbHex(&o, (unsigned __int64)call->collection);
	FlbStr(&o, " inst=");  FlbHex(&o, (unsigned __int64)call->instance);
	FlbStr(&o, " key=");   FlbHex(&o, (unsigned __int64)call->key);
	FlbStr(&o, " tid=");   FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

typedef unsigned __int64 (*graphExpandNode_t)(void* graph, void* edgeCost,
                                              void* openSet, void* flags,
                                              void* parent, void* neighbours,
                                              void* state, void* visitor);
static graphExpandNode_t orig_graphExpandNode = NULL;

// The engine's own open-set pop, called only on the arm the guard answers.
// Without it the iteration makes no progress: the caller reports "keep going"
// and the outer loop pops the same front node again until the search runs out
// of iterations.
typedef unsigned int (*openSetPopNext_t)(void* openSet);
static openSetPopNext_t fn_openSetPopNext = NULL;

// The pop is called, not detoured, so it carries no build-gate row. Its first
// bytes are still checked, because a call into an address that is not the
// function the firing arm was written against is worse than not arming.
static const unsigned char kOpenSetPopHead[16] =
{
	0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10,
	0x48,0x89,0x7C,0x24,0x18, 0x4C
};

static unsigned __int64 hook_graphExpandNode(void* graph, void* edgeCost,
                                             void* openSet, void* flags,
                                             void* parent, void* neighbours,
                                             void* state, void* visitor)
{
	LONG calls = InterlockedIncrement(&s_calls);

	GraphExpandCall call;
	InspectGraphExpandCall(openSet, visitor, &call);

	if (!GraphExpandArmSubstitutes(call.arm))
	{
		if (GraphExpandArmUnjudged(call.arm))
		{
			InterlockedIncrement(&s_unjudged);
			if (call.arm == GRAPH_EXPAND_NO_OPEN_SET)
				InterlockedIncrement(&s_noOpenSet);
			else if (call.arm == GRAPH_EXPAND_NO_VISITOR)
				InterlockedIncrement(&s_noVisitor);
			else
				InterlockedIncrement(&s_noArray);
		}
		else
		{
			InterlockedIncrement(&s_judged);
		}
		// Emitted before the call that can fault, so a session that dies in it
		// still carries the totals.
		MaybeHeartbeat(calls);
		return orig_graphExpandNode(graph, edgeCost, openSet, flags, parent,
		                            neighbours, state, visitor);
	}

	InterlockedIncrement(&s_fired);
	if (call.arm == GRAPH_EXPAND_BAD_SECTION)
		InterlockedIncrement(&s_badSection);
	else if (call.arm == GRAPH_EXPAND_NO_INSTANCE)
		InterlockedIncrement(&s_noInstance);
	else if (call.arm == GRAPH_EXPAND_NO_NODES)
		InterlockedIncrement(&s_noNodes);
	else
		InterlockedIncrement(&s_noPositions);
	InterlockedIncrement(call.fromCache ? &s_fireCache : &s_fireLookup);
	InterlockedCompareExchange(&s_firstSec, (LONG)call.section, -1);
	InterlockedExchange(&s_lastSec, (LONG)call.section);
	InterlockedExchange(&s_lastIdx, (LONG)call.index);

	// Consume the node the iteration was about to expand, and nothing else.
	// The visitor's cache is left exactly as it was: its value at +8 and its
	// key at +104 are only meaningful as a pair, and writing one without the
	// other would leave the next iteration reading a pointer for the wrong
	// section.
	unsigned int key = fn_openSetPopNext(openSet);

	// Stop counting at the cap rather than counting forever: this site fires
	// per expanded node, and a wrapped counter would restart logging from
	// inside a search.
	for (;;)
	{
		LONG seen = Read(&s_fireLines);
		if (seen >= kMaxFireLines)
			break;
		if (InterlockedCompareExchange(&s_fireLines, seen + 1, seen) == seen)
		{
			EmitFireLine(&call);
			break;
		}
	}
	MaybeHeartbeat(calls);

	// What the iteration returns on every one of its own paths: the key it
	// popped. The caller compares it against the goal key and reports success
	// on a match, so the only safe answer is the one the engine would have
	// given, and this is it.
	return (unsigned __int64)key;
}

void InstallGraphExpandGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_GRAPH_EXPAND_NODE))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	const char* why = NULL;
	if (!VerifyPrologueByRva(RVA_GRAPH_EXPAND_NODE))
		why = "prologue";
	else if (memcmp((const void*)GameAddr(RVA_OPENSET_POP_NEXT), kOpenSetPopHead,
	                sizeof(kOpenSetPopHead)) != 0)
		why = "pop";
	else
	{
		// Before the install: the detour calls it as soon as it is in.
		fn_openSetPopNext = (openSetPopNext_t)GameAddr(RVA_OPENSET_POP_NEXT);
		// The prologue was verified above; the row is not verified twice.
		why = HookInstall(HOOK_GRAPH_EXPAND_NODE, hook_graphExpandNode,
				&orig_graphExpandNode, installed, false);
	}

	if (!why)
	{
		LogMsg("Graph expand guard: installed (a heartbeat line follows the first minute of searching)");
	}
	else
	{
		orig_graphExpandNode = NULL;
		fn_openSetPopNext = NULL;
		ErrorLog(std::string("Graph expand guard: not installed (") + why
		         + "); expanding a node whose section has no graph instance still faults");
	}
}
