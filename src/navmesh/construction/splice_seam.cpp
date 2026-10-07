// splice_seam.cpp - A navmesh patch box across a cell border. NavMesh::generate(Aabb) gives such a
// box one clipped patch per cell, and the two patches' border edges never join; the detour instead
// regenerates every eligible cell the box crosses in full through the rebuild key's forced jobs. On
// the main thread it acts at once; from any other thread the box is held in a ring and the camera
// tick acts on it behind the wall splice's gate.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "navmesh/construction/splice_seam.h"
#include "navmesh/construction/splice_seam_policy.h"
#include "navmesh/construction/wall_splice_internal.h"
#include "navmesh/cache/nm_force_rebuild.h"
#include "navmesh/nm_workers.h"
#include "game/game.h"
#include "base/core.h"
#include "base/fixed_log_buf.h"
#include "fixes/guard_report.h"
#include "plugin/hook_manifest.h"
#include "plugin/hook_manifest_policy.h"
#include <windows.h>
#include <string.h>
#include <string>

namespace splice_seam_detail {
struct SeamPending { float box[6]; int used; };
struct SpliceSeamState
{
	navmesh::SpliceRing ring;
	volatile LONG armed, boxes, forced, fallback, skipped, deferred, ringFull, droppedLoad, passed, original;
	volatile LONG lostNet;   // ring.lost - ring.claimFailed, floored at 0; the tick's own, before the heartbeat
};
typedef void (__fastcall *addJobAabb_t)(void* nmg, void* zone, const float* box);
} // namespace splice_seam_detail
using namespace splice_seam_detail;

