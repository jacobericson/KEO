// wall_splice_drain.cpp - The main-thread half of the deferred wall splice: the ring's records
// coalesced per cell into a pending table, each issued through NavMesh::generate(Aabb) only once
// physics has applied the hull group switch and every cell it touches is ready; the heartbeat;
// and the DEV count of type-1 jobs that start while a switch is pending.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "navmesh/construction/wall_splice.h"
#include "navmesh/construction/wall_splice_internal.h"
#include "navmesh/construction/splice_queue_policy.h"
#include "navmesh/nm_workers.h"
#include "zone/grid.h"
#include "zone/transition.h"
#include "zone/reset/zone_reset_gate.h"
#include "game/game.h"
#include "base/core.h"
#include "base/fixed_log_buf.h"
#include "fixes/guard_report.h"
#include <windows.h>

namespace wall_splice_drain_detail {
struct PendingSplice { float box[6]; int cellX, cellY; int ticksWaited; int used; };

// What the tick reads once per frame, before it judges any pending box.
struct GateSnapshot
{
	int        mainCount;
	int        backCount;
	bool       queuesClear;
	bool       worldOk;
	void*      zoneMgr;
	uintptr_t  sectionMgr;
	isContentPending_t isReady;
};
} // namespace wall_splice_drain_detail
using namespace wall_splice_drain_detail;

namespace navmesh {

static const int kPendingSlots = 64;
static const int kIssuePerTick = 8;
static const int kBeatSeconds  = 60;

// Main thread only, but for s_waiting, which a heartbeat row points at.
static PendingSplice     s_pending[kPendingSlots];
static volatile LONG     s_waiting = 0;
static bool              s_wasLoading = false;
static volatile LONGLONG s_nextBeat = 0;
static LONGLONG          s_qpf = 0;
static LONG              s_lastPrinted = 0;
#ifdef KEO_DEBUG
static volatile LONG     s_issueLines = 0;
static const LONG        kIssueLineCap = 32;
#endif

static const GuardCounter kBeatRows[] =
{
	{ "recorded",    GF_COUNT, &g_wallSplice.recorded,    0 },
	{ "lost",        GF_COUNT, &g_wallSplice.ring.lost,   0 },
	{ "merged",      GF_COUNT, &g_wallSplice.merged,      0 },
	{ "issued",      GF_COUNT, &g_wallSplice.issued,      0 },
	{ "waiting",     GF_COUNT, &s_waiting,                0 },
	{ "requeued",    GF_COUNT, &g_wallSplice.requeued,    0 },
	{ "droppedGone", GF_COUNT, &g_wallSplice.droppedGone, 0 },
	{ "droppedLoad", GF_COUNT, &g_wallSplice.droppedLoad, 0 },
	{ "pendingFull", GF_COUNT, &g_wallSplice.pendingFull, 0 },
};
#ifdef KEO_DEBUG
static const GuardCounter kBeatRowsDev[] =
{
	{ "t1Start",  GF_COUNT, &g_wallSplice.t1Start,  0 },
	{ "t1Racing", GF_COUNT, &g_wallSplice.t1Racing, 0 },
};
#endif

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// A save load drops every pending box (its cells belong to the world being cleared) and every
// record that arrives while it runs. True while loading: the tick does nothing else.
static bool DropWhileLoading(bool saveLoading)
{
	if (!saveLoading)
	{
		s_wasLoading = false;
		return false;
	}
	if (!s_wasLoading)
	{
		for (int i = 0; i < kPendingSlots; ++i)
		{
			if (!s_pending[i].used)
				continue;
			s_pending[i].used = 0;
			InterlockedIncrement(&g_wallSplice.droppedLoad);
		}
		s_wasLoading = true;
	}
	float discard[SPLICE_RING_SLOTS][6];
	for (int pass = 0; pass < 4; ++pass)
	{
		const int n = SpliceRingDrain(&g_wallSplice.ring, discard, SPLICE_RING_SLOTS);
		if (n == 0)
			break;
		InterlockedExchangeAdd(&g_wallSplice.droppedLoad, n);
	}
	return true;
}

// The ring's records into the pending table: merged into an entry of the same cell while the
// union stays under the extent, else a free entry, else dropped and counted. The grid must be
// calibrated (the caller's check).
static void DrainIntoPending()
{
	float recs[SPLICE_RING_SLOTS][6];
	const int n = SpliceRingDrain(&g_wallSplice.ring, recs, SPLICE_RING_SLOTS);
	for (int r = 0; r < n; ++r)
	{
		int cx = 0, cy = 0;
		WorldToZoneGrid(recs[r][0], recs[r][2], &cx, &cy);
		bool placed = false;
		for (int i = 0; i < kPendingSlots && !placed; ++i)
		{
			PendingSplice& e = s_pending[i];
			if (e.used && SpliceCoalesce(e.box, e.cellX, e.cellY, recs[r], cx, cy))
			{
				InterlockedIncrement(&g_wallSplice.merged);
				placed = true;
			}
		}
		for (int i = 0; i < kPendingSlots && !placed; ++i)
		{
			PendingSplice& e = s_pending[i];
			if (e.used)
				continue;
			for (int k = 0; k < 6; ++k)
				e.box[k] = recs[r][k];
			e.cellX = cx;
			e.cellY = cy;
			e.ticksWaited = 0;
			e.used = 1;
			placed = true;
		}
		if (!placed)
			InterlockedIncrement(&g_wallSplice.pendingFull);
	}
}

static bool AnyPending()
{
	for (int i = 0; i < kPendingSlots; ++i)
		if (s_pending[i].used)
			return true;
	return false;
}

static GateSnapshot TakeSnapshot()
{
	GateSnapshot s;
	s.mainCount = 0;
	s.backCount = 0;
	s.queuesClear = false;
	s.zoneMgr = g_cachedZoneMgr;
	s.sectionMgr = *(uintptr_t*)GameAddr(RVA_GLOBAL_SECTION_MGR);
	s.isReady = game::g_hookOrig.orig_isContentPending
		? game::g_hookOrig.orig_isContentPending
		: (isContentPending_t)GameAddr(RVA_IS_CONTENT_PENDING);

	const uintptr_t physics = *(uintptr_t*)GameAddr(RVA_PAUSESTATE_PHYSICS);
	if (physics)
	{
		s.mainCount = *(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToChangeGroup_mainThreadData_count, 568));
		s.backCount = *(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToChangeGroup_backThreadData_count, 592));
		s.queuesClear = g_wallSplice.fn_queuesAreClearMT((void*)physics);
	}
	s.worldOk = !ZoneResetGateUp(&g_zoneResetGate) && !NavMeshStopSeen() && !isTransitionActive
	         && s.zoneMgr != NULL && s.sectionMgr != 0;
	return s;
}

