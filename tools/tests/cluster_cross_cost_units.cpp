// The cross-tile cluster link cost, over two fabricated graph instances at the real offsets and a
// two-entry collection: instance A in section 0 with identity rows, instance B in section 1
// rotated 90 degrees about y and translated 460.8 in x, one cluster pair linked both ways with
// the tile-local distance stored. Every expected world point below is worked by hand, so a
// distance that drops the rows, the translation or both is wrong.

#include <cmath>
#include <cstdio>
#include <cstring>
#include "fixes/search/cluster_cross_cost_policy.h"

#include "check.h"

struct Instance
{
	double         align;           // keeps the float rows 8-aligned
	unsigned char  bytes[272];
	float          positions[4][4];
	int            map[4];
	int            records[2][2];
	unsigned char  edges[3][8];
};

struct World
{
	Instance      a;
	Instance      b;
	unsigned char collection[48];
	unsigned char infos[SIZE_CCC_INSTANCE_INFO * 2];
};

static const int A_BASE = 4;
static const int B_BASE = 2;

static unsigned Key(unsigned section, unsigned node) { return node | (section << 22); }

static void SetEdge(unsigned char* e, unsigned short cost, unsigned short flags, unsigned target)
{
	memcpy(e, &cost, 2);
	memcpy(e + 2, &flags, 2);
	memcpy(e + 4, &target, 4);
}

static unsigned short EdgeCost(const unsigned char* e)
{
	unsigned short c;
	memcpy(&c, e, 2);
	return c;
}

static void SetRow(Instance* g, size_t off, float x, float y, float z)
{
	float* r = (float*)(g->bytes + off);
	r[0] = x; r[1] = y; r[2] = z; r[3] = 0.0f;
}

static void Wire(Instance* g, int section, int nodes, int records, int base)
{
	*(float**)(g->bytes + OFF_GI_POSITIONS)      = &g->positions[0][0];
	*(int*)(g->bytes + OFF_GI_BASE_EDGE_COUNT)   = base;
	*(int*)(g->bytes + OFF_GI_SECTION)           = section;
	*(int**)(g->bytes + OFF_GI_OWNED_MAP)        = g->map;
	*(int*)(g->bytes + OFF_GI_OWNED_MAP_COUNT)   = nodes;
	*(int**)(g->bytes + OFF_GI_OWNED_NODES)      = &g->records[0][0];
	*(int*)(g->bytes + OFF_GI_OWNED_NODE_COUNT)  = records;
	*(unsigned char**)(g->bytes + OFF_GI_OWNED_EDGES) = &g->edges[0][0];
	*(int*)(g->bytes + OFF_GI_OWNED_EDGE_COUNT)  = (int)(sizeof(g->edges) / sizeof(g->edges[0]));
}

// The tile-local distance between A node 0 (100, 5, 20) and B node 3 (10, 5, 50): (90, 0, -30).
static unsigned short LocalHalf()
{
	return CrossCostHalf(sqrtf(90.0f * 90.0f + 30.0f * 30.0f));
}

// A: node 0 owns three edges (the cross link to B node 3, a same-section link, and a link
// without the cross flag); node 1 owns nothing. B: node 3 owns the reciprocal link.
static void Init(World* w)
{
	memset(w, 0, sizeof(*w));
	Instance* a = &w->a;
	Instance* b = &w->b;

	Wire(a, 0, 2, 1, A_BASE);
	SetRow(a, OFF_GI_ROW0, 1.0f, 0.0f, 0.0f);
	SetRow(a, OFF_GI_ROW1, 0.0f, 1.0f, 0.0f);
	SetRow(a, OFF_GI_ROW2, 0.0f, 0.0f, 1.0f);
	SetRow(a, OFF_GI_TRANSLATION, 0.0f, 0.0f, 0.0f);
	a->positions[0][0] = 100.0f; a->positions[0][1] = 5.0f; a->positions[0][2] = 20.0f;
	a->positions[1][0] = 7.0f;
	a->map[0] = 0;
	a->map[1] = -1;
	a->records[0][0] = A_BASE;
	a->records[0][1] = 3;
	SetEdge(a->edges[0], LocalHalf(), CROSS_EDGE_FLAG, Key(1, 3));
	SetEdge(a->edges[1], 0x1234, CROSS_EDGE_FLAG, Key(0, 1));
	SetEdge(a->edges[2], 0x2345, 0x0000, Key(1, 3));

	// Rotated 90 degrees about y: x -> (0, 0, -1), y -> (0, 1, 0), z -> (1, 0, 0).
	Wire(b, 1, 4, 1, B_BASE);
	SetRow(b, OFF_GI_ROW0, 0.0f, 0.0f, -1.0f);
	SetRow(b, OFF_GI_ROW1, 0.0f, 1.0f, 0.0f);
	SetRow(b, OFF_GI_ROW2, 1.0f, 0.0f, 0.0f);
	SetRow(b, OFF_GI_TRANSLATION, 460.8f, 0.0f, 0.0f);
	b->positions[3][0] = 10.0f; b->positions[3][1] = 5.0f; b->positions[3][2] = 50.0f;
	b->map[0] = -1; b->map[1] = -1; b->map[2] = -1;
	b->map[3] = 0;
	b->records[0][0] = B_BASE;
	b->records[0][1] = 1;
	SetEdge(b->edges[0], LocalHalf(), CROSS_EDGE_FLAG, Key(0, 0));

	*(unsigned char**)(w->collection + OFF_CCC_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_CCC_INSTANCE_COUNT)            = 2;
	*(void**)(w->infos + OFF_CCC_INFO_GRAPH)                          = a->bytes;
	*(void**)(w->infos + SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH) = b->bytes;
}

