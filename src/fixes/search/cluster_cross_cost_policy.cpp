// cluster_cross_cost_policy.cpp - Rewrites a registered graph instance's cross-tile owned edges,
// and their reciprocals in the neighbours, with the world-frame distance between the two
// cluster centres. Pure: reads and writes only the instances and the collection it is handed,
// takes no lock and allocates nothing; the caller holds whatever keeps them stable.
#include "fixes/search/cluster_cross_cost_policy.h"
#include <math.h>
#include <string.h>

namespace cluster_cross_cost_policy_detail {

// An instance's owned-edge tables: node -> owned-node index, the owned-node records
// {startEdge, numEdges}, and the owned edges, which start at edge index base.
struct OwnedTables
{
	const int*     map;
	int            mapCount;
	const int*     records;
	int            recordCount;
	int            base;
	unsigned char* edges;
	int            edgeCount;
};

} // namespace cluster_cross_cost_policy_detail
using namespace cluster_cross_cost_policy_detail;

static const unsigned TARGET_NODE_MASK     = 0x3FFFFFu;
static const unsigned TARGET_SECTION_SHIFT = 22;

static const float* Floats(const void* inst, size_t off)
{
	return (const float*)((const unsigned char*)inst + off);
}

static int IntAt(const void* p, size_t off)
{
	return *(const int*)((const unsigned char*)p + off);
}

template <class T>
static T PtrAt(const void* p, size_t off)
{
	return *(T const*)((const unsigned char*)p + off);
}

// x*row0 + y*row1 + z*row2 + translation, over x, y and z.
static void WorldPoint(const void* inst, unsigned node, float out[3])
{
	const float* pos = PtrAt<const float*>(inst, OFF_GI_POSITIONS) + 4 * (size_t)node;
	const float* r0 = Floats(inst, OFF_GI_ROW0);
	const float* r1 = Floats(inst, OFF_GI_ROW1);
	const float* r2 = Floats(inst, OFF_GI_ROW2);
	const float* t  = Floats(inst, OFF_GI_TRANSLATION);
	for (int k = 0; k < 3; ++k)
		out[k] = pos[0] * r0[k] + pos[1] * r1[k] + pos[2] * r2[k] + t[k];
}

