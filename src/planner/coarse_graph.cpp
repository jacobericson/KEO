// coarse_graph.cpp - The coarse graph store: directory, epochs, retire stack, interior uid table,
// live pool and records, hand-off, block building and the read-time cross resolution.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/coarse_graph.h"

#include <float.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

namespace planner {

namespace coarse_graph_detail {

// One section's entry. epoch is even while no writer holds it; base and over change only
// between a writer's take and its release.
struct CgEntry
{
	volatile LONG     epoch;
	CgBlock* volatile base;
	CgBlock* volatile over;
};

// Everything CgStoreCreate allocates, behind one pointer. The three list heads need 16-byte
// alignment, which the allocation gives.
struct CgStoreState
{
	SLIST_HEADER      retire;      // replaced or refused heap blocks; freed by the main thread
	SLIST_HEADER      liveFree;    // free live buffers
	SLIST_HEADER      handOff;     // interior blocks from the builder
	CgEntry*          dir;         // CG_DIR_SLOTS
	int*              interiorUid; // CG_INTERIOR_SLOTS, 0 = empty (uid 0 is an exterior)
	CgBlock* volatile* records;    // CG_LIVE_RECORDS
	unsigned char*    pool;        // CG_LIVE_BUFFERS buffers of s_bufferStride bytes
};

// Byte offsets of a block's arrays in its allocation, header first.
struct CgLayout { size_t nodes, arcs, borders, nodeBorders, total; };

} // namespace coarse_graph_detail
using namespace coarse_graph_detail;

static CgStoreState*  s_store = NULL;
static volatile LONG  s_ready = 0;
static volatile LONG  s_gen = 1;
static int            s_promoteCursor = 0;
static volatile CgStats s_count;
static void         (*s_pauseAfterTake)(void* ctx) = NULL;
static void*          s_pauseAfterTakeCtx = NULL;

static size_t RoundUp(size_t v, size_t a)
{
	return (v + a - 1) / a * a;
}

static CgLayout LayoutFor(int nodes, int arcs, int borders)
{
	CgLayout l;
	l.nodes       = RoundUp(sizeof(CgBlock), 16);
	l.arcs        = RoundUp(l.nodes + (size_t)nodes * sizeof(CgNode), 16);
	l.borders     = RoundUp(l.arcs + (size_t)arcs * sizeof(CgArc), 16);
	l.nodeBorders = RoundUp(l.borders + (size_t)borders * sizeof(CgBorder), 16);
	l.total       = RoundUp(l.nodeBorders + (size_t)borders * sizeof(int), 64);
	return l;
}

// Points the block's arrays into its own allocation and clears its header.
static CgBlock* InitBlock(void* mem, const CgLayout& l)
{
	unsigned char* p = (unsigned char*)mem;
	CgBlock* b = (CgBlock*)p;
	memset(b, 0, sizeof(*b));
	b->liveBuffer  = -1;
	b->collSlot    = -1;
	b->nodes       = (CgNode*)(p + l.nodes);
	b->arcs        = (CgArc*)(p + l.arcs);
	b->borders     = (CgBorder*)(p + l.borders);
	b->nodeBorders = (int*)(p + l.nodeBorders);
	return b;
}

// A heap block with room for the given counts; NULL on an allocation failure.
static CgBlock* AllocBlock(int nodes, int arcs, int borders)
{
	CgLayout l = LayoutFor(nodes, arcs, borders);
	void* mem = _aligned_malloc(l.total, MEMORY_ALLOCATION_ALIGNMENT);
	if (!mem)
		return NULL;
	CgBlock* b = InitBlock(mem, l);
	b->nodeCount   = nodes;
	b->arcCount    = arcs;
	b->borderCount = borders;
	return b;
}

// Nodes without a finite centre and arcs with a negative or NaN cost: the coarse search skips both.
static void CountUnsearchable(const CgBlock* b)
{
	long n = 0;
	for (int i = 0; i < b->nodeCount; ++i)
		if (!_finite(b->nodes[i].centre[0]) || !_finite(b->nodes[i].centre[1]) || !_finite(b->nodes[i].centre[2]))
			++n;
	for (int i = 0; i < b->arcCount; ++i)
		if (!(b->arcs[i].cost >= 0.0f))
			++n;
	InterlockedExchangeAdd(&s_count.unsearchable, n);
}

// Borders by (oppUid, face), stable: an insertion sort over input that is normally sorted already.
static bool BorderBefore(const CgBorder& a, const CgBorder& b)
{
	if (a.oppUid != b.oppUid)
		return a.oppUid < b.oppUid;
	return a.face < b.face;
}

static void SortBorders(CgBorder* br, int n)
{
	for (int i = 1; i < n; ++i)
	{
		if (!BorderBefore(br[i], br[i - 1]))
			continue;
		CgBorder t = br[i];
		int j = i;
		while (j > 0 && BorderBefore(t, br[j - 1]))
		{
			br[j] = br[j - 1];
			--j;
		}
		br[j] = t;
	}
}

// Fills nodeBorders and each node's border range from the borders' from fields; counts arcsTrunc.
static void IndexNodeBorders(CgBlock* b)
{
	int k = 0;
	b->arcsTrunc = 0;
	for (int n = 0; n < b->nodeCount; ++n)
	{
		CgNode& node = b->nodes[n];
		node.firstBorder = k;
		for (int i = 0; i < b->borderCount; ++i)
			if (b->borders[i].from == n)
				b->nodeBorders[k++] = i;
		node.borderCount = k - node.firstBorder;
		if (node.arcCount + node.borderCount > CG_NODE_ARCS_MAX)
			++b->arcsTrunc;
	}
}

static void FreeBlock(CgBlock* b)
{
	if (b && b->liveBuffer < 0)
		_aligned_free(b);
}

static void Retire(CgBlock* b)
{
	if (b)
		InterlockedPushEntrySList(&s_store->retire, &b->link);
}

// ---- Lifecycle ---------------------------------------------------------------------------------

static void FreeState(CgStoreState* st)
{
	if (!st)
		return;
	_aligned_free(st->dir);
	_aligned_free(st->interiorUid);
	_aligned_free((void*)st->records);
	_aligned_free(st->pool);
	_aligned_free(st);
}

static void* AllocZeroed(size_t n)
{
	void* p = _aligned_malloc(n, MEMORY_ALLOCATION_ALIGNMENT);
	if (p)
		memset(p, 0, n);
	return p;
}

bool CgStoreCreate()
{
	if (s_ready)
		return true;
	CgStoreState* st = (CgStoreState*)AllocZeroed(sizeof(CgStoreState));
	if (!st)
		return false;
	CgLayout live = LayoutFor(CG_LIVE_MAX_NODES, CG_LIVE_MAX_ARCS, CG_LIVE_MAX_BORDERS);
	st->dir         = (CgEntry*)AllocZeroed(sizeof(CgEntry) * CG_DIR_SLOTS);
	st->interiorUid = (int*)AllocZeroed(sizeof(int) * CG_INTERIOR_SLOTS);
	st->records     = (CgBlock* volatile*)AllocZeroed(sizeof(CgBlock*) * CG_LIVE_RECORDS);
	st->pool        = (unsigned char*)AllocZeroed(live.total * CG_LIVE_BUFFERS);
	if (!st->dir || !st->interiorUid || !st->records || !st->pool)
	{
		FreeState(st);
		return false;
	}
	InitializeSListHead(&st->retire);
	InitializeSListHead(&st->liveFree);
	InitializeSListHead(&st->handOff);
	for (int i = 0; i < CG_LIVE_BUFFERS; ++i)
	{
		CgBlock* b = InitBlock(st->pool + (size_t)i * live.total, live);
		b->liveBuffer = i;
		InterlockedPushEntrySList(&st->liveFree, &b->link);
	}
	memset((void*)&s_count, 0, sizeof(CgStats));
	s_promoteCursor = 0;
	s_store = st;
	InterlockedExchange(&s_gen, 1);
	InterlockedExchange(&s_ready, 1);
	return true;
}

static void FreeList(SLIST_HEADER* h)
{
	PSLIST_ENTRY e = InterlockedFlushSList(h);
	while (e)
	{
		PSLIST_ENTRY next = e->Next;
		FreeBlock((CgBlock*)e);
		e = next;
	}
}

void CgStoreDestroy()
{
	CgStoreState* st = s_store;
	if (!st)
		return;
	InterlockedExchange(&s_ready, 0);
	for (int i = 0; i < CG_DIR_SLOTS; ++i)
	{
		FreeBlock(st->dir[i].base);
		FreeBlock(st->dir[i].over);
	}
	FreeList(&st->retire);
	FreeList(&st->handOff);
	s_store = NULL;
	FreeState(st);
}

bool CgStoreReady()
{
	return InterlockedCompareExchange(&s_ready, 0, 0) != 0;
}

unsigned CgStoreGen()
{
	return (unsigned)InterlockedCompareExchange(&s_gen, 0, 0);
}

void CgStoreNewWorld()
{
	InterlockedIncrement(&s_gen);
}

int CgExteriorIndex(int gx, int gy)
{
	if (gx < 0 || gx >= 64 || gy < 0 || gy >= 64)
		return -1;
	return gy * 64 + gx;
}

// An exterior uid: x | (y << 8), nothing above bit 15, both bytes below 64.
static bool IsExteriorUid(int uid)
{
	unsigned u = (unsigned)uid;
	return (u >> 16) == 0 && (u & 0xFF) < 64 && (u >> 8) < 64;
}

static unsigned InteriorHash(int uid)
{
	return ((unsigned)uid * 2654435761u) >> 20;   // 12 bits: CG_INTERIOR_SLOTS
}

// The uid's run in the interior table: its directory index, else -1 at the first empty slot (which
// insert takes instead) or at the end of the run.
static int ProbeInterior(int uid, bool insert)
{
	if (!s_store)
		return -1;
	unsigned h = InteriorHash(uid);
	for (int p = 0; p < CG_INTERIOR_PROBES; ++p)
	{
		unsigned slot = (h + (unsigned)p) & (CG_INTERIOR_SLOTS - 1);
		int have = s_store->interiorUid[slot];
		if (have == 0 && insert)
			s_store->interiorUid[slot] = have = uid;
		if (have == uid)
			return CG_EXTERIOR_SLOTS + (int)slot;
		if (have == 0)
			return -1;
	}
	return -1;
}

int CgIndexOfUid(int uid)
{
	if (IsExteriorUid(uid))
	{
		int x = uid & 0xFF;
		int y = (uid >> 8) & 0xFF;
		return y * 64 + x;
	}
	return ProbeInterior(uid, false);
}

int CgInsertInterior(int uid)
{
	if (IsExteriorUid(uid))
	{
		InterlockedIncrement(&s_count.uidClash);
		return -1;
	}
	return ProbeInterior(uid, true);
}

unsigned CgNodeKey(int dirIndex, int node)
{
	return ((unsigned)dirIndex << CG_NODE_BITS) | ((unsigned)node & ((1u << CG_NODE_BITS) - 1));
}

int CgNodeDir(unsigned key)
{
	return (int)(key >> CG_NODE_BITS);
}

int CgNodeIndex(unsigned key)
{
	return (int)(key & ((1u << CG_NODE_BITS) - 1));
}

// ---- Publication -------------------------------------------------------------------------------

void CgTestPauseAfterTake(void (*fn)(void* ctx), void* ctx)
{
	s_pauseAfterTakeCtx = ctx;
	s_pauseAfterTake = fn;
}

// The take: the entry's epoch from even e to e + 1 with one compare-exchange, retried while
// another writer holds it. Returns e.
static LONG TakeEntry(CgEntry* ent)
{
	for (;;)
	{
		LONG e = ent->epoch;
		if ((e & 1) == 0 && InterlockedCompareExchange(&ent->epoch, e + 1, e) == e)
			return e;
		InterlockedIncrement(&s_count.busy);
		SwitchToThread();
	}
}

static void ReleaseEntry(CgEntry* ent, LONG e)
{
	InterlockedExchange(&ent->epoch, e + 2);
}

static void PauseAfterTake()
{
	if (s_pauseAfterTake)
		s_pauseAfterTake(s_pauseAfterTakeCtx);
}

static bool PublishArgsOk(int dirIndex, CgBlock* b)
{
	if (!b)
		return false;
	if (!s_store || dirIndex < 0 || dirIndex >= CG_DIR_SLOTS)
	{
		InterlockedIncrement(&s_count.noSlot);
		if (s_store)
			Retire(b);
		else
			FreeBlock(b);
		return false;
	}
	return true;
}

CgPublishResult CgPublishBase(int dirIndex, CgBlock* b)
{
	if (!PublishArgsOk(dirIndex, b))
		return CGP_NO_SLOT;
	CgEntry* ent = &s_store->dir[dirIndex];
	LONG e = TakeEntry(ent);
	PauseAfterTake();
	CgBlock* old = ent->base;
	ent->base = b;
	ReleaseEntry(ent, e);
	Retire(old);
	InterlockedIncrement(&s_count.publishes);
	return CGP_OK;
}

// A current-generation over is replaced only by another current-generation over, and a
// current-generation live copy never by a save block.
static bool Outranks(const CgBlock* cur, const CgBlock* b, unsigned gen)
{
	if (!cur || cur->storeGen != gen)
		return false;
	if (b->storeGen != gen)
		return true;
	return cur->source == CG_LIVE && b->source == CG_SAVE;
}

CgPublishResult CgPublishOver(int dirIndex, CgBlock* b)
{
	if (!PublishArgsOk(dirIndex, b))
		return CGP_NO_SLOT;
	CgEntry* ent = &s_store->dir[dirIndex];
	LONG e = TakeEntry(ent);
	PauseAfterTake();
	CgBlock* cur = ent->over;
	if (Outranks(cur, b, CgStoreGen()))
	{
		ReleaseEntry(ent, e);
		Retire(b);
		InterlockedIncrement(&s_count.outranked);
		return CGP_OUTRANKED;
	}
	ent->over = b;
	ReleaseEntry(ent, e);
	Retire(cur);
	InterlockedIncrement(&s_count.publishes);
	return CGP_OK;
}

bool CgRead(int dirIndex, CgView* out)
{
	out->block = NULL;
	out->epoch = 0;
	if (!s_store || dirIndex < 0 || dirIndex >= CG_DIR_SLOTS)
		return false;
	CgEntry* ent = &s_store->dir[dirIndex];
	unsigned gen = CgStoreGen();
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG e1 = ent->epoch;
		if (e1 & 1)
			continue;
		MemoryBarrier();
		CgBlock* over = ent->over;
		CgBlock* base = ent->base;
		MemoryBarrier();
		if (ent->epoch != e1)
			continue;
		CgBlock* pick = (over && over->storeGen == gen) ? over : base;
		if (!pick)
			return false;
		out->block = pick;
		out->epoch = (unsigned)e1;
		return true;
	}
	InterlockedIncrement(&s_count.readBusy);
	return false;
}

