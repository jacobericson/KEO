#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/search/graph_heuristic_guard.h"
#include "fixes/search/graph_heuristic_guard_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include <windows.h>
#include <intrin.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

// The A* search's hierarchical heuristic reads a section's cluster-graph instance at three
// sites, through one-slot caches on its coarse-graph visitor, and no caller checks the
// instance: the goal-adjacency test, the cluster-centre lookup and the coarse-search seed. A
// section whose instance is absent, or a key naming a section past the collection, faults
// there. Each detour replicates its site's own lookup at entry; on a failed one it stops the
// site, gives the caller a finite answer and sets the heuristic's start cluster to -1, which
// makes every later heuristic evaluation of that search Euclidean before it touches a graph.
//
// The seed is shared with the pathExists setup, whose context is not a heuristic: only the
// heuristic init's return address may write the start cluster.
//
// Runs on the path thread inside a search, and for the seed also on the AI back thread
// through pathExists: no allocation, nothing held across the original, and the only lock is
// the deferred-log leaf LogMsgDeferrable's off-main path enters.

namespace graph_heuristic_guard_detail {

typedef FixedLogBufN<320> HBuf;

typedef char (*goalAdjacent_t)(void* heuristic, unsigned int clusterKey, int* goalIdxOut,
                               unsigned int* costOut);
typedef unsigned __int64 (*clusterCentre_t)(void* heuristic, unsigned int clusterKey,
                                            GraphPositionVec4* out);
typedef unsigned __int64 (*coarseSeed_t)(void* coarseSearch, void* visitor,
                                         unsigned int* goalClusterKeys, unsigned int count,
                                         unsigned int startClusterKey);

} // namespace graph_heuristic_guard_detail
using namespace graph_heuristic_guard_detail;

static goalAdjacent_t  orig_goalAdjacent  = NULL;
static clusterCentre_t orig_clusterCentre = NULL;
static coarseSeed_t    orig_coarseSeed    = NULL;

// calls == run + unjudged + early + fired, and adjacent + centre + seed == fired.
static volatile LONG s_calls          = 0;
static volatile LONG s_run            = 0;  // every instance the site reads is in place
static volatile LONG s_unjudged       = 0;  // nothing could be tested; the original runs
static volatile LONG s_early          = 0;  // the seed returns before any read
static volatile LONG s_fired          = 0;
static volatile LONG s_adjacent       = 0;
static volatile LONG s_centre         = 0;
static volatile LONG s_seed           = 0;
static volatile LONG s_seedPathExists = 0;  // seed fires under the pathExists setup
static volatile LONG s_heuristicOff   = 0;  // start-cluster writes
static volatile LONG s_firstSec       = -1;
static volatile LONG s_lastSec        = -1;

static volatile LONG s_fireLines = 0;
static const LONG kMaxFireLines  = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Both set once at install, on the main thread, before any detour can run.
static uintptr_t s_gameImageSize = 0;
static int       s_rowsInstalled = 0;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static const GuardCounter kBeatRows[] =
{
	{ "calls",          GF_COUNT, &s_calls,          0 },
	{ "run",            GF_COUNT, &s_run,            0 },
	{ "unjudged",       GF_COUNT, &s_unjudged,       0 },
	{ "early",          GF_COUNT, &s_early,          0 },
	{ "fired",          GF_COUNT, &s_fired,          0 },
	{ "adjacent",       GF_COUNT, &s_adjacent,       0 },
	{ "centre",         GF_COUNT, &s_centre,         0 },
	{ "seed",           GF_COUNT, &s_seed,           0 },
	{ "seedPathExists", GF_COUNT, &s_seedPathExists, 0 },
	{ "heuristicOff",   GF_COUNT, &s_heuristicOff,   0 },
	{ "firstSec",       GF_COUNT, &s_firstSec,       0 },
	{ "lastSec",        GF_COUNT, &s_lastSec,        0 },
};

