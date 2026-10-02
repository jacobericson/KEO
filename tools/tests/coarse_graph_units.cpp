// The coarse graph store, single-threaded: publication and its epoch take (a test hook runs a read
// inside a writer's take), the over ranks and generations, the uid decode and the interior table,
// the retire drain, the live records, the builder's hand-off, block building from a hand-made tile,
// the read-time cross resolution, the cache records and the read-whole loader. Every block is built
// from a TileGraph written here; no fixture is read. The concurrent run is coarse_graph_injection.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "planner/coarse_graph.h"
#include "planner/coarse_graph_cache.h"

#include "check.h"

using namespace planner;

namespace coarse_graph_units_detail {

struct PauseRead { int dir; int calls; bool answered; };

// A promotion run inside a builder publish's take: its calls, what it published and what it deferred.
struct PauseDefer { int calls; int published; long deferred; };

struct Neighbours
{
	const CgBlock* block[4];
	int            uid[4];
	int            dir[4];
	int            count;
};

// A far border of a rule fixture: its faces, its node and its portal's z.
struct FarBorder { int face; int oppFace; int from; float z; };

// One shape of the cross rule: the near border's faces and portal z, the far block's source and
// borders, and the class and target node the rule must give.
struct Shape
{
	int       face, oppFace;
	float     nearZ;
	int       source;
	int       farCount;
	FarBorder farBorders[1];
	int       cls;
	int       target;
};

struct FakeFile
{
	std::string log;
	std::vector<unsigned char> bytes;
	bool failRead;
};

} // namespace coarse_graph_units_detail
using namespace coarse_graph_units_detail;

static void Fresh()
{
	CgTestPauseAfterTake(NULL, NULL);
	CgStoreDestroy();
	CgStoreCreate();
}

static CgStats Stats()
{
	CgStats s;
	CgStatsGet(&s);
	return s;
}

static void SetV(float* v, float x, float y, float z)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

// Appends one section of `nodes` nodes whose node i sits at (x0 + 100 i, 0, 0), each node linked
// to the next by one arc.
static void AddSection(TileGraph* g, int uid, int kind, int nodes, float x0)
{
	TgSection s;
	memset(&s, 0, sizeof(s));
	s.uid = uid;
	s.kind = kind;
	s.firstNode = (int)g->nodes.size();
	s.nodeCount = nodes;
	s.firstBorder = (int)g->borders.size();
	s.borderCount = 0;
	for (int i = 0; i < nodes; ++i)
	{
		TgNode n;
		memset(&n, 0, sizeof(n));
		SetV(n.centre, x0 + 100.0f * (float)i, 0.0f, 0.0f);
		SetV(n.boxMin, n.centre[0] - 10.0f, -1.0f, -10.0f);
		SetV(n.boxMax, n.centre[0] + 10.0f, 1.0f, 10.0f);
		n.faces = 3 + i;
		n.firstArc = (int)g->arcs.size();
		n.arcCount = (i + 1 < nodes) ? 1 : 0;
		if (n.arcCount)
		{
			TgArc a = { i + 1, 100.0f };
			g->arcs.push_back(a);
		}
		g->nodes.push_back(n);
	}
	g->sections.push_back(s);
}

// Appends a border to the last section.
static void AddBorder(TileGraph* g, int oppUid, int face, int oppFace, int from, const float* a, const float* b)
{
	TgBorder br;
	memset(&br, 0, sizeof(br));
	br.oppUid = oppUid;
	br.face = face;
	br.edge = face * 3;
	br.oppFace = oppFace;
	br.oppEdge = oppFace * 3;
	br.from = from;
	memcpy(br.a, a, sizeof(br.a));
	memcpy(br.b, b, sizeof(br.b));
	g->borders.push_back(br);
	g->sections.back().borderCount++;
}

static CgBlock* OneSectionBlock(int uid, int source, unsigned gen, int nodes)
{
	TileGraph g;
	g.interiorsDropped = 0;
	g.bordersSkipped = 0;
	AddSection(&g, uid, uid > 0xFFFF ? TGS_INTERIOR : TGS_EXTERIOR, nodes, 0.0f);
	return CgBlockFromTile(g, 0, source, gen);
}

// A live buffer holding one node, as the path thread fills one.
static CgBlock* FilledLive(int uid)
{
	CgBlock* b = CgLiveAcquire();
	if (!b)
		return NULL;
	b->uid = uid;
	b->nodeCount = 1;
	b->arcCount = 0;
	b->borderCount = 0;
	memset(&b->nodes[0], 0, sizeof(CgNode));
	SetV(b->nodes[0].centre, 5.0f, 6.0f, 7.0f);
	return b;
}

// Free buffers in the pool, counted by taking them all and giving them back.
static int PoolFree()
{
	std::vector<CgBlock*> taken;
	for (CgBlock* b = CgLiveAcquire(); b; b = CgLiveAcquire())
		taken.push_back(b);
	for (size_t i = 0; i < taken.size(); ++i)
		CgLiveRelease(taken[i]);
	return (int)taken.size();
}

static int ReadSource(int dir)
{
	CgView v;
	return CgRead(dir, &v) ? v.block->source : -1;
}

// ---- Publication ---------------------------------------------------------------------------------

static void ReadInsideTake(void* ctx)
{
	PauseRead* p = (PauseRead*)ctx;
	CgView v;
	p->calls++;
	p->answered = CgRead(p->dir, &v);
}

static void CheckTakeRefusesReaders()
{
	// CgPublishOver: the reader runs inside its take and must refuse the entry.
	Fresh();
	CgPublishBase(7, OneSectionBlock(7, CG_BASE, 0, 1));
	PauseRead p = { 7, 0, true };
	CgTestPauseAfterTake(ReadInsideTake, &p);
	CgPublishResult r = CgPublishOver(7, OneSectionBlock(7, CG_SAVE, CgStoreGen(), 2));
	CgTestPauseAfterTake(NULL, NULL);
	CgView v;
	bool after = CgRead(7, &v) && v.block->source == CG_SAVE;
	Check(r == CGP_OK && p.calls == 1 && !p.answered && after, "store: a reader refuses an entry CgPublishOver holds");

	// CgPublishBase: the same.
	PauseRead q = { 7, 0, true };
	CgTestPauseAfterTake(ReadInsideTake, &q);
	CgPublishBase(7, OneSectionBlock(7, CG_BASE, 0, 3));
	CgTestPauseAfterTake(NULL, NULL);
	CgStats s = Stats();
	Check(q.calls == 1 && !q.answered && s.readBusy >= 2 && CgRead(7, &v),
	      "store: a reader refuses an entry CgPublishBase holds");
}