// The cells a box touches, from its two corners on x and z.
static void TouchedCells(const float box[6], int* x0, int* y0, int* x1, int* y1)
{
	int ax = 0, ay = 0, bx = 0, by = 0;
	WorldToZoneGrid(box[0] - box[3], box[2] - box[5], &ax, &ay);
	WorldToZoneGrid(box[0] + box[3], box[2] + box[5], &bx, &by);
	*x0 = ax < bx ? ax : bx;
	*x1 = ax < bx ? bx : ax;
	*y0 = ay < by ? ay : by;
	*y1 = ay < by ? by : ay;
}

// Gone: a touched cell neither loading nor accessible. Two flag reads per cell, no lock.
static bool AnyCellGone(void* zoneMgr, int x0, int y0, int x1, int y1)
{
	for (int x = x0; x <= x1; ++x)
		for (int y = y0; y <= y1; ++y)
		{
			void* zone = GetZoneEntry(zoneMgr, x, y);
			if (!zone || (!IsZoneLoading(zone) && !IsZoneAccessible(zone)))
				return true;
		}
	return false;
}

// Ready: every touched cell accessible (so game-owned, never a private one) and ready by the
// original isContentPending, which takes the section manager's +0x1E0 lock, as vanilla's own
// main-thread callers do.
static bool AllCellsReady(const GateSnapshot& s, int x0, int y0, int x1, int y1)
{
	for (int x = x0; x <= x1; ++x)
		for (int y = y0; y <= y1; ++y)
		{
			void* zone = GetZoneEntry(s.zoneMgr, x, y);
			if (!zone || !IsZoneAccessible(zone))
				return false;
			int coords[2] = { x, y };
			if (!s.isReady((void*)s.sectionMgr, (void*)coords))
				return false;
		}
	return true;
}

#ifdef KEO_DEBUG
static void LogIssue(const PendingSplice& e, int x0, int y0, int x1, int y1)
{
	if (!GuardFireClaim(&s_issueLines, kIssueLineCap))
		return;
	FixedLogBuf o;
	FlbInit(&o);
	FlbStr(&o, "WallSplice: issued centre=");
	FlbDec(&o, (__int64)e.box[0]); FlbChar(&o, ','); FlbDec(&o, (__int64)e.box[1]); FlbChar(&o, ',');
	FlbDec(&o, (__int64)e.box[2]);
	FlbStr(&o, " half=");
	FlbDec(&o, (__int64)e.box[3]); FlbChar(&o, ','); FlbDec(&o, (__int64)e.box[4]); FlbChar(&o, ',');
	FlbDec(&o, (__int64)e.box[5]);
	FlbStr(&o, " cells=");
	FlbDec(&o, x0); FlbChar(&o, ','); FlbDec(&o, y0); FlbStr(&o, "..");
	FlbDec(&o, x1); FlbChar(&o, ','); FlbDec(&o, y1);
	FlbStr(&o, " waited="); FlbDec(&o, e.ticksWaited);
	LogMsg(FlbDone(&o));
}
#endif