static void EmitHeartbeat()
{
	HBuf o;
	GuardHeartbeatBegin(&o, "GraphHeuristicGuard running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer, so a silent guard reads as installed and not firing. The clock is
// read on the first call and then once every 1024: these sites run per heuristic evaluation.
static void MaybeHeartbeat(LONG calls)
{
	if (GuardBeatSample(calls) && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

// The return address as an RVA into the game module; inExe is false for any other module.
static unsigned ReturnRva(void* ret, bool* inExe)
{
	const uintptr_t addr = (uintptr_t)ret;
	const uintptr_t base = gameBase;
	*inExe = base != 0 && s_gameImageSize != 0 && addr >= base && (addr - base) < s_gameImageSize;
	return *inExe ? (unsigned)(addr - base) : 0;
}

static void CountPass(GraphHeuristicArm arm)
{
	if (arm == GH_UNJUDGED)
		InterlockedIncrement(&s_unjudged);
	else if (arm == GH_EARLY_RETURN)
		InterlockedIncrement(&s_early);
	else
		InterlockedIncrement(&s_run);
}

static const char* SiteName(GraphHeuristicSite site)
{
	switch (site)
	{
	case GH_SITE_ADJACENT: return "adjacent";
	case GH_SITE_CENTRE:   return "centre";
	default:               return "seed";
	}
}

static const char* ArmName(GraphHeuristicArm arm)
{
	switch (arm)
	{
	case GH_BAD_SECTION:   return "no such section";
	case GH_NO_COLLECTION: return "no collection";
	default:               return "no instance";
	}
}

static void EmitFireLine(GraphHeuristicSite site, const GraphHeuristicCall* call, unsigned retRva,
                         bool heuristicOff)
{
	HBuf o;
	FlbInit(&o);
	FlbStr(&o, "GraphHeuristicGuard FIRED: ");
	FlbStr(&o, SiteName(site));
	FlbChar(&o, ' ');
	FlbStr(&o, ArmName(call->arm));
	FlbStr(&o, " for section "); FlbDec(&o, (__int64)call->section);
	FlbStr(&o, " key ");         FlbHex(&o, (unsigned __int64)call->key);
	if (heuristicOff)
		FlbStr(&o, "; the heuristic is Euclidean from here. fired=");
	else
		FlbStr(&o, "; the seed is skipped, the pathExists context untouched. fired=");
	FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " ret="); FlbHex(&o, (unsigned __int64)retRva);
	FlbStr(&o, " tid="); FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

static void RecordFire(GraphHeuristicSite site, const GraphHeuristicCall* call, unsigned retRva,
                       bool heuristicOff)
{
	InterlockedIncrement(&s_fired);
	InterlockedIncrement(site == GH_SITE_ADJACENT ? &s_adjacent
	                     : site == GH_SITE_CENTRE ? &s_centre : &s_seed);
	if (heuristicOff)
		InterlockedIncrement(&s_heuristicOff);
	InterlockedCompareExchange(&s_firstSec, (LONG)call->section, -1);
	InterlockedExchange(&s_lastSec, (LONG)call->section);
	if (GuardFireClaim(&s_fireLines, kMaxFireLines))
		EmitFireLine(site, call, retRva, heuristicOff);
}

static const void* VisitorOf(void* heuristic)
{
	return heuristic ? *(void* const*)((char*)heuristic + OFF_HEUR_VISITOR) : NULL;
}

static char hook_graphHeuristicGoalAdjacent(void* heuristic, unsigned int clusterKey, int* goalIdxOut,
                                            unsigned int* costOut)
{
	const LONG calls = InterlockedIncrement(&s_calls);
	GraphHeuristicCall call;
	InspectGraphHeuristicKey(VisitorOf(heuristic), GH_SITE_ADJACENT, clusterKey, &call);
	if (!GraphHeuristicArmFires(call.arm))
	{
		CountPass(call.arm);
		MaybeHeartbeat(calls);
		return orig_goalAdjacent(heuristic, clusterKey, goalIdxOut, costOut);
	}

	// "Not adjacent", with both outputs untouched; the caller's next step asks the centre.
	bool inExe;
	const unsigned retRva = ReturnRva(_ReturnAddress(), &inExe);
	GraphHeuristicMakeEuclidean(heuristic);
	RecordFire(GH_SITE_ADJACENT, &call, retRva, true);
	MaybeHeartbeat(calls);
	return 0;
}

static unsigned __int64 hook_graphHeuristicClusterCentre(void* heuristic, unsigned int clusterKey,
                                                         GraphPositionVec4* out)
{
	const LONG calls = InterlockedIncrement(&s_calls);
	GraphHeuristicCall call;
	InspectGraphHeuristicKey(VisitorOf(heuristic), GH_SITE_CENTRE, clusterKey, &call);
	if (!GraphHeuristicArmFires(call.arm))
	{
		CountPass(call.arm);
		MaybeHeartbeat(calls);
		return orig_clusterCentre(heuristic, clusterKey, out);
	}

	// A finite centre keeps the current evaluation's arithmetic finite.
	bool inExe;
	const unsigned retRva = ReturnRva(_ReturnAddress(), &inExe);
	if (out)
		*out = GraphHeuristicCentreFallback(GraphHeuristicCentreFallbackPoint(heuristic));
	GraphHeuristicMakeEuclidean(heuristic);
	RecordFire(GH_SITE_CENTRE, &call, retRva, true);
	MaybeHeartbeat(calls);
	// The positions pointer this normally returns; neither caller reads it.
	return 0;
}

static unsigned __int64 hook_graphHeuristicCoarseSeed(void* coarseSearch, void* visitor,
                                                      unsigned int* goalClusterKeys, unsigned int count,
                                                      unsigned int startClusterKey)
{
	const LONG calls = InterlockedIncrement(&s_calls);
	bool inExe;
	const unsigned retRva = ReturnRva(_ReturnAddress(), &inExe);
	GraphHeuristicCall call;
	InspectGraphHeuristicSeed(coarseSearch, visitor, goalClusterKeys, count, startClusterKey, &call);
	if (!GraphHeuristicArmFires(call.arm))
	{
		CountPass(call.arm);
		MaybeHeartbeat(calls);
		return orig_coarseSeed(coarseSearch, visitor, goalClusterKeys, count, startClusterKey);
	}

	// The site's own first store, so the coarse search keeps its visitor; the seeding is
	// skipped. The coarse search sits inside the heuristic only under the heuristic init.
	*(void**)coarseSearch = visitor;
	const bool heuristicOff = GraphHeuristicSeedWritesStartCluster(retRva, inExe);
	if (heuristicOff)
		GraphHeuristicMakeEuclidean(GraphHeuristicOfSeedCoarse(coarseSearch));
	if (inExe && retRva == GH_RET_PATH_EXISTS)
		InterlockedIncrement(&s_seedPathExists);
	RecordFire(GH_SITE_SEED, &call, retRva, heuristicOff);
	MaybeHeartbeat(calls);
	// Both callers ignore the result.
	return 0;
}

static uintptr_t GameImageSize()
{
	if (!gameBase)
		return 0;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)gameBase;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(gameBase + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	return nt->OptionalHeader.SizeOfImage;
}

static void NoteRow(const char* why, const char* row, int* n)
{
	if (!why)
	{
		++*n;
		return;
	}
	ErrorLog(std::string("Graph heuristic guard: ") + row + " not installed (" + why
	         + "); that site still faults on an absent graph instance");
}

void InstallGraphHeuristicGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_GRAPH_HEURISTIC_GOAL_ADJACENT))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;
	s_gameImageSize = GameImageSize();

	int n = 0;
	NoteRow(HookInstall(HOOK_GRAPH_HEURISTIC_GOAL_ADJACENT, hook_graphHeuristicGoalAdjacent,
	                    &orig_goalAdjacent, installed, true), "goal adjacency", &n);
	NoteRow(HookInstall(HOOK_GRAPH_HEURISTIC_CLUSTER_CENTRE, hook_graphHeuristicClusterCentre,
	                    &orig_clusterCentre, installed, true), "cluster centre", &n);
	NoteRow(HookInstall(HOOK_GRAPH_HEURISTIC_COARSE_SEED, hook_graphHeuristicCoarseSeed,
	                    &orig_coarseSeed, installed, true), "coarse seed", &n);
	s_rowsInstalled = n;

	LogMsg(std::string("Graph heuristic guard: installed ") + (char)('0' + n)
	       + "/3 (a heartbeat line follows the first search, then one a minute)");
}

bool GraphHeuristicGuardComplete()
{
	return s_rowsInstalled == 3;
}

const char* GraphHeuristicGuardToken()
{
	return GraphHeuristicGuardComplete() ? "ON" : (s_rowsInstalled > 0 ? "PARTIAL" : "OFF");
}