static void CheckRanksAndGenerations()
{
	Fresh();
	CgPublishBase(5, OneSectionBlock(5, CG_BASE, 0, 1));
	CgLivePost(3, FilledLive(5));
	int promoted = CgPromoteLive(8);
	CgPublishResult r = CgPublishOver(5, OneSectionBlock(5, CG_SAVE, CgStoreGen(), 2));
	CgStats s = Stats();
	Check(promoted == 1 && r == CGP_OUTRANKED && ReadSource(5) == CG_LIVE && s.outranked == 1,
	      "store: a save over never replaces a current-generation live over");

	CgStoreNewWorld();
	Check(ReadSource(5) == CG_BASE, "store: a new world makes every over stale");

	r = CgPublishOver(5, OneSectionBlock(5, CG_SAVE, CgStoreGen(), 2));
	Check(r == CGP_OK && ReadSource(5) == CG_SAVE, "store: a stale over is replaced by a save over");
}

static void CheckRetireDrain()
{
	Fresh();
	CgPublishBase(9, OneSectionBlock(9, CG_BASE, 0, 1));
	CgDrainRetired();
	long before = Stats().retiredFreed;
	CgPublishBase(9, OneSectionBlock(9, CG_BASE, 0, 2));
	CgDrainRetired();
	CgView v;
	bool current = CgRead(9, &v) && v.block->nodeCount == 2;
	Check(before == 0 && Stats().retiredFreed == 1 && current, "store: a retired heap block is freed by the drain");
}

// Two live copies of one section: the newer, acquired second, is posted at the lower slot, so the
// promotion's slot order publishes it first and meets the older one after it.
static void CheckLiveOrder()
{
	Fresh();
	CgBlock* older = FilledLive(5);
	CgBlock* newer = FilledLive(5);
	if (newer)
		newer->nodes[0].centre[0] = 99.0f;
	CgLivePost(3, newer);
	CgLivePost(5, older);
	long outranked = Stats().outranked;
	int promoted = CgPromoteLive(8);
	CgView v;
	Check(promoted == 1 && Stats().outranked == outranked + 1 && CgRead(5, &v) && v.block->nodes[0].centre[0] == 99.0f,
	      "store: an older live copy never replaces a newer one");

	Fresh();
	CgBlock* early = FilledLive(6);
	CgStoreNewWorld();
	CgLivePost(4, early);
	long stale = Stats().stale;
	promoted = CgPromoteLive(8);
	Check(promoted == 0 && Stats().stale == stale + 1 && ReadSource(6) == -1,
	      "store: a copy acquired before a new world is stale at promotion");
}

static void PromoteInsideTake(void* ctx)
{
	PauseDefer* p = (PauseDefer*)ctx;
	CgTestPauseAfterTake(NULL, NULL);
	p->calls++;
	long before = Stats().deferred;
	p->published = CgPromoteLive(8);
	p->deferred = Stats().deferred - before;
}

// Entry 8 holds a base; a live copy of uid 8 is posted; the promotion runs inside a builder base
// publish's take of entry 8, so its own publish finds the entry held.
static PauseDefer DeferOnePromotion()
{
	Fresh();
	CgPublishBase(8, OneSectionBlock(8, CG_BASE, 0, 1));
	CgLivePost(10, FilledLive(8));
	PauseDefer p = { 0, -1, 0 };
	CgTestPauseAfterTake(PromoteInsideTake, &p);
	CgPublishBase(8, OneSectionBlock(8, CG_BASE, 0, 2));
	CgTestPauseAfterTake(NULL, NULL);
	return p;
}

static void CheckDeferredPublish()
{
	PauseDefer p = DeferOnePromotion();
	Check(p.calls == 1 && p.published == 0 && p.deferred == 1 && ReadSource(8) == CG_BASE,
	      "store: a main-thread publish that finds its entry held defers it");
	int landed = CgPromoteLive(8);
	Check(landed == 1 && ReadSource(8) == CG_LIVE, "store: a deferred publish lands on the next promotion");

	p = DeferOnePromotion();
	CgStoreNewWorld();
	long stale = Stats().stale;
	landed = CgPromoteLive(8);
	Check(p.deferred == 1 && landed == 0 && Stats().stale == stale + 1 && ReadSource(8) == CG_BASE,
	      "store: a deferred block of an older generation is retired, not published");
}

// ---- Uids ----------------------------------------------------------------------------------------

static void CheckUids()
{
	Fresh();
	Check(CgIndexOfUid(0x1C15) == 28 * 64 + 21 && CgIndexOfUid(0x3F3F) == 4095 && CgIndexOfUid(0) == 0,
	      "uid: an exterior uid maps to gy*64+gx");

	int i4000 = CgInsertInterior(0x4000);
	Check(CgIndexOfUid(0x0040) == -1 && i4000 >= CG_EXTERIOR_SLOTS && CgIndexOfUid(0x4000) == i4000,
	      "uid: a uid with a byte of 64 or more is not an exterior");

	int a = CgInsertInterior(0x1d04dd);
	int b = CgInsertInterior(0x1d04dd);
	Check(a >= CG_EXTERIOR_SLOTS && a < CG_DIR_SLOTS && b == a && CgIndexOfUid(0x1d04dd) == a,
	      "uid: an interior uid inserts once and is found again");

	long clash = Stats().uidClash;
	Check(CgInsertInterior(0x0105) == -1 && Stats().uidClash == clash + 1,
	      "uid: an interior uid that decodes as an exterior is refused and counted");

	Fresh();
	std::vector<int> seen(CG_DIR_SLOTS, 0);
	int failed = 0, lost = 0, dup = 0;
	for (int k = 0; k < CG_INTERIOR_SLOTS + 256; ++k)
	{
		int uid = 0x100000 + k * 7919;
		int idx = CgInsertInterior(uid);
		if (idx < 0)
		{
			++failed;
			if (CgIndexOfUid(uid) != -1) ++lost;
			continue;
		}
		if (seen[(size_t)idx]++) ++dup;
		if (CgIndexOfUid(uid) != idx) ++lost;
	}
	Check(failed >= 256 && lost == 0 && dup == 0, "uid: the interior table's full probe run answers no slot");
}