// ---- Live pool and records ---------------------------------------------------------------------

CgBlock* CgLiveAcquire()
{
	if (!s_store)
		return NULL;
	PSLIST_ENTRY e = InterlockedPopEntrySList(&s_store->liveFree);
	if (!e)
	{
		InterlockedIncrement(&s_count.liveEmpty);
		return NULL;
	}
	CgBlock* b = (CgBlock*)e;
	b->uid = 0;
	b->source = CG_LIVE;
	b->storeGen = 0;
	b->collSlot = -1;
	b->arcsTrunc = 0;
	b->nodeCount = b->arcCount = b->borderCount = 0;
	InterlockedIncrement(&s_count.liveAcquired);
	return b;
}

void CgLiveRelease(CgBlock* b)
{
	if (b && b->liveBuffer >= 0 && s_store)
		InterlockedPushEntrySList(&s_store->liveFree, &b->link);
}

void CgLivePost(int slot, CgBlock* buf)
{
	if (!buf)
		return;
	if (!s_store || slot < 0 || slot >= CG_LIVE_RECORDS)
	{
		InterlockedIncrement(&s_count.slotCap);
		CgLiveRelease(buf);
		return;
	}
	buf->collSlot = slot;
	buf->storeGen = CgStoreGen();
	CgBlock* prev = (CgBlock*)InterlockedExchangePointer((PVOID volatile*)&s_store->records[slot], buf);
	InterlockedIncrement(&s_count.posted);
	if (prev)
	{
		InterlockedIncrement(&s_count.superseded);
		CgLiveRelease(prev);
	}
}