namespace navmesh {

static_assert(SEAM_MAX_CELLS <= NM_REBUILD_MAX_CELLS, "a span's cells fit one NmForceRebuildCells call");

static const int kSeamPendingSlots = 16;
static const int kSeamActPerTick   = 4;
static const int kSeamBeatSeconds  = 60;

// POD, zero-initialised. The ring is initialised before the row installs; the counters are
// interlocked; the pending table and the heartbeat state are the main thread's.
static SpliceSeamState       s_seam;
static SeamPending           s_pending[kSeamPendingSlots];
static bool                  s_wasLoading = false;
static double                s_nextBeatAt = 0.0;
static LONG                  s_lastPrinted = 0;
static navMeshGenerateAabb_t orig_navMeshGenerateAabb = NULL;
static queuesAreClearMT_t    s_queuesAreClearMT = NULL;
static addJobAabb_t          s_addJobAabb = NULL;
#ifdef KEO_DEBUG
static volatile LONG         s_boxLines = 0;
static const LONG            kBoxLineCap = 32;
#endif

// The two callees' first bytes in this build: the row installs only when both match.
static const unsigned char kSeamQueuesAreClearHead[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x83,0xB9,0xB0,0x01,0x00,0x00,0x00,0x48,0x8B,0xD9 };
static const unsigned char kAddJobAabbHead[16] =
	{ 0x40,0x55,0x56,0x57,0x48,0x83,0xEC,0x70,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF };

static const GuardCounter kSeamBeatRows[] =
{
	{ "boxes",       GF_COUNT, &s_seam.boxes,            0 },
	{ "forced",      GF_COUNT, &s_seam.forced,           0 },
	{ "fallback",    GF_COUNT, &s_seam.fallback,         0 },
	{ "skipped",     GF_COUNT, &s_seam.skipped,          0 },
	{ "deferred",    GF_COUNT, &s_seam.deferred,         0 },
	{ "ringFull",    GF_COUNT, &s_seam.ringFull,         0 },
	{ "lost",        GF_COUNT, &s_seam.lostNet,          0 },
	{ "droppedLoad", GF_COUNT, &s_seam.droppedLoad,      0 },
	{ "claimFailed", GF_COUNT, &s_seam.ring.claimFailed, 0 },
	{ "passed",      GF_COUNT, &s_seam.passed,           0 },
	{ "original",    GF_COUNT, &s_seam.original,         0 },
};

// The engine's cell grid as this NavMesh holds it: three plain loads, no call.
static SeamGrid GridOf(const void* navMesh)
{
	const unsigned char* m = (const unsigned char*)navMesh;
	SeamGrid g;
	g.originX  = *(const float*)(m + KLIB_OFF_NavMesh_worldX);
	g.originZ  = *(const float*)(m + KLIB_OFF_NavMesh_worldY);
	g.cellSize = *(const float*)(m + KLIB_OFF_NavMesh_cellSize);
	return g;
}

#ifdef KEO_DEBUG
static const char* CellName(int action, const NmForceCellResult& r)
{
	if (action == SEAM_CELL_FORCE)
		return r.verdict == NM_FORCE_CELL_QUEUED ? "forced" : "covered";
	return action == SEAM_CELL_PARTIAL ? "partial" : "skip";
}

static void LogBox(const void* navMesh, const float* box, const SeamSpan& s, const int* action,
                   const NmForceCellResult* r)
{
	if (!GuardFireClaim(&s_boxLines, kBoxLineCap))
		return;
	const SeamGrid g = GridOf(navMesh);
	FixedLogBuf o;
	FlbInit(&o);
	FlbStr(&o, "SpliceSeam: box centre=");
	FlbDec(&o, (__int64)box[0]); FlbChar(&o, ','); FlbDec(&o, (__int64)box[1]); FlbChar(&o, ',');
	FlbDec(&o, (__int64)box[2]);
	FlbStr(&o, " half=");
	FlbDec(&o, (__int64)box[3]); FlbChar(&o, ','); FlbDec(&o, (__int64)box[4]); FlbChar(&o, ',');
	FlbDec(&o, (__int64)box[5]);
	FlbStr(&o, " grid=");
	FlbDec(&o, (__int64)g.originX); FlbChar(&o, ','); FlbDec(&o, (__int64)g.originZ); FlbChar(&o, ',');
	FlbDec(&o, (__int64)g.cellSize);
	FlbStr(&o, " cells="); FlbDec(&o, s.count);
	for (int i = 0; i < s.count; ++i)
	{
		FlbStr(&o, " ("); FlbDec(&o, s.x[i]); FlbChar(&o, ','); FlbDec(&o, s.z[i]); FlbStr(&o, ")=");
		FlbStr(&o, CellName(action[i], r[i]));
	}
	LogMsg(FlbDone(&o));
}
#endif

// Main thread, no mod lock held on entry: the forced entry takes the queue lock briefly with no
// call under it, and addJob takes it itself. Each cell of the span forced, patched as vanilla
// would, or skipped. False only without a generator, so the caller hands the box to the original.
static bool SeamAct(void* navMesh, const float* box, const SeamSpan& s)
{
	const uintptr_t nmg = *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)navMesh, NavMesh_generator, OFF_MGR_NAVMESH_GEN));
	if (!nmg)
		return false;
	NmRebuildCell cells[SEAM_MAX_CELLS];
	for (int i = 0; i < s.count; ++i)
	{
		cells[i].gx = s.x[i];
		cells[i].gy = s.z[i];
	}
	NmForceCellResult r[SEAM_MAX_CELLS];
	memset(r, 0, sizeof(r));
	NmForceRebuildCells(navMesh, cells, s.count, r);
	int action[SEAM_MAX_CELLS];
	for (int i = 0; i < s.count; ++i)
	{
		action[i] = SeamCellOf(r[i].haveZone, r[i].terrain, r[i].skip);
		if (action[i] == SEAM_CELL_FORCE)
			InterlockedIncrement(&s_seam.forced);
		else if (action[i] == SEAM_CELL_PARTIAL)
		{
			float clip[6];
			SeamClip(box, (const float*)(KLIB_MEMBER(4, (uintptr_t)r[i].zone, ZoneMap_bounds, OFF_ZONE_AABB_CENTER)), clip);
			s_addJobAabb((void*)nmg, r[i].zone, clip);
			InterlockedIncrement(&s_seam.fallback);
		}
		else
			InterlockedIncrement(&s_seam.skipped);
	}
	InterlockedIncrement(&s_seam.boxes);
#ifdef KEO_DEBUG
	LogBox(navMesh, box, s, action, r);
#endif
	return true;
}