// ---- Live records and the hand-off -------------------------------------------------------------

static void CheckLive()
{
	Fresh();
	CgBlock* buf = FilledLive(12);
	CgLivePost(40, buf);
	int n = CgPromoteLive(8);
	CgView v;
	bool read = CgRead(12, &v) && v.block->source == CG_LIVE && v.block->liveBuffer < 0 && v.block != buf
	         && v.block->collSlot == 40 && v.block->nodeCount == 1 && v.block->nodes[0].centre[2] == 7.0f;
	CgStats s = Stats();
	Check(n == 1 && read && s.promoted == 1 && PoolFree() == CG_LIVE_BUFFERS,
	      "live: a posted buffer is promoted, published and returned to the pool");

	Fresh();
	CgLivePost(41, FilledLive(13));
	CgLivePost(41, FilledLive(13));
	s = Stats();
	int freeBefore = PoolFree();
	n = CgPromoteLive(8);
	Check(s.superseded == 1 && freeBefore == CG_LIVE_BUFFERS - 1 && n == 1 && PoolFree() == CG_LIVE_BUFFERS,
	      "live: a second post before the promotion returns the first buffer, counted superseded");

	Fresh();
	CgLivePost(42, FilledLive(14));
	CgStoreNewWorld();
	n = CgPromoteLive(8);
	s = Stats();
	Check(n == 0 && s.stale == 1 && ReadSource(14) == -1 && PoolFree() == CG_LIVE_BUFFERS,
	      "live: a post of an older generation is released, counted stale");

	Fresh();
	CgLivePost(CG_LIVE_RECORDS, FilledLive(15));
	s = Stats();
	Check(s.slotCap == 1 && s.posted == 0 && PoolFree() == CG_LIVE_BUFFERS,
	      "live: a slot at CG_LIVE_RECORDS is refused and the buffer released");

	Fresh();
	CgLivePost(43, FilledLive(0x1d04dd));
	bool unknown = CgIndexOfUid(0x1d04dd) == -1;
	n = CgPromoteLive(8);
	int dir = CgIndexOfUid(0x1d04dd);
	Check(unknown && n == 1 && dir >= CG_EXTERIOR_SLOTS && CgRead(dir, &v) && v.block->uid == 0x1d04dd,
	      "live: an interior live copy's uid is inserted by the promotion");

	Fresh();
	CgBlock* save = OneSectionBlock(0x2a0b1c, CG_SAVE, CgStoreGen(), 2);
	CgHandOffInterior(save);
	unknown = CgIndexOfUid(0x2a0b1c) == -1;
	n = CgDrainHandOff();
	dir = CgIndexOfUid(0x2a0b1c);
	s = Stats();
	Check(unknown && n == 1 && s.handedOff == 1 && dir >= CG_EXTERIOR_SLOTS && CgRead(dir, &v) && v.block == save,
	      "hand-off: an interior save block is published by the drain, its uid inserted");

	Fresh();
	int got = 0;
	std::vector<CgBlock*> all;
	for (int i = 0; i < CG_LIVE_BUFFERS; ++i)
	{
		CgBlock* b = CgLiveAcquire();
		if (b) { ++got; all.push_back(b); }
	}
	CgBlock* none = CgLiveAcquire();
	s = Stats();
	Check(got == CG_LIVE_BUFFERS && none == NULL && s.liveEmpty == 1, "store: an empty live pool answers NULL and counts");
	for (size_t i = 0; i < all.size(); ++i)
		CgLiveRelease(all[i]);
}

// ---- Block building ------------------------------------------------------------------------------

