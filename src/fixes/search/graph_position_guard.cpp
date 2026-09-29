#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/search/graph_position_guard.h"
#include "fixes/search/graph_position_guard_policy.h"
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

// The cluster-graph search (checkFaceConnectivity's own graph, reached only
// when clusterGraphBypass consults the original: measure, player or off) asks
// this helper for a node's world position twice per query -- once per
// candidate border point while its setup fills a node's position array, and
// once for the query point itself inside its heuristic. Both calls go through
// a one-slot per-section instance cache carried on a small per-search context,
// a third copy of the shape graph_visitor_guard and graph_expand_guard each
// already close on their own functions. Neither the cached pointer nor a
// fresh lookup's result is checked before the site dereferences it to reach
// the position array; an absent instance is a NULL there, and the load that
// follows is an access violation on the search thread.
//
// Everything the fault depends on is an argument at entry, so the test is a
// function-entry detour, and it is on the function itself rather than on
// either caller, because both callers reach the same unchecked read through
// it.
//
// The detour installs unconditionally (prologue permitting): graphPositionGuard
// chooses only whether a call that met an absent instance is substituted or
// merely counted and handed to the original, never whether the site is
// classified at all -- so the fault's own base rate stays measurable with the
// lever off.
//
// Runs on the navmesh and content-stream threads inside a search: no
// allocation, nothing held across the original, and the only lock is the
// deferred-log leaf that LogMsgDeferrable's off-main path enters.

// run + unjudged + fired == calls, and each group's reasons sum to it: every
// call takes exactly one classification arm. Unjudged is counted apart from
// run on purpose -- a call where nothing could be tested must never read as a
// call where the instance was tested and was there. fired is classification,
// not action: it counts every call that met an absent instance whether or
// not graphPositionGuard is on, so acted + observed == fired regardless of
// the lever -- the guard is installed unconditionally (VerifyPrologueByRva
// permitting) and the lever only chooses which of those two the firing arm
// takes, never whether the site is watched at all.
static volatile LONG s_calls        = 0;  // detour entries
static volatile LONG s_run          = 0;  // the instance was tested and was in place
static volatile LONG s_unjudged     = 0;  // nothing could be tested; handed to the original
static volatile LONG s_noCtx        = 0;  //   no context, or its own pointer was absent
static volatile LONG s_noArray      = 0;  //   the collection had no instance array
static volatile LONG s_fired        = 0;  // calls that met an absent instance, either mode
static volatile LONG s_badSection   = 0;  //   the key named a section the collection lacks
static volatile LONG s_noFallback   = 0;  //   no collection, and the fallback slot was empty
static volatile LONG s_noLookup     = 0;  //   a fresh lookup found no instance
static volatile LONG s_noCached     = 0;  //   the cached slot already held no instance
static volatile LONG s_acted        = 0;  //   of the fired calls, substituted (mode = guard)
static volatile LONG s_observed     = 0;  //   of the fired calls, left to fault (mode = observe)
static volatile LONG s_firstSec     = -1; // which sections fired, and the last index:
static volatile LONG s_lastSec      = -1; //   the counters outlive the capped lines below
static volatile LONG s_lastIdx      = -1;

static volatile LONG s_fireLines  = 0;
static const LONG kMaxFireLines   = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Set once at install, before the hook can be called, and never again -- the
// same "read when the guard installs" rule every lever in this file follows.
// true substitutes the far position and answers the call itself (as shipped);
// false only classifies and counts, then always calls the original with the
// original arguments, so an absent instance still faults exactly as vanilla
// does and the fault's own base rate stays measurable with the lever off.
static bool s_actMode = true;

// Fixed-buffer formatting: search thread, so no CRT stream and no allocation.
typedef FixedLogBufN<320> PBuf;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static const GuardCounter kBeatRows[] =
{
	{ "calls",      GF_COUNT, &s_calls,      0 },
	{ "run",        GF_COUNT, &s_run,        0 },
	{ "unjudged",   GF_COUNT, &s_unjudged,   0 },
	{ "noCtx",      GF_COUNT, &s_noCtx,      0 },
	{ "noArray",    GF_COUNT, &s_noArray,    0 },
	{ "fired",      GF_COUNT, &s_fired,      0 },
	{ "badSection", GF_COUNT, &s_badSection, 0 },
	{ "noFallback", GF_COUNT, &s_noFallback, 0 },
	{ "noLookup",   GF_COUNT, &s_noLookup,   0 },
	{ "noCached",   GF_COUNT, &s_noCached,   0 },
	{ "acted",      GF_COUNT, &s_acted,      0 },
	{ "observed",   GF_COUNT, &s_observed,   0 },
	{ "firstSec",   GF_COUNT, &s_firstSec,   0 },
	{ "lastSec",    GF_COUNT, &s_lastSec,    0 },
	{ "lastIdx",    GF_COUNT, &s_lastIdx,    0 },
};