// A node 0 in the world: (100, 5, 20). B node 3: 10*(0,0,-1) + 5*(0,1,0) + 50*(1,0,0) +
// (460.8,0,0) = (510.8, 5, -10). The difference is (-410.8, 0, 30).
static float WorldDistance()
{
	const float dx = 100.0f - (50.0f + 460.8f);
	const float dz = 20.0f - (-10.0f);
	return sqrtf(dx * dx + dz * dz);
}

// Only the translation, no rows: B node 3 at (470.8, 5, 50); the difference is (-370.8, 0, -30).
static float TranslationOnlyDistance()
{
	const float dx = 100.0f - (10.0f + 460.8f);
	const float dz = 20.0f - 50.0f;
	return sqrtf(dx * dx + dz * dz);
}

static CrossCostCounts Rewrite(World* w)
{
	CrossCostCounts c = { 0, 0, 0 };
	CrossCostRewrite(w->a.bytes, w->collection, &c);
	return c;
}

static void CheckDistance()
{
	World w;
	Init(&w);
	const float d = CrossCostWorldDistance(w.a.bytes, 0, w.b.bytes, 3);
	Check(fabsf(d - WorldDistance()) < 0.01f, "cross cost: the world-frame distance function");
	Check(fabsf(d - TranslationOnlyDistance()) > 1.0f, "cross cost: a rotated instance's rows are applied");

	const CrossCostCounts c = Rewrite(&w);
	Check(EdgeCost(w.a.edges[0]) == CrossCostHalf(WorldDistance())
	      && EdgeCost(w.a.edges[0]) != LocalHalf(),
	      "cross cost: world-frame distance");
	Check(EdgeCost(w.b.edges[0]) == EdgeCost(w.a.edges[0]),
	      "cross cost: the neighbour's reciprocal edge gets the same cost");
	Check(c.links == 1 && c.rewritten == 1 && c.skipped == 0, "cross cost: one link, rewritten");
	Check(EdgeCost(w.a.edges[1]) == 0x1234, "cross cost: a same-section edge is untouched");
	Check(EdgeCost(w.a.edges[2]) == 0x2345, "cross cost: an edge without the cross flag is untouched");

	unsigned char a1[sizeof(w.a.edges)], b1[sizeof(w.b.edges)];
	memcpy(a1, w.a.edges, sizeof(a1));
	memcpy(b1, w.b.edges, sizeof(b1));
	Rewrite(&w);
	Check(memcmp(a1, w.a.edges, sizeof(a1)) == 0 && memcmp(b1, w.b.edges, sizeof(b1)) == 0,
	      "cross cost: a second rewrite changes nothing");
}

static void CheckSkips()
{
	World w;
	CrossCostCounts c;

	Init(&w);
	SetEdge(w.a.edges[0], LocalHalf(), CROSS_EDGE_FLAG, Key(5, 3));
	c = Rewrite(&w);
	Check(c.links == 1 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf()
	      && EdgeCost(w.b.edges[0]) == LocalHalf(),
	      "cross cost: a section past the count is skipped");

	Init(&w);
	*(void**)(w.infos + SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH) = NULL;
	c = Rewrite(&w);
	Check(c.links == 1 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a NULL neighbour is skipped");

	Init(&w);
	SetEdge(w.a.edges[0], LocalHalf(), CROSS_EDGE_FLAG, Key(1, 7));
	c = Rewrite(&w);
	Check(c.links == 1 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a node past the neighbour's count is skipped");

	Init(&w);
	SetEdge(w.b.edges[0], LocalHalf(), CROSS_EDGE_FLAG, Key(0, 1));
	c = Rewrite(&w);
	Check(c.links == 1 && c.skipped == 1 && c.rewritten == 0
	      && EdgeCost(w.a.edges[0]) == CrossCostHalf(WorldDistance())
	      && EdgeCost(w.b.edges[0]) == LocalHalf(),
	      "cross cost: a missing reciprocal is skipped");

	Init(&w);
	w.a.records[0][0] = A_BASE - 1;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a range starting before the owned edges is skipped whole");

	Init(&w);
	w.a.map[0] = 1;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 0 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: an owned index past the records is not read");

	Init(&w);
	*(int**)(w.a.bytes + OFF_GI_OWNED_MAP) = NULL;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 0 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a NULL owned map returns with nothing counted");

	Init(&w);
	*(int*)(w.a.bytes + OFF_GI_OWNED_NODE_COUNT) = -1;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 0 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a negative count returns with nothing counted");

	Init(&w);
	*(int*)(w.a.bytes + OFF_GI_OWNED_EDGE_COUNT) = -1;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 0 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a negative owned-edge count returns with nothing counted");

	Init(&w);
	w.a.records[0][1] = 4;
	c = Rewrite(&w);
	Check(c.links == 0 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.a.edges[0]) == LocalHalf(),
	      "cross cost: a range past the owned edges is skipped whole");

	Init(&w);
	*(int*)(w.b.bytes + OFF_GI_OWNED_EDGE_COUNT) = 0;
	c = Rewrite(&w);
	Check(c.links == 1 && c.skipped == 1 && c.rewritten == 0 && EdgeCost(w.b.edges[0]) == LocalHalf(),
	      "cross cost: a reciprocal range past the neighbour's owned edges is not written");
}

static void CheckHalf()
{
	// 256 * 1.00390625 = 257.0f, bit pattern 0x43808000.
	Check(CrossCostHalf(256.0f) == 0x4380, "half: the top 16 bits of the scaled float");
	Check(CrossCostHalf(0.0f) == 0x0000, "half: zero stays zero");
}

int main()
{
	CheckDistance();
	CheckSkips();
	CheckHalf();
	return CheckExit("cluster_cross_cost_units");
}