void CgHandOffInterior(CgBlock* heapBlock)
{
	if (!heapBlock || !s_store)
		return;
	InterlockedPushEntrySList(&s_store->handOff, &heapBlock->link);
	InterlockedIncrement(&s_count.handedOff);
}

void CgDrainRetired()
{
	if (!s_store)
		return;
	PSLIST_ENTRY e = InterlockedFlushSList(&s_store->retire);
	while (e)
	{
		PSLIST_ENTRY next = e->Next;
		CgBlock* b = (CgBlock*)e;
		if (b->liveBuffer >= 0)
			CgLiveRelease(b);
		else
		{
			_aligned_free(b);
			InterlockedIncrement(&s_count.retiredFreed);
		}
		e = next;
	}
}

int CgDrainHandOff()
{
	if (!s_store)
		return 0;
	PSLIST_ENTRY e = InterlockedFlushSList(&s_store->handOff);
	PSLIST_ENTRY fifo = NULL;
	while (e)                                   // the flush is newest first; publish oldest first
	{
		PSLIST_ENTRY next = e->Next;
		e->Next = fifo;
		fifo = e;
		e = next;
	}
	unsigned gen = CgStoreGen();
	int published = 0;
	while (fifo)
	{
		PSLIST_ENTRY next = fifo->Next;
		CgBlock* b = (CgBlock*)fifo;
		fifo = next;
		bool stale = b->source != CG_BASE && b->storeGen != gen;
		int dir = stale ? -1 : CgInsertInterior(b->uid);
		if (dir < 0)
		{
			InterlockedIncrement(stale ? &s_count.stale : &s_count.noSlot);
			FreeBlock(b);
			continue;
		}
		CgPublishResult r = b->source == CG_BASE ? CgPublishBase(dir, b) : CgPublishOver(dir, b);
		if (r == CGP_OK)
			++published;
	}
	return published;
}

