// coarse_graph_live_site.cpp - The live overlay's game side: the graph-instance connect's call into
// the walk, the re-copy of the registered neighbours a later stitch changed, their counters, and the
// PlannerLive: line. The connect side runs on the path thread inside NavMesh::update's exclusive
// world lock; it takes no lock of its own, allocates nothing and logs nothing (interlocked counters
// only). A stitch on a generator thread may rewrite a mesh's streaming sets meanwhile, so every copy
// and the comparison run under a fault guard and a re-copy whose signature moved is discarded. The
// report runs on the main thread.
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
#include "base/clock.h"
#include <stdio.h>
#include <string.h>
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
static volatile LONG s_zeroStride     = 0;   // copies of a mesh whose face-data striding is 0
static volatile LONG s_liveFault      = 0;   // registering copies stopped by an access fault (in skipped)

// The re-copies: each stale neighbour a registration lists is posted, torn or left.
static volatile LONG s_staleSeen      = 0;   // registered neighbours the comparison listed
static volatile LONG s_stitchRefresh  = 0;   // re-copies posted
static volatile LONG s_refreshTorn    = 0;   // re-copies whose slot's signature moved during the copy
static volatile LONG s_staleLeft      = 0;   // listed neighbours not posted, and neighbours past the cap
static volatile LONG s_refreshFault   = 0;   // re-copies stopped by an access fault
static volatile LONG s_compareFault   = 0;   // comparisons stopped by an access fault
static volatile LONG s_refreshUs      = 0;   // microseconds in the comparison and the re-copies

// One record per collection slot: the uid, store generation and streaming-set signature of the
// section's last posted copy. Read and written only in CgLiveOnConnect's call tree, which the
// exclusive world lock serializes. A return before the copies leaves a record as it was, so a record
// may outlive its section when another takes the slot; the comparison treats a record naming
// another uid as absent, so the slot's occupant is copied again when a neighbour names it.
static CgLiveSig s_liveSig[CG_LIVE_RECORDS];

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

static void ClearRecord(int slot)
{
	if (slot >= 0 && slot < CG_LIVE_RECORDS)
		s_liveSig[slot].valid = 0;
}

static void SetRecord(int slot, int uid, unsigned gen, unsigned sig)
{
	if (slot < 0 || slot >= CG_LIVE_RECORDS)
		return;
	CgLiveSig& r = s_liveSig[slot];
	r.valid = 1;
	r.uid = uid;
	r.gen = gen;
	r.sig = sig;
}

