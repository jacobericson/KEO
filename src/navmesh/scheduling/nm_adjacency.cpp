// nm_adjacency.cpp - navmesh adjacency state, shared helpers and install.
// The observer, checker and heartbeat live in their own adjacency files.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "navmesh/scheduling/nm_adjacency.h"
#include "navmesh/nm_workers.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include <string.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

#include "navmesh/scheduling/nm_adjacency_internal.h"

namespace nm_adjacency_detail {
// ---------------------------------------------------------------------------
// State visible in every build (the heartbeat and the checker's presence)
// ---------------------------------------------------------------------------

volatile LONG s_checkerPresent = 0;

} // namespace nm_adjacency_detail
using namespace nm_adjacency_detail;
void NmAdjNoteCheckerPresent() { InterlockedExchange(&s_checkerPresent, 1); }


namespace nm_adjacency_detail {
// UtilityT::getSubMapSector's cell sizes (X, then Z), static constants in the
// image; read at install.
static const size_t RVA_SUBMAP_CELL_Z = 0x20989F0;
static const size_t RVA_SUBMAP_CELL_X = 0x20989F4;

int   s_mode = MODE_OFF;
static const char* s_modeWhy = "not installed";
static float s_cellX = 0.0f, s_cellZ = 0.0f;
bool  s_observerInstalled = false;
const char* s_observerWhy = "not attempted";

// Worker/bg claim scans and claim ends mutate the registry under queue
// +152; the path-thread drain observer takes +152 then done.mutex. WBegin
// makes g_regSeq odd, WEnd publishes the hint then makes it even. Exclusion
// scans read under +152; the stitch diagnostic copies entries with four
// sequence-checked attempts and counts/omits a failed copy. No torn copy is
// used. Initialized before install; individual jobs free on drop or drain,
// and stop unpins. bgLastLook/bgLooked are the bg-only unlocked exception.
NmAdjRegistry g_reg;
volatile LONG g_regSeq  = 0;   // odd while a writer is inside
volatile LONG g_pubHint = 0;   // published entries, for the observer's fast path
HANDLE        g_adjEvent = NULL;

__declspec(thread) int              t_own     = -1;
__declspec(thread) unsigned __int64 t_ownTask = 0;

LONGLONG s_qpf = 1;

// Counters.
volatile LONG   s_calls        = 0;   // NavMeshGenerator::update passes
volatile LONG   s_claims       = 0;
volatile LONG   s_skips        = 0;   // candidates skipped for a conflict
volatile LONG   s_skipClaims   = 0;   // claims that passed over a conflicting job
volatile LONG   s_would        = 0;   // count mode: claims that conflicted
volatile LONG   s_deferred     = 0;
volatile LONG64 s_deferMaxUs   = 0;
// A bg wait episode: the bg thread waiting with nothing in the queue it could
// run, from its first poll until it claims, idles or stops.
volatile LONG   s_bgWaits      = 0;
volatile LONG64 s_bgWaitUs     = 0;
volatile LONG64 s_bgWaitMaxUs  = 0;
volatile LONG   s_bgWaitHist[8];      // <1,<2,<5,<10,<20,<50,<200,>=200 ms
volatile LONG   s_bgSkip       = 0;   // the bg thread ran a later job past a blocked one
volatile LONG   s_bgIdle       = 0;   // episodes that ended in threadProc's sleep
volatile LONG   s_bgPassFull   = 0;   // wait episodes (or claims) whose scan hit the passed-job cap
// The longest time a bg-only job stayed runnable and unclaimed, as the bg
// thread saw it: what a stalled bg thread shows as.
volatile LONG64 s_bgoMaxUs     = 0;
volatile LONG   s_bgoClaims    = 0;
volatile LONG   s_full         = 0;
volatile LONG   s_resAge       = 0;
volatile LONG   s_resDrop      = 0;
volatile LONG   s_obsFreed     = 0;
volatile LONG   s_obsOverflow  = 0;
volatile LONG   s_obsOtherNmg  = 0;
volatile LONG   s_liveMax      = 0;
volatile LONG   s_pubMax       = 0;
volatile LONG   s_checks       = 0;
volatile LONG   s_viol         = 0;
volatile LONG   s_violCell     = 0;
volatile LONG   s_spanOut      = 0;
volatile LONG   s_unowned      = 0;
volatile LONG   s_torn         = 0;
volatile LONG   s_violLines    = 0;

double s_nextBeat = 0.0;

LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }
LONG64 Read64(volatile LONG64* p) { return InterlockedCompareExchange64(p, 0, 0); }

void NoteMax64(volatile LONG64* slot, LONG64 v)
{
	for (;;)
	{
		LONG64 cur = Read64(slot);
		if (v <= cur || InterlockedCompareExchange64(slot, v, cur) == cur)
			return;
	}
}