static bool LiveCountsOk(const CgBlock* buf)
{
	return buf->nodeCount >= 0 && buf->nodeCount <= CG_LIVE_MAX_NODES
	    && buf->arcCount >= 0 && buf->arcCount <= CG_LIVE_MAX_ARCS
	    && buf->borderCount >= 0 && buf->borderCount <= CG_LIVE_MAX_BORDERS;
}

// The heap copy of a live buffer, its arrays re-pointed into its own allocation.
static CgBlock* CopyLive(const CgBlock* buf)
{
	CgBlock* h = AllocBlock(buf->nodeCount, buf->arcCount, buf->borderCount);
	if (!h)
		return NULL;
	h->uid       = buf->uid;
	h->source    = CG_LIVE;
	h->storeGen  = buf->storeGen;
	h->collSlot  = buf->collSlot;
	h->arcsTrunc = buf->arcsTrunc;
	memcpy(h->nodes, buf->nodes, sizeof(CgNode) * (size_t)buf->nodeCount);
	memcpy(h->arcs, buf->arcs, sizeof(CgArc) * (size_t)buf->arcCount);
	memcpy(h->borders, buf->borders, sizeof(CgBorder) * (size_t)buf->borderCount);
	memcpy(h->nodeBorders, buf->nodeBorders, sizeof(int) * (size_t)buf->borderCount);
	return h;
}