static void CheckBlocks()
{
	Fresh();
	TileGraph g;
	g.interiorsDropped = 0;
	g.bordersSkipped = 0;
	AddSection(&g, 0x0102, TGS_EXTERIOR, 2, 0.0f);
	AddSection(&g, 0x1d04dd, TGS_INTERIOR, 3, 1000.0f);
	float a[3], b[3];
	SetV(a, 0.0f, 0.0f, 0.0f);
	SetV(b, 10.0f, 0.0f, 0.0f);
	AddBorder(&g, 0x0102, 8, 4, 2, a, b);
	AddBorder(&g, 0x0101, 5, 1, 0, a, b);
	CgBlock* blk = CgBlockFromTile(g, 1, CG_BASE, 0);
	bool ok = blk && blk->uid == 0x1d04dd && blk->source == CG_BASE && blk->nodeCount == 3 && blk->arcCount == 2
	       && blk->borderCount == 2 && blk->liveBuffer == -1 && blk->collSlot == -1
	       && blk->nodes[2].centre[0] == 1200.0f && blk->nodes[1].faces == 4
	       && blk->nodes[0].firstArc == 0 && blk->nodes[0].arcCount == 1 && blk->arcs[0].to == 1 && blk->arcs[1].to == 2
	       && blk->arcs[0].cost == 100.0f
	       && blk->borders[0].oppUid == 0x0101 && blk->borders[1].oppUid == 0x0102
	       && blk->borders[0].from == 0 && blk->borders[1].face == 8 && blk->borders[1].oppFace == 4
	       && blk->borders[0].portal[0] == 5.0f
	       && blk->nodes[0].borderCount == 1 && blk->nodeBorders[blk->nodes[0].firstBorder] == 0
	       && blk->nodes[2].borderCount == 1 && blk->nodeBorders[blk->nodes[2].firstBorder] == 1
	       && blk->nodes[1].borderCount == 0 && blk->arcsTrunc == 0;
	Check(ok, "store: a hand-made tile's block carries its nodes, arcs and borders");
	if (blk) CgPublishBase(CgInsertInterior(blk->uid), blk);

	TileGraph w;
	w.interiorsDropped = 0;
	w.bordersSkipped = 0;
	AddSection(&w, 0x0406, TGS_EXTERIOR, 3, 0.0f);
	w.nodes[0].water = 0;
	w.nodes[1].water = 128;
	w.nodes[2].water = 255;
	CgBlock* wet = CgBlockFromTile(w, 0, CG_BASE, 0);
	Check(wet && wet->nodes[0].water == 0 && wet->nodes[1].water == 128 && wet->nodes[2].water == 255,
	      "store: a block keeps its nodes' water");
	if (wet) CgPublishBase(CgIndexOfUid(wet->uid), wet);

	TileGraph t;
	t.interiorsDropped = 0;
	t.bordersSkipped = 0;
	AddSection(&t, 0x0203, TGS_EXTERIOR, 2, 0.0f);
	t.nodes[0].arcCount = 0;
	t.arcs.clear();
	t.nodes[0].firstArc = 0;
	for (int k = 0; k < 40; ++k)
	{
		TgArc arc = { 1, 50.0f };
		t.arcs.push_back(arc);
	}
	t.nodes[0].arcCount = 40;
	t.nodes[1].firstArc = 40;
	t.nodes[1].arcCount = 0;
	for (int k = 0; k < 30; ++k)
		AddBorder(&t, 0x0204, k, k, 0, a, b);
	CgBlock* trunc = CgBlockFromTile(t, 0, CG_BASE, 0);
	Check(trunc && trunc->arcsTrunc == 1 && trunc->nodes[0].arcCount + trunc->nodes[0].borderCount == 70,
	      "store: a node over CG_NODE_ARCS_MAX arcs and borders is counted arcsTrunc");
	if (trunc) CgPublishBase(CgIndexOfUid(trunc->uid), trunc);

	TileGraph u;
	u.interiorsDropped = 0;
	u.bordersSkipped = 0;
	AddSection(&u, 0x0305, TGS_EXTERIOR, 3, 0.0f);
	u.arcs[0].cost = -1.0f;
	u.arcs[1].cost = std::numeric_limits<float>::quiet_NaN();
	u.nodes[2].centre[1] = std::numeric_limits<float>::infinity();
	long before = Stats().unsearchable;
	CgBlock* bad = CgBlockFromTile(u, 0, CG_BASE, 0);
	Check(bad && Stats().unsearchable == before + 3,
	      "store: a negative or NaN arc cost and a non-finite centre are counted unsearchable");
	if (bad) CgPublishBase(CgIndexOfUid(bad->uid), bad);
}

// ---- Cross resolution ----------------------------------------------------------------------------

static const CgBlock* FindNeighbour(void* ctx, int uid, int* dirOut)
{
	Neighbours* n = (Neighbours*)ctx;
	for (int i = 0; i < n->count; ++i)
	{
		if (n->uid[i] == uid)
		{
			*dirOut = n->dir[i];
			return n->block[i];
		}
	}
	return NULL;
}

static bool Near(const float* v, float x, float y, float z)
{
	return std::fabs(v[0] - x) < 1e-3f && std::fabs(v[1] - y) < 1e-3f && std::fabs(v[2] - z) < 1e-3f;
}

static void CheckCross()
{
	Fresh();
	// Tile 0.0 (uid 0x0000) and tile 1.0 (uid 0x0001). A's node 0 has three border faces: 10 and 11
	// face B's node 1 (edges 20 and 40 long), 12 faces B's node 0.
	TileGraph ga, gb;
	ga.interiorsDropped = ga.bordersSkipped = 0;
	gb.interiorsDropped = gb.bordersSkipped = 0;
	AddSection(&ga, 0x0000, TGS_EXTERIOR, 1, 4000.0f);
	float a0[3], a1[3], b0[3], b1[3], c0[3], c1[3];
	SetV(a0, 4608.0f, 0.0f, 0.0f);   SetV(a1, 4608.0f, 0.0f, 20.0f);
	SetV(b0, 4608.0f, 2.0f, 100.0f); SetV(b1, 4608.0f, 2.0f, 140.0f);
	SetV(c0, 4608.0f, 0.0f, 300.0f); SetV(c1, 4608.0f, 0.0f, 310.0f);
	AddBorder(&ga, 0x0001, 10, 20, 0, a0, a1);
	AddBorder(&ga, 0x0001, 11, 21, 0, b0, b1);
	AddBorder(&ga, 0x0001, 12, 22, 0, c0, c1);
	AddSection(&gb, 0x0001, TGS_EXTERIOR, 2, 5000.0f);
	gb.nodes[1].centre[2] = 300.0f;
	ga.nodes[0].water = 50;
	gb.nodes[0].water = 7;
	gb.nodes[1].water = 200;
	AddBorder(&gb, 0x0000, 20, 10, 1, a0, a1);
	AddBorder(&gb, 0x0000, 21, 11, 1, b0, b1);
	AddBorder(&gb, 0x0000, 22, 12, 0, c0, c1);
	CgBlock* A = CgBlockFromTile(ga, 0, CG_BASE, 0);
	CgBlock* B = CgBlockFromTile(gb, 0, CG_BASE, 0);
	Neighbours nb;
	memset(&nb, 0, sizeof(nb));
	nb.block[0] = A; nb.uid[0] = 0x0000; nb.dir[0] = 0;
	nb.block[1] = B; nb.uid[1] = 0x0001; nb.dir[1] = 1;
	nb.count = 2;
	CgResolved out[8];
	CgStats s0 = Stats();
	int n = CgCrossArcs(A, 0, FindNeighbour, &nb, out, 8);
	CgStats s1 = Stats();
	int toB1 = -1, toB0 = -1;
	for (int i = 0; i < n; ++i)
	{
		if (out[i].dirIndex == 1 && out[i].node == 1) toB1 = i;
		if (out[i].dirIndex == 1 && out[i].node == 0) toB0 = i;
	}
	Check(n == 2 && toB1 >= 0 && toB0 >= 0, "cross: the neighbour's border names the target node");
	Check(n == 2 && toB1 >= 0 && toB0 >= 0 && s1.crossNoBlock == s0.crossNoBlock && s1.crossDropped == s0.crossDropped
	      && s1.crossOneSided == s0.crossOneSided,
	      "cross: a mirrored pair resolves as before and counts nothing");
	Check(n == 2 && toB1 >= 0 && toB1 != toB0, "cross: borders to one node collapse into one arc");
	Check(toB1 >= 0 && Near(out[toB1].portal, 4608.0f, 2.0f, 120.0f) && Near(out[toB1].edgeA, 4608.0f, 2.0f, 100.0f)
	      && Near(out[toB1].edgeB, 4608.0f, 2.0f, 140.0f) && toB0 >= 0 && Near(out[toB0].portal, 4608.0f, 0.0f, 305.0f),
	      "cross: the longest border edge's midpoint is the portal");
	// A's node 0 at (4000, 0, 0); B's node 0 at (5000, 0, 0) and node 1 at (5100, 0, 300).
	Check(toB1 >= 0 && std::fabs(out[toB1].cost - (float)std::sqrt(1100.0 * 1100.0 + 300.0 * 300.0)) < 1e-2f
	      && toB0 >= 0 && std::fabs(out[toB0].cost - 1000.0f) < 1e-2f,
	      "cross: the cost is the world distance between the centres");
	Check(toB1 >= 0 && out[toB1].water == 200 && toB0 >= 0 && out[toB0].water == 7,
	      "cross: a resolved arc carries its target's water");
	nb.count = 1;
	long noBlock = Stats().crossNoBlock;
	Check(CgCrossArcs(A, 0, FindNeighbour, &nb, out, 8) == 0, "cross: a missing neighbour resolves nothing");
	Check(Stats().crossNoBlock == noBlock + 3, "cross: a missing neighbour counts crossNoBlock");
	CgPublishBase(0, A);
	CgPublishBase(1, B);
}