// Any thread. True when the box was taken: rebuilt in full on the main thread, or held in the
// ring for the camera tick from any other. Off the main thread: no lock, no allocation, no log.
// fromRing: the tick's own call for a ring box, whose pass to the original the caller counts.
static bool SeamTake(void* navMesh, const float* box, bool fromRing)
{
	if (!navMesh || !box)
		return false;
	SeamSpan s;
	SeamSpanOf(box, GridOf(navMesh), &s);
	const bool mainThread = IsMainThread();
	switch (SeamRouteOf(s, mainThread, mainThread && SpliceWorldOk()))
	{
	case SEAM_RING:
		switch (SpliceRingTryPush(&s_seam.ring, box))
		{
		case SPLICE_PUSH_TAKEN:
			InterlockedIncrement(&s_seam.deferred);
			return true;
		case SPLICE_PUSH_FULL:
			InterlockedIncrement(&s_seam.ringFull);
			return false;
		default:
			return false;      // the claim failed: counted once, in the ring's claimFailed
		}
	case SEAM_ACT:
		if (SeamAct(navMesh, box, s))
			return true;
		break;                 // no generator
	default:
		if (s.overflow || s.count <= 1)
			return false;      // the original's own box
		break;                 // the main thread with the world not ok
	}
	if (!fromRing)
		InterlockedIncrement(&s_seam.passed);
	return false;
}

// Any thread: one original call unless the box was taken; a ring box the tick passes back to
// the original is counted.
static void SeamCall(void* navMesh, const float* box, bool fromRing)
{
	if (SeamTake(navMesh, box, fromRing))
		return;
	if (fromRing)
		InterlockedIncrement(&s_seam.original);
	orig_navMeshGenerateAabb(navMesh, box);
}

// NavMesh::generate(Aabb), any thread.
static void __fastcall hook_navMeshGenerateAabb(void* navMesh, const float* box)
{
	SeamCall(navMesh, box, false);
}

void InstallSpliceSeam(int* installed, int*)
{
	if (!HookRowWanted(HOOK_NAVMESH_GENERATE_AABB))
	{
		const HookWantInputs in = HookWantInputsFromConfig();
		if (in.spliceSeam && !in.wallSplice)
			LogMsg("SpliceSeam: not armed (wallSpliceFix off)");
		else if (in.spliceSeam && !in.caching)
			LogMsg("SpliceSeam: not armed (caching off)");
		return;
	}

	const char* why = NULL;
	if (memcmp((const void*)GameAddr(RVA_QUEUES_ARE_CLEAR_MT), kSeamQueuesAreClearHead, 16) != 0)
		why = "queuesAreClearMT";
	else if (memcmp((const void*)GameAddr(RVA_NMG_ADD_JOB_AABB), kAddJobAabbHead, 16) != 0)
		why = "addJobAabb";
	else if (NmForceRebuildBindQueue(&why))
	{
		// Before the install: the detour can run on any thread as soon as it is in.
		SpliceRingInit(&s_seam.ring);
		s_queuesAreClearMT = (queuesAreClearMT_t)GameAddr(RVA_QUEUES_ARE_CLEAR_MT);
		s_addJobAabb = (addJobAabb_t)GameAddr(RVA_NMG_ADD_JOB_AABB);
		why = HookInstall(HOOK_NAVMESH_GENERATE_AABB, hook_navMeshGenerateAabb, &orig_navMeshGenerateAabb, installed, true);
	}

	if (!why)
	{
		InterlockedExchange(&s_seam.armed, 1);
		LogMsg("SpliceSeam: armed (a box across a cell border regenerates its cells in full; a heartbeat line follows in any minute a counter moved)");
		return;
	}
	orig_navMeshGenerateAabb = NULL;
	LogError(std::string("SpliceSeam: not installed (") + why + "); a box across a cell border splices as vanilla");
}