int CgPromoteLive(int max)
{
	if (!s_store)
		return 0;
	unsigned gen = CgStoreGen();
	int taken = 0, published = 0;
	for (int scanned = 0; scanned < CG_LIVE_RECORDS && taken < max; ++scanned)
	{
		int slot = s_promoteCursor;
		s_promoteCursor = (s_promoteCursor + 1) % CG_LIVE_RECORDS;
		CgBlock* buf = s_store->records[slot]
		             ? (CgBlock*)InterlockedExchangePointer((PVOID volatile*)&s_store->records[slot], NULL) : NULL;
		if (!buf)
			continue;
		++taken;
		bool stale = buf->storeGen != gen;
		int dir = stale ? -1 : CgIndexOfUid(buf->uid);
		if (!stale && dir < 0)
			dir = CgInsertInterior(buf->uid);
		// Stale; or no directory index, counts past the buffer's capacity, or no memory for the copy.
		CgBlock* h = (dir >= 0 && LiveCountsOk(buf)) ? CopyLive(buf) : NULL;
		CgLiveRelease(buf);
		if (!h)
		{
			InterlockedIncrement(stale ? &s_count.stale : &s_count.noSlot);
			continue;
		}
		CountUnsearchable(h);
		if (CgPublishOver(dir, h) == CGP_OK)
		{
			InterlockedIncrement(&s_count.promoted);
			++published;
		}
	}
	return published;
}