// The rule's near block: uid 0x0000, one node at (4000, 0, 0), one border toward 0x0001 with the
// given faces over the edge (4608, 0, z - 5)-(4608, 0, z + 5), so its portal is (4608, 0, z).
static CgBlock* NearBlock(int face, int oppFace, float z)
{
	TileGraph g;
	g.interiorsDropped = g.bordersSkipped = 0;
	AddSection(&g, 0x0000, TGS_EXTERIOR, 1, 4000.0f);
	float a[3], b[3];
	SetV(a, 4608.0f, 0.0f, z - 5.0f);
	SetV(b, 4608.0f, 0.0f, z + 5.0f);
	AddBorder(&g, 0x0001, face, oppFace, 0, a, b);
	return CgBlockFromTile(g, 0, CG_BASE, 0);
}

// The rule's far block: uid 0x0001 with node 0 at (4600, 0, 0) and node 1 at (4700, 0, 0), boxes 10
// either side in x and z, so only node 0's box widened by the slack holds a portal at (4608, 0, 0);
// each far border lies over (4608, 0, z - 5)-(4608, 0, z + 5).
static CgBlock* FarBlock(int source, const FarBorder* fb, int count)
{
	TileGraph g;
	g.interiorsDropped = g.bordersSkipped = 0;
	AddSection(&g, 0x0001, TGS_EXTERIOR, 2, 4600.0f);
	for (int i = 0; i < count; ++i)
	{
		float a[3], b[3];
		SetV(a, 4608.0f, 0.0f, fb[i].z - 5.0f);
		SetV(b, 4608.0f, 0.0f, fb[i].z + 5.0f);
		AddBorder(&g, 0x0000, fb[i].face, fb[i].oppFace, fb[i].from, a, b);
	}
	return CgBlockFromTile(g, 0, source, CgStoreGen());
}

// The near border is faces 10 / 20; each shape's far border names it as its row says.
static const Shape kMirrored     = { 10, 20, 0.0f,   CG_BASE, 1, { { 20, 10, 1, 0.0f } },   CG_BORDER_MIRRORED,  1 };
static const Shape kNamesOurFace = { 10, 20, 0.0f,   CG_BASE, 1, { { 21, 10, 1, 0.0f } },   CG_BORDER_ONE_SIDED, 1 };
static const Shape kAtOppFace    = { 10, 20, 0.0f,   CG_BASE, 1, { { 20, 11, 1, 0.0f } },   CG_BORDER_ONE_SIDED, 1 };
static const Shape kOtherPortal  = { 10, 20, 0.0f,   CG_SAVE, 1, { { 21, 10, 1, 100.0f } }, CG_BORDER_DROPPED,  -1 };
static const Shape kSaveNamed    = { 10, 20, 0.0f,   CG_SAVE, 1, { { 21, 10, 1, 0.0f } },   CG_BORDER_ONE_SIDED, 1 };
static const Shape kNearOnlyBase = { 10, 20, 0.0f,   CG_BASE, 0, { { 0, 0, 0, 0.0f } },     CG_BORDER_ONE_SIDED, 0 };
static const Shape kNearOnlyLive = { 10, 20, 0.0f,   CG_LIVE, 0, { { 0, 0, 0, 0.0f } },     CG_BORDER_ONE_SIDED, 0 };
static const Shape kNearOnlySave = { 10, 20, 0.0f,   CG_SAVE, 0, { { 0, 0, 0, 0.0f } },     CG_BORDER_DROPPED,  -1 };
static const Shape kNoBox        = { 10, 20, 200.0f, CG_BASE, 0, { { 0, 0, 0, 0.0f } },     CG_BORDER_DROPPED,  -1 };
static const Shape kSlack19      = { 10, 20, 0.0f,   CG_SAVE, 1, { { 21, 10, 1, 19.0f } },  CG_BORDER_ONE_SIDED, 1 };
static const Shape kSlack21      = { 10, 20, 0.0f,   CG_SAVE, 1, { { 21, 10, 1, 21.0f } },  CG_BORDER_DROPPED,  -1 };