// The copy, with the slot's signature read before and after it, under a fault guard. Standalone and
// POD-only: MSVC 2010 refuses a structured handler in a function holding an object that needs
// unwinding. counts->slot holds the slot from the first read on. False on an access fault.
static bool SafeLiveCopy(void* graphInst, void* coll, const float shift[3], CgBlock* buf, CgLiveCounts* counts,
                         CgLiveResult* r, unsigned* before, unsigned* after)
{
	bool ok = true;
	int uid = 0;
	GuardEnter();
	__try
	{
		int slot = graphInst ? *(const int*)((const unsigned char*)graphInst + OFF_GI_SECTION) : -1;
		counts->slot = slot;
		CgLiveSlotSignature(coll, slot, &uid, before);
		*r = CgLiveCopy(graphInst, coll, shift, buf, counts);
		CgLiveSlotSignature(coll, slot, &uid, after);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// The comparison from the registering slot's mesh under a fault guard, POD-only like SafeLiveCopy.
// On an access fault it lists nothing, with *checked and *overflow zero, and counts compareFault;
// the records are only read.
static int SafeStaleNeighbours(void* coll, int slot, unsigned gen, int* slots, unsigned* sigs, int* checked,
                               int* overflow)
{
	int n = 0;
	GuardEnter();
	__try
	{
		n = CgLiveStaleNeighbours(coll, CgCollectionMeshInstance(coll, slot), slot, s_liveSig, CG_LIVE_RECORDS,
		                          gen, slots, sigs, CG_LIVE_REFRESH_MAX, checked, overflow);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		n = 0;
		*checked = *overflow = 0;
		InterlockedIncrement(&s_compareFault);
	}
	GuardLeave();
	return n;
}

// The sums a posted buffer adds; read before the post, since from the exchange on the main thread
// may take the buffer.
static void AddPosted(const CgBlock* buf, const CgLiveCounts& counts)
{
	RaiseMax(&s_maxNodes, buf->nodeCount);
	RaiseMax(&s_maxBorders, buf->borderCount);
	InterlockedExchangeAdd(&s_arcsTrunc, buf->arcsTrunc);
	InterlockedExchangeAdd(&s_waterNodes, counts.waterNodes);
	InterlockedExchangeAdd(&s_zeroStride, counts.zeroStride);
}

// The registering section's copy: posted on CGL_OK, as before, even when its signature moved (the
// stitch in flight belongs to a job whose own registration reads this section again), with the
// slot's record set; a fault, a refusal or an empty pool clears the record (a fault or a refusal
// also releases the buffer). Returns the slot, -1 when it is not known.
static int CopyRegistering(void* graphInst, void* coll, const float shift[3])
{
	CgBlock* buf = CgLiveAcquire();
	if (!buf)
	{
		InterlockedIncrement(&s_noBuffer);
		int slot = graphInst ? *(const int*)((const unsigned char*)graphInst + OFF_GI_SECTION) : -1;
		ClearRecord(slot);
		return slot;
	}
	CgLiveCounts counts;
	memset(&counts, 0, sizeof(counts));
	counts.slot = -1;
	CgLiveResult r = CGL_NO_MESH;
	unsigned before = 0, after = 0;
	if (!SafeLiveCopy(graphInst, coll, shift, buf, &counts, &r, &before, &after))
	{
		InterlockedIncrement(&s_liveFault);
		CgLiveRelease(buf);
		ClearRecord(counts.slot);
		return counts.slot;
	}
	InterlockedExchangeAdd(&s_bordersSkipped, counts.bordersSkipped);
	InterlockedExchangeAdd(&s_noFaceData, counts.noFaceData);
	if (r != CGL_OK)
	{
		CountRefusal(r);
		CgLiveRelease(buf);
		ClearRecord(counts.slot);
		return counts.slot;
	}
	AddPosted(buf, counts);
	int uid = buf->uid;
	unsigned gen = buf->storeGen;
	CgLivePost(counts.slot, buf);
	InterlockedIncrement(&s_copies);
	SetRecord(counts.slot, uid, gen, before);
	return counts.slot;
}

// One listed neighbour copied again: posted only when the copy answered CGL_OK and the slot's
// signature held across it (stitchRefresh, the record set). A torn copy keeps the record, so the
// next registration naming the slot retries it; a fault, a refusal or an empty pool clears it.
// Every copy not posted counts in staleLeft.
static void RecopySlot(void* coll, int slot, const float shift[3])
{
	void* graphInst = (void*)CgCollectionGraphInstance(coll, slot);
	CgBlock* buf = graphInst ? CgLiveAcquire() : NULL;
	CgLiveCounts counts;
	memset(&counts, 0, sizeof(counts));
	counts.slot = -1;
	CgLiveResult r = CGL_NO_MESH;
	unsigned before = 0, after = 0;
	bool ok = buf && SafeLiveCopy(graphInst, coll, shift, buf, &counts, &r, &before, &after);
	if (ok && r == CGL_OK && before == after)
	{
		AddPosted(buf, counts);
		int uid = buf->uid;
		unsigned gen = buf->storeGen;
		CgLivePost(counts.slot, buf);
		InterlockedIncrement(&s_stitchRefresh);
		SetRecord(slot, uid, gen, before);
		return;
	}
	if (buf)
		CgLiveRelease(buf);
	InterlockedIncrement(&s_staleLeft);
	if (ok && r == CGL_OK)
	{
		InterlockedIncrement(&s_refreshTorn);
		return;
	}
	if (buf && !ok)
		InterlockedIncrement(&s_refreshFault);
	ClearRecord(slot);
}

// The comparison from the registering mesh and the re-copy of each neighbour it lists; the
// neighbours past the cap count in staleLeft.
static void RefreshNeighbours(void* coll, int slot, const float shift[3])
{
	LONGLONG t0 = QpcNow();
	int slots[CG_LIVE_REFRESH_MAX];
	unsigned sigs[CG_LIVE_REFRESH_MAX];
	int checked = 0, overflow = 0;
	int n = SafeStaleNeighbours(coll, slot, CgStoreGen(), slots, sigs, &checked, &overflow);
	InterlockedExchangeAdd(&s_staleSeen, n);
	InterlockedExchangeAdd(&s_staleLeft, overflow);
	for (int i = 0; i < n; ++i)
		RecopySlot(coll, slots[i], shift);
	InterlockedExchangeAdd(&s_refreshUs, QpcDeltaUs(t0, QpcNow()));
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
	int slot = CopyRegistering(graphInst, coll, shift);
	RefreshNeighbours(coll, slot, shift);
}

void CgLiveReport(double now)
{
#ifdef KEO_DEBUG
	const double interval = 30.0;
#else
	const double interval = 60.0;
#endif
	if (now - s_lastReport < interval)
		return;
	s_lastReport = now;
	LONG copies = s_copies;
	LONG skipped = s_overCap + s_noBuffer + s_noMesh + s_badSlot + s_noShift + s_liveFault;
	LONG moved = copies + skipped + s_stitchRefresh + s_staleSeen + s_staleLeft;
	if (moved == s_lastMoved)
		return;
	s_lastMoved = moved;
	CgStats st;
	CgStatsGet(&st);
	char line[1024];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "PlannerLive: copies=%ld skipped=%ld overCap=%ld noBuffer=%ld noMesh=%ld badSlot=%ld noShift=%ld"
	            " slotCap=%ld superseded=%ld stale=%ld promoted=%ld noSlot=%ld uidClash=%ld busy=%ld"
	            " bordersSkipped=%ld arcsTrunc=%ld maxNodes=%ld maxBorders=%ld waterNodes=%ld noFaceData=%ld"
	            " zeroStride=%ld staleSeen=%ld stitchRefresh=%ld refreshTorn=%ld staleLeft=%ld liveFault=%ld refreshUs=%ld"
	            " crossNoBlock=%ld crossDropped=%ld crossOneSided=%ld deferred=%ld",
	            (long)copies, (long)skipped, (long)s_overCap, (long)s_noBuffer, (long)s_noMesh, (long)s_badSlot,
	            (long)s_noShift, st.slotCap, st.superseded, st.stale, st.promoted, st.noSlot, st.uidClash, st.busy,
	            (long)s_bordersSkipped, (long)s_arcsTrunc, (long)s_maxNodes, (long)s_maxBorders, (long)s_waterNodes,
	            (long)s_noFaceData, (long)s_zeroStride, (long)s_staleSeen, (long)s_stitchRefresh, (long)s_refreshTorn,
	            (long)s_staleLeft, (long)(s_liveFault + s_refreshFault + s_compareFault), (long)s_refreshUs,
	            st.crossNoBlock, st.crossDropped, st.crossOneSided, st.deferred);
	LogMsg(line);
}

} // namespace planner