static void Midpoint(const float* a, const float* b, float* out)
{
	for (int k = 0; k < 3; ++k)
		out[k] = (a[k] + b[k]) * 0.5f;
}

// An arc the block keeps: inside the tile's arcs, its target one of the section's own nodes.
static bool ArcKept(const TileGraph& g, const TgSection& sec, int ai)
{
	return ai >= 0 && ai < (int)g.arcs.size() && g.arcs[(size_t)ai].to >= 0 && g.arcs[(size_t)ai].to < sec.nodeCount;
}

static int SectionArcCount(const TileGraph& g, const TgSection& sec)
{
	int n = 0;
	for (int i = 0; i < sec.nodeCount; ++i)
	{
		const TgNode& tn = g.nodes[(size_t)(sec.firstNode + i)];
		for (int k = 0; k < tn.arcCount; ++k)
			n += ArcKept(g, sec, tn.firstArc + k) ? 1 : 0;
	}
	return n;
}

CgBlock* CgBlockFromTile(const TileGraph& g, int section, int source, unsigned storeGen)
{
	if (section < 0 || section >= (int)g.sections.size())
		return NULL;
	const TgSection& sec = g.sections[(size_t)section];
	if (sec.nodeCount < 0 || sec.firstNode < 0 || sec.firstNode + sec.nodeCount > (int)g.nodes.size()
	    || sec.borderCount < 0 || sec.firstBorder < 0 || sec.firstBorder + sec.borderCount > (int)g.borders.size())
		return NULL;
	CgBlock* b = AllocBlock(sec.nodeCount, SectionArcCount(g, sec), sec.borderCount);
	if (!b)
		return NULL;
	b->uid      = sec.uid;
	b->source   = source;
	b->storeGen = storeGen;
	int arc = 0;
	for (int i = 0; i < sec.nodeCount; ++i)
	{
		const TgNode& tn = g.nodes[(size_t)(sec.firstNode + i)];
		CgNode& n = b->nodes[i];
		memcpy(n.centre, tn.centre, sizeof(n.centre));
		memcpy(n.boxMin, tn.boxMin, sizeof(n.boxMin));
		memcpy(n.boxMax, tn.boxMax, sizeof(n.boxMax));
		n.faces = tn.faces;
		n.water = tn.water;
		n.firstArc = arc;
		for (int k = 0; k < tn.arcCount; ++k)
		{
			int ai = tn.firstArc + k;
			if (!ArcKept(g, sec, ai))
				continue;
			b->arcs[arc].to = g.arcs[(size_t)ai].to;
			b->arcs[arc].cost = g.arcs[(size_t)ai].cost;
			++arc;
		}
		n.arcCount = arc - n.firstArc;
	}
	for (int i = 0; i < sec.borderCount; ++i)
	{
		const TgBorder& tb = g.borders[(size_t)(sec.firstBorder + i)];
		CgBorder& br = b->borders[i];
		br.oppUid  = tb.oppUid;
		br.face    = tb.face;
		br.oppFace = tb.oppFace;
		br.from    = (tb.from >= 0 && tb.from < sec.nodeCount) ? tb.from : -1;
		memcpy(br.a, tb.a, sizeof(br.a));
		memcpy(br.b, tb.b, sizeof(br.b));
		Midpoint(br.a, br.b, br.portal);
	}
	SortBorders(b->borders, b->borderCount);
	IndexNodeBorders(b);
	CountUnsearchable(b);
	return b;
}