// Resolves the shape's near node and checks the arc against its class and target, and that the
// call counted exactly its class (one crossOneSided, one crossDropped, or nothing when mirrored).
static bool ResolvesAs(const Shape& sh)
{
	CgBlock* N = NearBlock(sh.face, sh.oppFace, sh.nearZ);
	CgBlock* F = FarBlock(sh.source, sh.farBorders, sh.farCount);
	bool ok = N && F;
	if (ok)
	{
		Neighbours nb;
		memset(&nb, 0, sizeof(nb));
		nb.block[0] = N; nb.uid[0] = 0x0000; nb.dir[0] = 0;
		nb.block[1] = F; nb.uid[1] = 0x0001; nb.dir[1] = 1;
		nb.count = 2;
		CgResolved out[4];
		CgStats s0 = Stats();
		int n = CgCrossArcs(N, 0, FindNeighbour, &nb, out, 4);
		CgStats s1 = Stats();
		long one = s1.crossOneSided - s0.crossOneSided, drop = s1.crossDropped - s0.crossDropped;
		ok = s1.crossNoBlock == s0.crossNoBlock;
		if (sh.cls == CG_BORDER_DROPPED)
			ok = ok && n == 0 && drop == 1 && one == 0;
		else
			ok = ok && n == 1 && out[0].dirIndex == 1 && out[0].node == sh.target && drop == 0
			     && one == (sh.cls == CG_BORDER_ONE_SIDED ? 1 : 0);
	}
	if (N) CgPublishBase(1000, N);
	if (F) CgPublishBase(1001, F);
	return ok;
}

// A near node with three borders: one named at the portal by a save far block (one-sided), one the
// far block does not name (dropped), one toward a section with no block; resolved twice.
static void CheckCrossCountsPerCall()
{
	TileGraph g;
	g.interiorsDropped = g.bordersSkipped = 0;
	AddSection(&g, 0x0000, TGS_EXTERIOR, 1, 4000.0f);
	float a[3], b[3], c[3], d[3];
	SetV(a, 4608.0f, 0.0f, -5.0f);
	SetV(b, 4608.0f, 0.0f, 5.0f);
	SetV(c, 4608.0f, 0.0f, 195.0f);
	SetV(d, 4608.0f, 0.0f, 205.0f);
	AddBorder(&g, 0x0001, 10, 20, 0, a, b);
	AddBorder(&g, 0x0001, 12, 22, 0, c, d);
	AddBorder(&g, 0x0002, 14, 24, 0, a, b);
	CgBlock* N = CgBlockFromTile(g, 0, CG_BASE, 0);
	CgBlock* F = FarBlock(CG_SAVE, kSaveNamed.farBorders, 1);
	Neighbours nb;
	memset(&nb, 0, sizeof(nb));
	nb.block[0] = N; nb.uid[0] = 0x0000; nb.dir[0] = 0;
	nb.block[1] = F; nb.uid[1] = 0x0001; nb.dir[1] = 1;
	nb.count = 2;
	CgResolved out[4];
	CgStats s0 = Stats();
	int n1 = (N && F) ? CgCrossArcs(N, 0, FindNeighbour, &nb, out, 4) : -1;
	CgStats s1 = Stats();
	int n2 = (N && F) ? CgCrossArcs(N, 0, FindNeighbour, &nb, out, 4) : -1;
	CgStats s2 = Stats();
	Check(n1 == 1 && n2 == 1
	      && s1.crossOneSided - s0.crossOneSided == 1 && s1.crossDropped - s0.crossDropped == 1
	      && s1.crossNoBlock - s0.crossNoBlock == 1
	      && s2.crossOneSided - s0.crossOneSided == 2 && s2.crossDropped - s0.crossDropped == 2
	      && s2.crossNoBlock - s0.crossNoBlock == 2,
	      "cross: drops and one-sided borders are counted once per call");
	if (N) CgPublishBase(1002, N);
	if (F) CgPublishBase(1003, F);
}

static void CheckCrossRule()
{
	Fresh();
	Check(ResolvesAs(kMirrored) && ResolvesAs(kNamesOurFace),
	      "cross: a far border naming our face at the same portal resolves through it and counts one-sided");
	Check(ResolvesAs(kAtOppFace), "cross: a far border at the face our border names, at the same portal, resolves through it");
	Check(ResolvesAs(kOtherPortal), "cross: a far border naming our face at another portal is not taken");
	Check(ResolvesAs(kSaveNamed), "cross: a far border naming our face at the same portal resolves into a save far block");
	Check(ResolvesAs(kNearOnlyBase) && ResolvesAs(kNearOnlyLive),
	      "cross: a near-only border resolves by the portal into a live or base far block and counts one-sided");
	Check(ResolvesAs(kNearOnlySave), "cross: a near-only border into a save far block is dropped and counted");
	Check(ResolvesAs(kNoBox), "cross: a portal no widened far box holds is dropped and counted");
	Check(ResolvesAs(kSlack19) && ResolvesAs(kSlack21),
	      "cross: the portal slack takes a far portal 19 units away and refuses one 21 units away");
	CheckCrossCountsPerCall();

	bool agree = true;
	const Shape* shapes[] = { &kMirrored, &kNamesOurFace, &kAtOppFace, &kOtherPortal, &kSaveNamed, &kNearOnlyBase,
	                          &kNearOnlyLive, &kNearOnlySave, &kNoBox, &kSlack19, &kSlack21 };
	for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i)
	{
		CgBlock* N = NearBlock(shapes[i]->face, shapes[i]->oppFace, shapes[i]->nearZ);
		CgBlock* F = FarBlock(shapes[i]->source, shapes[i]->farBorders, shapes[i]->farCount);
		agree = agree && N && F && N->borderCount == 1 && CgClassifyBorder(N, N->borders[0], F) == shapes[i]->cls
		        && CgClassifyBorder(N, N->borders[0], NULL) == CG_BORDER_DROPPED;
		if (N) CgPublishBase(1004, N);
		if (F) CgPublishBase(1005, F);
	}
	Check(agree, "cross: the classifier agrees with the resolution on every shape above");
}