float CrossCostWorldDistance(const void* instA, unsigned nodeA, const void* instB, unsigned nodeB)
{
	float a[3], b[3];
	WorldPoint(instA, nodeA, a);
	WorldPoint(instB, nodeB, b);
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

unsigned short CrossCostHalf(float d)
{
	const float scaled = d * CROSS_COST_SCALE;
	unsigned bits;
	memcpy(&bits, &scaled, sizeof(bits));
	return (unsigned short)(bits >> 16);
}

static bool LoadTables(const void* inst, OwnedTables* t)
{
	t->map         = PtrAt<const int*>(inst, OFF_GI_OWNED_MAP);
	t->mapCount    = IntAt(inst, OFF_GI_OWNED_MAP_COUNT);
	t->records     = PtrAt<const int*>(inst, OFF_GI_OWNED_NODES);
	t->recordCount = IntAt(inst, OFF_GI_OWNED_NODE_COUNT);
	t->base        = IntAt(inst, OFF_GI_BASE_EDGE_COUNT);
	t->edges       = PtrAt<unsigned char*>(inst, OFF_GI_OWNED_EDGES);
	t->edgeCount   = IntAt(inst, OFF_GI_OWNED_EDGE_COUNT);
	return t->map && t->records && t->edges
	    && t->mapCount >= 0 && t->recordCount >= 0 && t->base >= 0 && t->edgeCount >= 0;
}

// node's owned range; false when the node has none, or its record or range is out of bounds.
static bool OwnedRange(const OwnedTables* t, unsigned node, int* start, int* num)
{
	if (node >= (unsigned)t->mapCount)
		return false;
	const int own = t->map[node];
	if (own < 0 || own >= t->recordCount)
		return false;
	*start = t->records[2 * own];
	*num   = t->records[2 * own + 1];
	return true;
}

// Whether the owned range [start, start + num) lies inside the owned-edge array.
static bool RangeInOwned(const OwnedTables* t, int start, int num)
{
	return start >= t->base && num >= 0
	    && (long long)start - t->base + num <= (long long)t->edgeCount;
}

static unsigned char* EdgeAt(const OwnedTables* t, int start, int i)
{
	return t->edges + 8 * (size_t)(start + i - t->base);
}

static unsigned short EdgeFlags(const unsigned char* e) { return *(const unsigned short*)(e + 2); }
static unsigned EdgeTarget(const unsigned char* e)      { return *(const unsigned*)(e + 4); }

// The neighbour's edge back to (node, mySec), rewritten; false when it is absent.
static bool WriteReciprocal(void* nb, unsigned m, unsigned node, unsigned mySec, unsigned short cost)
{
	OwnedTables t;
	int start, num;
	if (!LoadTables(nb, &t) || !OwnedRange(&t, m, &start, &num) || !RangeInOwned(&t, start, num))
		return false;
	const unsigned back = node | (mySec << TARGET_SECTION_SHIFT);
	for (int i = 0; i < num; ++i)
	{
		unsigned char* e = EdgeAt(&t, start, i);
		if ((EdgeFlags(e) & CROSS_EDGE_FLAG) && EdgeTarget(e) == back)
		{
			*(unsigned short*)e = cost;
			return true;
		}
	}
	return false;
}

// The neighbour instance a target section names, or NULL when the section is past the
// collection or holds no graph instance.
static void* Neighbour(const void* coll, unsigned tsec)
{
	const int count = IntAt(coll, OFF_CCC_INSTANCE_COUNT);
	const unsigned char* infos = PtrAt<const unsigned char*>(coll, OFF_CCC_INSTANCE_DATA);
	if (count <= 0 || tsec >= (unsigned)count || !infos)
		return NULL;
	return PtrAt<void*>(infos + SIZE_CCC_INSTANCE_INFO * (size_t)tsec, OFF_CCC_INFO_GRAPH);
}

static void RewriteLink(void* inst, void* coll, unsigned node, unsigned mySec, unsigned char* e,
                        CrossCostCounts* out)
{
	const unsigned target = EdgeTarget(e);
	void* nb = Neighbour(coll, target >> TARGET_SECTION_SHIFT);
	const unsigned m = target & TARGET_NODE_MASK;
	// The owned map holds one entry per node (the instance build, 0xD27740, sizes it so), so its
	// count bounds the positions array too, here and in CrossCostRewrite's node loop.
	const int nbNodes = nb ? IntAt(nb, OFF_GI_OWNED_MAP_COUNT) : 0;
	if (!nb || nbNodes <= 0 || m >= (unsigned)nbNodes
	    || !PtrAt<const float*>(inst, OFF_GI_POSITIONS) || !PtrAt<const float*>(nb, OFF_GI_POSITIONS))
	{
		++out->skipped;
		return;
	}
	const unsigned short cost = CrossCostHalf(CrossCostWorldDistance(inst, node, nb, m));
	*(unsigned short*)e = cost;
	// Without a reciprocal the forward edge stays rewritten; the link is counted skipped.
	if (WriteReciprocal(nb, m, node, mySec, cost))
		++out->rewritten;
	else
		++out->skipped;
}

void CrossCostRewrite(void* inst, void* coll, CrossCostCounts* out)
{
	OwnedTables t;
	if (!LoadTables(inst, &t))
		return;
	const unsigned mySec = (unsigned)IntAt(inst, OFF_GI_SECTION);
	for (unsigned node = 0; node < (unsigned)t.mapCount; ++node)
	{
		int start, num;
		if (!OwnedRange(&t, node, &start, &num))
			continue;
		if (!RangeInOwned(&t, start, num))
		{
			++out->skipped;
			continue;
		}
		for (int i = 0; i < num; ++i)
		{
			unsigned char* e = EdgeAt(&t, start, i);
			if (!(EdgeFlags(e) & CROSS_EDGE_FLAG) || (EdgeTarget(e) >> TARGET_SECTION_SHIFT) == mySec)
				continue;
			++out->links;
			RewriteLink(inst, coll, node, mySec, e, out);
		}
	}
}
