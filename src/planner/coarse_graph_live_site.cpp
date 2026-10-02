// coarse_graph_live_site.cpp - The live overlay's game side: the graph-instance connect's call into
// the walk, its refusal counters, and the PlannerLive: line. The connect side runs on the path
// thread inside NavMesh::update's exclusive world lock; it takes no lock of its own, allocates
// nothing and logs nothing (interlocked counters only). The report runs on the main thread.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/coarse_graph_live.h"
#include "planner/coarse_graph.h"
#include "game/game.h"
#include "base/core.h"
#include <stdio.h>
#include "base/klib_include.h"
#include <core/Functions.h>
#include "base/klib_include_end.h"

namespace planner {

// Each connect the store was ready for counts once: in copies, or in exactly one refusal.
static volatile LONG s_copies         = 0;
static volatile LONG s_overCap        = 0;   // over the node, arc or border capacity
static volatile LONG s_noBuffer       = 0;   // the live pool was empty
static volatile LONG s_noMesh         = 0;   // no mesh instance, original mesh or graph array
static volatile LONG s_badSlot        = 0;   // the graph instance's slot is past the collection
static volatile LONG s_noShift        = 0;   // no section manager, or its world shift pointer is NULL
static volatile LONG s_bordersSkipped = 0;
static volatile LONG s_noFaceData     = 0;   // copies whose section's face data could not be read
static volatile LONG s_arcsTrunc      = 0;   // summed over the copied buffers
static volatile LONG s_waterNodes     = 0;   // nodes with a non-zero water byte, summed over the copied buffers
static volatile LONG s_maxNodes       = 0;
static volatile LONG s_maxBorders     = 0;

// Main thread only: the report's timer and the copies-plus-skipped sum it last printed.
static double s_lastReport = 0.0;
static LONG   s_lastMoved  = 0;

static void RaiseMax(volatile LONG* m, LONG v)
{
	LONG cur = *m;
	while (v > cur)
	{
		LONG seen = InterlockedCompareExchange(m, v, cur);
		if (seen == cur)
			break;
		cur = seen;
	}
}

// The section manager's world shift: a pointer to the Havok frame's offset, read once a section.
static const float* WorldShift()
{
	uintptr_t mgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!mgr)
		return NULL;
	return *(const float* const*)(KLIB_MEMBER(4, mgr, NavMesh_worldShift, 0x1D8));
}

static void CountRefusal(CgLiveResult r)
{
	switch (r)
	{
	case CGL_BAD_SLOT: InterlockedIncrement(&s_badSlot); break;
	case CGL_NO_MESH:  InterlockedIncrement(&s_noMesh);  break;
	default:           InterlockedIncrement(&s_overCap); break;
	}
}

void CgLiveOnConnect(void* graphInst, void* coll)
{
	if (!CgStoreReady()) return;
	const float* shiftPtr = WorldShift();
	if (!shiftPtr)
	{
		InterlockedIncrement(&s_noShift);
		return;
	}
	const float shift[3] = { shiftPtr[0], shiftPtr[1], shiftPtr[2] };
	CgBlock* buf = CgLiveAcquire();
	if (!buf)
	{
		InterlockedIncrement(&s_noBuffer);
		return;
	}
	CgLiveCounts counts;
	CgLiveResult r = CgLiveCopy(graphInst, coll, shift, buf, &counts);
	InterlockedExchangeAdd(&s_bordersSkipped, counts.bordersSkipped);
	InterlockedExchangeAdd(&s_noFaceData, counts.noFaceData);
	if (r != CGL_OK)
	{
		CountRefusal(r);
		CgLiveRelease(buf);
		return;
	}
	// Read before the post: from the exchange on, the main thread may take the buffer.
	RaiseMax(&s_maxNodes, buf->nodeCount);
	RaiseMax(&s_maxBorders, buf->borderCount);
	InterlockedExchangeAdd(&s_arcsTrunc, buf->arcsTrunc);
	InterlockedExchangeAdd(&s_waterNodes, counts.waterNodes);
	CgLivePost(counts.slot, buf);
	InterlockedIncrement(&s_copies);
}

void CgLiveReport(double now)
{
#ifdef ZONEOPT_DEBUG
	const double interval = 30.0;
#else
	const double interval = 60.0;
#endif
	if (now - s_lastReport < interval)
		return;
	s_lastReport = now;
	LONG copies = s_copies;
	LONG skipped = s_overCap + s_noBuffer + s_noMesh + s_badSlot + s_noShift;
	if (copies + skipped == s_lastMoved)
		return;
	s_lastMoved = copies + skipped;
	CgStats st;
	CgStatsGet(&st);
	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "PlannerLive: copies=%ld skipped=%ld overCap=%ld noBuffer=%ld noMesh=%ld badSlot=%ld noShift=%ld"
	            " slotCap=%ld superseded=%ld stale=%ld promoted=%ld noSlot=%ld uidClash=%ld busy=%ld"
	            " bordersSkipped=%ld arcsTrunc=%ld maxNodes=%ld maxBorders=%ld waterNodes=%ld noFaceData=%ld",
	            (long)copies, (long)skipped, (long)s_overCap, (long)s_noBuffer, (long)s_noMesh, (long)s_badSlot,
	            (long)s_noShift, st.slotCap, st.superseded, st.stale, st.promoted, st.noSlot, st.uidClash, st.busy,
	            (long)s_bordersSkipped, (long)s_arcsTrunc, (long)s_maxNodes, (long)s_maxBorders, (long)s_waterNodes,
	            (long)s_noFaceData);
	LogMsg(line);
}

} // namespace planner