// ---- Cache records -------------------------------------------------------------------------------

static TileGraph CacheTile(float shift)
{
	TileGraph g;
	g.interiorsDropped = 2;
	g.bordersSkipped = 1;
	AddSection(&g, 0x0507, TGS_EXTERIOR, 3, shift);
	g.nodes[1].water = 254;
	g.nodes[2].water = 255;
	float a[3], b[3];
	SetV(a, 1.0f, 2.0f, 3.0f);
	SetV(b, 4.0f, 5.0f, 6.0f);
	AddBorder(&g, 0x0508, 7, 9, 1, a, b);
	AddSection(&g, (int)0x8040086e, TGS_INTERIOR, 2, 900.0f + shift);
	g.sections.back().origin[1] = 2215.63f;
	AddBorder(&g, 0x0507, 1, 2, -1, a, b);
	return g;
}

static bool SameTile(const TileGraph& x, const TileGraph& y)
{
	if (x.sections.size() != y.sections.size() || x.nodes.size() != y.nodes.size() || x.arcs.size() != y.arcs.size()
	    || x.borders.size() != y.borders.size() || x.interiorsDropped != y.interiorsDropped
	    || x.bordersSkipped != y.bordersSkipped)
		return false;
	for (size_t i = 0; i < x.sections.size(); ++i)
	{
		const TgSection& a = x.sections[i];
		const TgSection& b = y.sections[i];
		if (a.uid != b.uid || a.kind != b.kind || a.firstNode != b.firstNode || a.nodeCount != b.nodeCount
		    || a.firstBorder != b.firstBorder || a.borderCount != b.borderCount || memcmp(a.origin, b.origin, sizeof(a.origin)))
			return false;
	}
	return (x.nodes.empty() || !memcmp(&x.nodes[0], &y.nodes[0], x.nodes.size() * sizeof(TgNode)))
	    && (x.arcs.empty() || !memcmp(&x.arcs[0], &y.arcs[0], x.arcs.size() * sizeof(TgArc)))
	    && (x.borders.empty() || !memcmp(&x.borders[0], &y.borders[0], x.borders.size() * sizeof(TgBorder)));
}

// A file of two tiles: 3.4 (size 1000, mtime 77) and 5.4 (size 2000, mtime 88).
static void TwoTileFile(std::vector<unsigned char>* file)
{
	std::vector<unsigned char> payload;
	std::vector<CgCacheIndexEntry> entries;
	for (int t = 0; t < 2; ++t)
	{
		CgCacheIndexEntry e;
		memset(&e, 0, sizeof(e));
		e.gx = (short)(t == 0 ? 3 : 5);
		e.gy = 4;
		e.fileSize = t == 0 ? 1000u : 2000u;
		e.mtime = t == 0 ? 77u : 88u;
		e.payloadOffset = (unsigned)payload.size();
		CgCacheEncodeTile(CacheTile(100.0f * (float)t), &payload);
		e.payloadBytes = (unsigned)payload.size() - e.payloadOffset;
		e.sectionCount = 2;
		entries.push_back(e);
	}
	CgCacheBuild(entries, payload, file);
}

static void CheckCache()
{
	Check(CgCrc32("123456789", 9) == 0xCBF43926u, "cache: CRC32 of 123456789 is CBF43926");

	TileGraph in = CacheTile(0.0f), out;
	std::vector<unsigned char> rec;
	CgCacheEncodeTile(in, &rec);
	bool decoded = CgCacheDecodeTile(&rec[0], rec.size(), &out);
	bool shortRefused = !CgCacheDecodeTile(&rec[0], rec.size() - 1, &out);
	CgCacheDecodeTile(&rec[0], rec.size(), &out);
	Check(decoded && shortRefused && SameTile(in, out) && out.sections[1].uid == (int)0x8040086e,
	      "cache: a tile record round-trips");
	Check(decoded && out.nodes.size() == 5 && out.nodes[0].water == 0 && out.nodes[1].water == 254
	      && out.nodes[2].water == 255 && out.nodes[3].water == 0,
	      "cache: a tile record keeps its water bytes");

	TileGraph high = CacheTile(0.0f), low = CacheTile(0.0f), edge = CacheTile(0.0f);
	high.nodes[0].water = 256;
	low.nodes[4].water = -1;
	edge.nodes[0].water = 255;
	std::vector<unsigned char> highRec, lowRec, edgeRec;
	CgCacheEncodeTile(high, &highRec);
	CgCacheEncodeTile(low, &lowRec);
	CgCacheEncodeTile(edge, &edgeRec);
	Check(!CgCacheDecodeTile(&highRec[0], highRec.size(), &out) && !CgCacheDecodeTile(&lowRec[0], lowRec.size(), &out)
	      && CgCacheDecodeTile(&edgeRec[0], edgeRec.size(), &out) && out.nodes[0].water == 255,
	      "cache: a node water byte outside 0..255 is refused");

	std::vector<unsigned char> file;
	TwoTileFile(&file);
	CgCacheCheck valid = CgCacheValidate(&file[0], file.size());
	const CgCacheIndexEntry* e0 = CgCacheFind(&file[0], file.size(), 3, 4, 1000u, 77u);
	const CgCacheHeader* h = (const CgCacheHeader*)&file[0];
	size_t at = (size_t)h->payloadOffset + (e0 ? e0->payloadOffset : 0) + 20;
	file[at] ^= 0x5A;
	Check(valid == CGC_OK && e0 != NULL && CgCacheValidate(&file[0], file.size()) == CGC_OK
	      && CgCacheFind(&file[0], file.size(), 3, 4, 1000u, 77u) == NULL
	      && CgCacheFind(&file[0], file.size(), 5, 4, 2000u, 88u) != NULL,
	      "cache: a record whose CRC differs is refused");

	TwoTileFile(&file);
	Check(CgCacheFind(&file[0], file.size(), 5, 4, 2001u, 88u) == NULL
	      && CgCacheFind(&file[0], file.size(), 5, 4, 2000u, 89u) == NULL
	      && CgCacheFind(&file[0], file.size(), 5, 4, 2000u, 88u) != NULL,
	      "cache: a changed size or mtime misses");

	std::vector<unsigned char> other(file);
	CgCacheIndexEntry* idx = (CgCacheIndexEntry*)&other[sizeof(CgCacheHeader)];
	idx[1].readerVersion = (unsigned)TAGFILE_READER_VERSION + 1;
	std::vector<unsigned char> otherHead(file);
	((CgCacheHeader*)&otherHead[0])->readerVersion = (unsigned)TAGFILE_READER_VERSION + 1;
	Check(CgCacheFind(&other[0], other.size(), 5, 4, 2000u, 88u) == NULL
	      && CgCacheValidate(&otherHead[0], otherHead.size()) == CGC_READER,
	      "cache: another reader version misses");

	std::vector<unsigned char> badMagic(file), badVersion(file);
	((CgCacheHeader*)&badMagic[0])->magic ^= 1u;
	((CgCacheHeader*)&badVersion[0])->version = PLANNER_CACHE_VERSION + 1;
	Check(CgCacheValidate(&badMagic[0], badMagic.size()) == CGC_MAGIC
	      && CgCacheValidate(&badVersion[0], badVersion.size()) == CGC_VERSION,
	      "cache: a bad magic or version is refused");

	// A version-1 file with its header CRC recomputed, so only the version can refuse it.
	std::vector<unsigned char> v1(file);
	CgCacheHeader* v1h = (CgCacheHeader*)&v1[0];
	v1h->version = 1;
	std::vector<unsigned char> crcInput(v1.begin(), v1.begin() + 32);
	crcInput.insert(crcInput.end(), v1.begin() + v1h->indexOffset, v1.begin() + v1h->indexOffset + v1h->indexBytes);
	v1h->headerCrc = CgCrc32(&crcInput[0], crcInput.size());
	Check(CgCacheValidate(&v1[0], v1.size()) == CGC_VERSION, "cache: a version-1 file is refused");
}