static void EmitHeartbeat()
{
	PBuf o;
	GuardHeartbeatBegin(&o, "GraphPositionGuard running:");
	FlbStr(&o, " mode="); FlbStr(&o, s_actMode ? "guard" : "observe");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer. A guard that spoke only when it fired would make
// "never fired" and "never installed on a live site" the same silence. The
// clock is read on the first call and then once every 1024, because this site
// runs per position lookup rather than per teardown.
static void MaybeHeartbeat(LONG calls)
{
	if (GuardBeatSample(calls) && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

static const char* ArmName(GraphPositionArm arm)
{
	switch (arm)
	{
	case GRAPH_POSITION_BAD_SECTION:               return "no such section";
	case GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION:  return "no instance (no collection)";
	case GRAPH_POSITION_NO_INSTANCE_LOOKUP:         return "no instance (looked up)";
	case GRAPH_POSITION_NO_INSTANCE_CACHED:         return "no instance (cached)";
	default:                                        return "unclassified";
	}
}

// Which section and index the lookup wanted, and by which of the three roads
// the absence was found. A count alone cannot say whether an absent section is
// one being retired -- a lifetime problem -- or one the cache is still
// misreporting after it stopped being absent.
static void EmitFireLine(const GraphPositionCall* call, bool acted)
{
	PBuf o; o.n = 0;
	FlbStr(&o, "GraphPositionGuard FIRED: ");
	FlbStr(&o, ArmName(call->arm));
	FlbStr(&o, " for section "); FlbDec(&o, (__int64)(long)call->section);
	FlbStr(&o, " index ");       FlbDec(&o, (__int64)(long)call->index);
	if (acted)
		FlbStr(&o, "; the position is set far away. fired=");
	else
		FlbStr(&o, "; not acted on (observe mode) -- the original runs unchanged. fired=");
	FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " coll="); FlbHex(&o, (unsigned __int64)call->collection);
	FlbStr(&o, " tid=");  FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

typedef unsigned __int64 (*graphPositionLookup_t)(void** ctxPtr, unsigned int packedKey,
                                                   GraphPositionVec4* outPos);
static graphPositionLookup_t orig_graphPositionLookup = NULL;

static unsigned __int64 hook_graphPositionLookup(void** ctxPtr, unsigned int packedKey,
                                                  GraphPositionVec4* outPos)
{
	LONG calls = InterlockedIncrement(&s_calls);

	GraphPositionCall call;
	InspectGraphPositionCall(ctxPtr, packedKey, &call);

	if (!GraphPositionArmSubstitutes(call.arm))
	{
		if (GraphPositionArmUnjudged(call.arm))
		{
			InterlockedIncrement(&s_unjudged);
			if (call.arm == GRAPH_POSITION_NO_CTX)
				InterlockedIncrement(&s_noCtx);
			else
				InterlockedIncrement(&s_noArray);
		}
		else
		{
			InterlockedIncrement(&s_run);
		}
		MaybeHeartbeat(calls);
		return orig_graphPositionLookup(ctxPtr, packedKey, outPos);
	}

	InterlockedIncrement(&s_fired);
	if (call.arm == GRAPH_POSITION_BAD_SECTION)
		InterlockedIncrement(&s_badSection);
	else if (call.arm == GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION)
		InterlockedIncrement(&s_noFallback);
	else if (call.arm == GRAPH_POSITION_NO_INSTANCE_LOOKUP)
		InterlockedIncrement(&s_noLookup);
	else
		InterlockedIncrement(&s_noCached);
	InterlockedCompareExchange(&s_firstSec, (LONG)call.section, -1);
	InterlockedExchange(&s_lastSec, (LONG)call.section);
	InterlockedExchange(&s_lastIdx, (LONG)call.index);

	const bool acted = s_actMode;
	InterlockedIncrement(acted ? &s_acted : &s_observed);

	for (;;)
	{
		LONG seen = Read(&s_fireLines);
		if (seen >= kMaxFireLines)
			break;
		if (InterlockedCompareExchange(&s_fireLines, seen + 1, seen) == seen)
		{
			EmitFireLine(&call, acted);
			break;
		}
	}
	MaybeHeartbeat(calls);

	if (!acted)
	{
		// Observe mode: classified and counted, but the call is otherwise
		// untouched -- the original runs with the original arguments and
		// faults exactly as it would with the guard absent, so the fault's
		// own base rate stays visible in a control with the lever off.
		return orig_graphPositionLookup(ctxPtr, packedKey, outPos);
	}

	if (outPos)
		*outPos = GraphPositionFarSentinel();

	// The positions-array pointer this normally returns: neither caller reads
	// it, so 0 is the safe answer on the arm nothing consumes.
	return 0;
}

void InstallGraphPositionGuard(int* installed, int*)
{
	// The lever chooses the action (substitute or observe), never whether the
	// site is watched: the detour installs whenever the prologue verifies, so
	// graphPositionGuard=false still classifies and counts every call, and
	// the fault's base rate stays measurable in exactly the run meant to
	// measure it.
	s_actMode = fixes::g_fixesCfg.graphPositionGuardEnabled;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	const char* why = HookInstall(HOOK_GRAPH_POSITION_LOOKUP, hook_graphPositionLookup,
			&orig_graphPositionLookup, installed, true);

	if (!why)
	{
		LogMsg(std::string("Graph position guard: installed, mode=")
		       + (s_actMode ? "guard" : "observe")
		       + " (a heartbeat line follows the first minute of searching)");
	}
	else
	{
		orig_graphPositionLookup = NULL;
		ErrorLog(std::string("Graph position guard: not installed (") + why
		         + "); a cluster-graph search that meets a section with no graph instance still faults");
	}
}