// A save load drops every held box (its cells belong to the world being cleared) and every box
// that arrives while it runs. True while loading: the tick only beats.
static bool DropHeld(bool saveLoading)
{
	if (!saveLoading)
	{
		s_wasLoading = false;
		return false;
	}
	if (!s_wasLoading)
	{
		for (int i = 0; i < kSeamPendingSlots; ++i)
		{
			if (!s_pending[i].used)
				continue;
			s_pending[i].used = 0;
			InterlockedIncrement(&s_seam.droppedLoad);
		}
		s_wasLoading = true;
	}
	float discard[SPLICE_RING_SLOTS][6];
	for (int pass = 0; pass < 4; ++pass)
	{
		const int n = SpliceRingDrain(&s_seam.ring, discard, SPLICE_RING_SLOTS);
		if (n == 0)
			break;
		InterlockedExchangeAdd(&s_seam.droppedLoad, n);
	}
	return true;
}

// The ring's boxes into the free pending slots only: while the table is full a box waits in the
// ring.
static void DrainIntoPending()
{
	int freeSlots = 0;
	for (int i = 0; i < kSeamPendingSlots; ++i)
		if (!s_pending[i].used)
			++freeSlots;
	if (freeSlots == 0)
		return;
	float recs[kSeamPendingSlots][6];
	const int n = SpliceRingDrain(&s_seam.ring, recs, freeSlots);
	int slot = 0;
	for (int r = 0; r < n; ++r)
	{
		while (s_pending[slot].used)
			++slot;
		for (int k = 0; k < 6; ++k)
			s_pending[slot].box[k] = recs[r][k];
		s_pending[slot].used = 1;
	}
}

// One snapshot for the tick; each held box judged against it, the readiness query only once every
// cheaper test passed; at most kSeamActPerTick acted on, the rest wait for the next tick.
static void JudgePending()
{
	bool any = false;
	for (int i = 0; i < kSeamPendingSlots && !any; ++i)
		any = s_pending[i].used != 0;
	if (!any)
		return;
	const SpliceGateSnapshot snap = SpliceGateTake(s_queuesAreClearMT);
	if (!snap.sectionMgr)
		return;
	int actedNow = 0;
	for (int i = 0; i < kSeamPendingSlots && actedNow < kSeamActPerTick; ++i)
	{
		SeamPending& e = s_pending[i];
		if (!e.used)
			continue;
		SeamSpan s;
		SeamSpanOf(e.box, GridOf((const void*)snap.sectionMgr), &s);

		SpliceGate g;
		g.mainCount = snap.mainCount;
		g.backCount = snap.backCount;
		g.queuesClear = snap.queuesClear;
		g.worldOk = snap.worldOk;
		g.cellGone = false;
		for (int c = 0; snap.zoneMgr && c < s.count && !g.cellGone; ++c)
			g.cellGone = SpliceCellGone(snap.zoneMgr, s.x[c], s.z[c]);
		g.cellsReady = !g.cellGone && g.worldOk && g.mainCount == 0 && g.backCount == 0 && g.queuesClear;
		for (int c = 0; g.cellsReady && c < s.count; ++c)
			g.cellsReady = SpliceCellReady(snap, s.x[c], s.z[c]);

		if (!SeamDeferredActs(g))
			continue;
		SeamCall((void*)snap.sectionMgr, e.box, true);
		e.used = 0;
		++actedNow;
	}
}

static void Heartbeat(double now)
{
	LONG net = InterlockedCompareExchange(&s_seam.ring.lost, 0, 0) - InterlockedCompareExchange(&s_seam.ring.claimFailed, 0, 0);
	InterlockedExchange(&s_seam.lostNet, net > 0 ? net : 0);

	if (now < s_nextBeatAt)
		return;
	s_nextBeatAt = now + kSeamBeatSeconds;
	LONG print = 0;
	for (int i = 0; i < (int)ARRAYSIZE(kSeamBeatRows); ++i)
		print += InterlockedCompareExchange(kSeamBeatRows[i].value, 0, 0);
	if (print == s_lastPrinted)
		return;
	s_lastPrinted = print;
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "SpliceSeam:");
	GuardFields(&o, kSeamBeatRows, (int)ARRAYSIZE(kSeamBeatRows), true);
	LogMsg(FlbDone(&o));
}

void SpliceSeamTick(double now, bool saveLoading)
{
	if (!InterlockedCompareExchange(&s_seam.armed, 0, 0)) return;
	if (!DropHeld(saveLoading))
	{
		DrainIntoPending();
		JudgePending();
	}
	Heartbeat(now);
}

} // namespace navmesh