// ---- Read whole ----------------------------------------------------------------------------------

static bool FakeOpen(void* ctx, const wchar_t*)
{
	((FakeFile*)ctx)->log += "open,";
	return true;
}

static bool FakeSize(void* ctx, unsigned __int64* out)
{
	FakeFile* f = (FakeFile*)ctx;
	f->log += "size,";
	*out = f->bytes.size();
	return true;
}

static bool FakeRead(void* ctx, void* buf, size_t n)
{
	FakeFile* f = (FakeFile*)ctx;
	f->log += "read,";
	if (f->failRead || n > f->bytes.size())
		return false;
	memcpy(buf, &f->bytes[0], n);
	return true;
}

static void FakeClose(void* ctx)
{
	((FakeFile*)ctx)->log += "close,";
}

static void CheckReadWhole()
{
	FakeFile f;
	f.failRead = false;
	for (int i = 0; i < 100; ++i)
		f.bytes.push_back((unsigned char)i);
	CgFileOps ops = { &f, FakeOpen, FakeSize, FakeRead, FakeClose };
	std::vector<unsigned char> got;
	bool ok = CgReadWhole(ops, L"tile1.2.hkt", 1000, &got);
	Check(ok && f.log == "open,size,read,close," && got.size() == 100 && got[99] == 99,
	      "read-whole: the file is closed before the bytes are returned");

	f.log.clear();
	f.failRead = true;
	ok = CgReadWhole(ops, L"tile1.2.hkt", 1000, &got);
	Check(!ok && f.log == "open,size,read,close," && got.empty(), "read-whole: a failed read still closes");

	f.log.clear();
	f.failRead = false;
	ok = CgReadWhole(ops, L"tile1.2.hkt", 99, &got);
	Check(!ok && f.log == "open,size,close," && got.empty(), "read-whole: a file over the cap is refused and closed");
}

static void CheckCacheVersions()
{
	std::vector<unsigned char> file;
	TwoTileFile(&file);
	const CgCacheHeader* h = (const CgCacheHeader*)&file[0];
	bool carries = h->extractVersion == TG_EXTRACT_VERSION && CgCacheValidate(&file[0], file.size()) == CGC_OK;
	((CgCacheHeader*)&file[0])->extractVersion = TG_EXTRACT_VERSION - 1;
	Check(CgCacheValidate(&file[0], file.size()) == CGC_EXTRACT,
	      "cache: a file at an older extractor version is refused");
	Check(carries, "cache: a rebuilt file carries the extractor version and validates");

	TileGraph in = CacheTile(0.0f), out;
	std::vector<unsigned char> plain, tail;
	CgCacheEncodeTile(in, &plain);
	CgCacheEncodeTile(in, &tail, 1u);
	Check(CgCacheTileFlags(&plain[0], plain.size()) == 0 && CgCacheTileFlags(&tail[0], tail.size()) == 1
	      && CgCacheTileFlags(&tail[0], 4) == 0
	      && CgCacheDecodeTile(&plain[0], plain.size(), &out) && CgCacheDecodeTile(&tail[0], tail.size(), &out),
	      "cache: a tile's tail bit survives the record");

	std::vector<unsigned char> high(tail);
	unsigned two = 2u;
	memcpy(&high[12], &two, sizeof(two));
	Check(!CgCacheDecodeTile(&high[0], high.size(), &out), "cache: a tile head flag above bit 0 is refused");
}

int main()
{
	CheckTakeRefusesReaders();
	CheckRanksAndGenerations();
	CheckLiveOrder();
	CheckDeferredPublish();
	CheckRetireDrain();
	CheckUids();
	CheckLive();
	CheckBlocks();
	CheckCross();
	CheckCrossRule();
	CheckCache();
	CheckReadWhole();
	CheckCacheVersions();
	CgStoreDestroy();
	return CheckExit("coarse_graph_units");
}