// Judges every pending box against one snapshot. The readiness query, which takes a lock, runs
// only for a box every cheaper test already passed; at most kIssuePerTick are issued per tick,
// and the rest wait unjudged for the next one.
static void EvaluatePending()
{
	if (!AnyPending())
		return;
	const GateSnapshot s = TakeSnapshot();
	int issuedNow = 0;
	for (int i = 0; i < kPendingSlots && issuedNow < kIssuePerTick; ++i)
	{
		PendingSplice& e = s_pending[i];
		if (!e.used)
			continue;
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		TouchedCells(e.box, &x0, &y0, &x1, &y1);

		SpliceGate g;
		g.mainCount = s.mainCount;
		g.backCount = s.backCount;
		g.queuesClear = s.queuesClear;
		g.worldOk = s.worldOk;
		g.cellGone = s.zoneMgr != NULL && AnyCellGone(s.zoneMgr, x0, y0, x1, y1);
		g.cellsReady = !g.cellGone && g.worldOk && g.mainCount == 0 && g.backCount == 0 && g.queuesClear
		            && AllCellsReady(s, x0, y0, x1, y1);

		switch (SpliceDecide(g, e.ticksWaited))
		{
		case SQ_ISSUE:
			g_wallSplice.fn_navMeshGenerateAabb((void*)s.sectionMgr, e.box);
			InterlockedIncrement(&g_wallSplice.issued);
#ifdef KEO_DEBUG
			LogIssue(e, x0, y0, x1, y1);
#endif
			e.used = 0;
			++issuedNow;
			break;
		case SQ_WAIT:
			if (g.worldOk)          // a world not ok waits without aging
				++e.ticksWaited;
			break;
		case SQ_REQUEUE:
			e.ticksWaited = 0;
			InterlockedIncrement(&g_wallSplice.requeued);
			break;
		case SQ_DROP:
			e.used = 0;
			InterlockedIncrement(&g_wallSplice.droppedGone);
			break;
		}
	}
}

static void Heartbeat()
{
	LONG waiting = 0;
	for (int i = 0; i < kPendingSlots; ++i)
		if (s_pending[i].used)
			++waiting;
	InterlockedExchange(&s_waiting, waiting);

	if (s_qpf == 0)
	{
		LARGE_INTEGER f;
		s_qpf = QueryPerformanceFrequency(&f) ? f.QuadPart : -1;
	}
	if (!GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds))
		return;
	LONG print = 0;
	for (int i = 0; i < (int)ARRAYSIZE(kBeatRows); ++i)
		print += Read(kBeatRows[i].value);
#ifdef KEO_DEBUG
	for (int i = 0; i < (int)ARRAYSIZE(kBeatRowsDev); ++i)
		print += Read(kBeatRowsDev[i].value);
#endif
	if (print == s_lastPrinted)
		return;
	s_lastPrinted = print;
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "WallSplice:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
#ifdef KEO_DEBUG
	GuardFields(&o, kBeatRowsDev, (int)ARRAYSIZE(kBeatRowsDev), true);
#endif
	LogMsg(FlbDone(&o));
}

void WallSpliceTick(double /*now*/, bool saveLoading)
{
	if (!InterlockedCompareExchange(&g_wallSplice.armed, 0, 0)) return;
	if (DropWhileLoading(saveLoading))
		return;
	// A record needs its cell, and the grid calibrates later in the first frame of a world: until
	// then the records wait in the ring.
	if (gridCalibrated)
	{
		DrainIntoPending();
		EvaluatePending();
	}
	Heartbeat();
}

// A navmesh thread. Counts and reads only, without a lock: a racy read for a diagnostic, never a
// decision. Runs whatever the key says, so vanilla's own splices are counted too.
void WallSpliceNoteType1Start()
{
	InterlockedIncrement(&g_wallSplice.t1Start);
	const uintptr_t physics = *(volatile uintptr_t*)GameAddr(RVA_PAUSESTATE_PHYSICS);
	if (!physics)
		return;
	const int mainCount = *(volatile int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToChangeGroup_mainThreadData_count, 568));
	const int backCount = *(volatile int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToChangeGroup_backThreadData_count, 592));
	const unsigned char clear = *(volatile unsigned char*)(KLIB_MEMBER(2, physics, PhysicsInterface__queuesClear, 0x320));
	if (mainCount != 0 || backCount != 0 || clear == 0)
		InterlockedIncrement(&g_wallSplice.t1Racing);
}

} // namespace navmesh