// ---- Cross resolution --------------------------------------------------------------------------

// The first border of nb with (oppUid, face) == (uid, face), or -1.
static int FindBorder(const CgBlock* nb, int uid, int face)
{
	int lo = 0, hi = nb->borderCount;
	while (lo < hi)
	{
		int mid = lo + (hi - lo) / 2;
		const CgBorder& m = nb->borders[mid];
		if (m.oppUid < uid || (m.oppUid == uid && m.face < face))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < nb->borderCount && nb->borders[lo].oppUid == uid && nb->borders[lo].face == face)
		return lo;
	return -1;
}

static float Dist2(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

int CgCrossArcs(const CgBlock* a, int node, CgNeighbourFn neighbourOf, void* ctx, CgResolved* out, int max)
{
	if (!a || !neighbourOf || node < 0 || node >= a->nodeCount || max <= 0)
		return 0;
	const CgNode& from = a->nodes[node];
	float best[CG_NODE_ARCS_MAX];               // each written arc's longest edge, squared
	int written = 0;
	for (int k = 0; k < from.borderCount; ++k)
	{
		int bi = a->nodeBorders[from.firstBorder + k];
		if (bi < 0 || bi >= a->borderCount)
			continue;
		const CgBorder& br = a->borders[bi];
		int dir = -1;
		const CgBlock* nb = neighbourOf(ctx, br.oppUid, &dir);
		if (!nb)
			continue;
		int m = FindBorder(nb, a->uid, br.oppFace);
		if (m < 0)
			continue;
		int target = nb->borders[m].from;
		if (target < 0 || target >= nb->nodeCount)
			continue;
		float len = Dist2(br.a, br.b);
		int j = 0;
		while (j < written && !(out[j].dirIndex == dir && out[j].node == target))
			++j;
		if (j == written)
		{
			if (written >= max || written >= CG_NODE_ARCS_MAX)
				continue;
			out[j].dirIndex = dir;
			out[j].node = target;
			out[j].cost = sqrtf(Dist2(from.centre, nb->nodes[target].centre));
			out[j].water = nb->nodes[target].water;
			best[j] = -1.0f;
			++written;
		}
		if (len > best[j])
		{
			best[j] = len;
			Midpoint(br.a, br.b, out[j].portal);
			memcpy(out[j].edgeA, br.a, sizeof(out[j].edgeA));
			memcpy(out[j].edgeB, br.b, sizeof(out[j].edgeB));
		}
	}
	return written;
}

void CgStatsGet(CgStats* out)
{
	*out = const_cast<const CgStats&>(s_count);   // each counter read whole; the set is not a snapshot
}

} // namespace planner