void NoteMax(volatile LONG* slot, LONG v)
{
	for (;;)
	{
		LONG cur = Read(slot);
		if (v <= cur || InterlockedCompareExchange(slot, v, cur) == cur)
			return;
	}
}


void Wake()
{
	if (g_adjEvent)
		SetEvent(g_adjEvent);
	NavMeshWakeWorkersIfQueued();
}

} // namespace nm_adjacency_detail
using namespace nm_adjacency_detail;
bool NmAdjActive()    { return s_mode != MODE_OFF; }
bool NmAdjEnforcing() { return s_mode == MODE_ENFORCE; }

namespace nm_adjacency_detail {
bool ReadU32Guarded(const void* at, unsigned int* out)
{
	__try { *out = *(const unsigned int*)at; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace nm_adjacency_detail
using namespace nm_adjacency_detail;

void NmAdjDescribeTask(uintptr_t task, NmJobDesc* out)
{
	uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(4, task, NavMeshGenerator__Task_zone, 0);
	int x = zone ? *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X)) : -1000;
	int y = zone ? *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y)) : -1000;
	int type = *(int*)(KLIB_MEMBER(4, task, NavMeshGenerator__Task_flags, 88)) & 7;

	bool haveUid = false;
	unsigned int uid = 0;
	const float* box = NULL;
	if (type == 4)
	{
		// addStitchJob sets both the output (the live instance it re-stitches)
		// and the bounds; a type-3 task's bounds are filled only in updateBT.
		uintptr_t output = *(uintptr_t*)(KLIB_MEMBER(4, task, NavMeshGenerator__Task_output, 80));
		if (output)
			haveUid = ReadU32Guarded((const void*)KLIB_MEMBER(4, output, NavInstance_uid, 0x3C), &uid);
		box = (const float*)(KLIB_MEMBER(4, task, NavMeshGenerator__Task_bounds, 48));
	}
	NmAdjDescribe(x, y, type, haveUid, uid, box, s_cellX, s_cellZ, out);
}

uintptr_t NmAdjPinnedLocked(const NmQueueLock&)
{
	return (uintptr_t)g_reg.pin;
}

namespace nm_adjacency_detail {

static bool ReadCellSize(size_t rva, float* out)
{
	float v = *(const float*)GameAddr(rva);
	if (!(v >= 100.0f && v <= 100000.0f))
		return false;
	*out = v;
	return true;
}
} // namespace nm_adjacency_detail

void InstallNavMeshAdjacency(int* installed, int*)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart ? f.QuadPart : 1;
	NmAdjInit(&g_reg);
	g_adjEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

	if (!ReadCellSize(RVA_SUBMAP_CELL_X, &s_cellX) || !ReadCellSize(RVA_SUBMAP_CELL_Z, &s_cellZ))
	{
		s_cellX = s_cellZ = 0.0f;   // interiors keep the 8-neighbourhood
		ErrorLog("NavMesh adjacency: the sub-map cell size is unreadable; a type-4 interior's box is not widened");
	}

	s_observerWhy = HookInstall(HOOK_NMG_UPDATE, hook_nmgUpdate, &orig_nmgUpdate,
	                            installed, true);
	s_observerInstalled = (s_observerWhy == NULL);
	if (!s_observerInstalled)
		orig_nmgUpdate = NULL;

	const bool enforceWanted = navmesh::g_navmeshCfg.navmeshAdjExclusionEnabled;
	if (!g_adjEvent)
	{
		s_mode = MODE_COUNT_NO_OBSERVER;
		s_modeWhy = "no wait event";
	}
	else if (!s_observerInstalled)
	{
		// Without the drain end no entry could ever be released: windows end at
		// the publish, and nothing is deferred.
		s_mode = MODE_COUNT_NO_OBSERVER;
		s_modeWhy = "no drain observer";
	}
	else if (enforceWanted)
	{
		s_mode = MODE_ENFORCE;
		s_modeWhy = "";
	}
	else
	{
		s_mode = MODE_COUNT;
		s_modeWhy = "navmeshAdjExclusion=false";
	}

	if (s_observerInstalled)
		LogMsg(std::string("NavMesh adjacency: installed, mode=")
		       + (s_mode == MODE_ENFORCE ? "enforce" : "count")
		       + (s_modeWhy[0] ? std::string(" (") + s_modeWhy + ")" : std::string())
		       + " (a heartbeat line follows every minute)");
	else
		ErrorLog(std::string("NavMesh adjacency: drain observer not installed (") + s_observerWhy
		         + "); adjacent navmesh jobs are counted, not deferred");
	s_nextBeat = 0.0;
	NavMeshAdjTick(ElapsedSec());
}

