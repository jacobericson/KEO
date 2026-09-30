// coarse_search.cpp - A* over a coarse graph through its operations struct. Pure; runs on the
// caller's thread, one search at a time per scratch, and takes no lock.

#include <cmath>
#include <exception>
#include <vector>
#include "planner/coarse_search.h"

namespace planner {

namespace coarse_search_detail {

// One search's working state over the caller's scratch.
struct Search
{
	const CoarseGraphOps* ops;
	CoarseScratch*        s;
	int                   cap;        // touched-node budget for this search
	int                   heapSize;
	float                 goalPos[3];
};

enum Relaxed { RELAX_OK = 0, RELAX_LIMIT };

} // namespace coarse_search_detail
using namespace coarse_search_detail;

static const int SLOT_NONE = -1;

static unsigned MixKey(unsigned x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static float Distance(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The table position holding node, or the empty position where it would go. The table is at
// least twice the scratch's node count, so an empty position always exists.
static unsigned Probe(const CoarseScratch* s, unsigned node)
{
	unsigned mask = (unsigned)s->key.size() - 1u;
	unsigned i = MixKey(node) & mask;
	while (s->slotOf[i] != SLOT_NONE && s->key[i] != node)
		i = (i + 1u) & mask;
	return i;
}

// Empties the previous search's table entries in reverse insertion order: each entry's probe run
// held only earlier entries when it went in, so every lookup still reaches its own position.
static void ResetTable(CoarseScratch* s)
{
	for (int i = s->touched - 1; i >= 0; --i)
		s->slotOf[Probe(s, s->node[i])] = SLOT_NONE;
	s->touched = 0;
}

// Heap order: the lower f, then the lower g, then the lower node key.
static bool Before(const CoarseScratch* s, int a, int b)
{
	if (s->f[a] != s->f[b]) return s->f[a] < s->f[b];
	if (s->g[a] != s->g[b]) return s->g[a] < s->g[b];
	return s->node[a] < s->node[b];
}

static void Place(CoarseScratch* s, int pos, int slot)
{
	s->heap[pos] = slot;
	s->heapPos[slot] = pos;
}

static void SiftUp(Search* st, int pos)
{
	CoarseScratch* s = st->s;
	int slot = s->heap[pos];
	while (pos > 0)
	{
		int up = (pos - 1) / 2;
		if (!Before(s, slot, s->heap[up])) break;
		Place(s, pos, s->heap[up]);
		pos = up;
	}
	Place(s, pos, slot);
}

static void SiftDown(Search* st, int pos)
{
	CoarseScratch* s = st->s;
	int slot = s->heap[pos];
	for (;;)
	{
		int child = pos * 2 + 1;
		if (child >= st->heapSize) break;
		if (child + 1 < st->heapSize && Before(s, s->heap[child + 1], s->heap[child])) ++child;
		if (!Before(s, s->heap[child], slot)) break;
		Place(s, pos, s->heap[child]);
		pos = child;
	}
	Place(s, pos, slot);
}

static void Push(Search* st, int slot)
{
	Place(st->s, st->heapSize, slot);
	++st->heapSize;
	SiftUp(st, st->heapSize - 1);
}

static int Pop(Search* st)
{
	CoarseScratch* s = st->s;
	int top = s->heap[0];
	s->heapPos[top] = -1;
	--st->heapSize;
	if (st->heapSize > 0)
	{
		Place(s, 0, s->heap[st->heapSize]);
		SiftDown(st, 0);
	}
	return top;
}

// A new node takes the next slot; a node without a position is unknown to the graph and is not
// entered. RELAX_LIMIT when the budget has no slot left.
static Relaxed Enter(Search* st, unsigned node, int parent, float g, unsigned pos)
{
	CoarseScratch* s = st->s;
	if (s->touched >= st->cap) return RELAX_LIMIT;
	float p[3];
	if (!st->ops->position(st->ops->ctx, node, p)) return RELAX_OK;
	int slot = s->touched++;
	s->key[pos]     = node;
	s->slotOf[pos]  = slot;
	s->node[slot]   = node;
	s->g[slot]      = g;
	s->f[slot]      = g + Distance(p, st->goalPos);
	s->parent[slot] = parent;
	Push(st, slot);
	return RELAX_OK;
}

// A node reached with a lower g than it holds is updated and re-opened, closed or not.
static Relaxed Relax(Search* st, unsigned node, int parent, float g)
{
	CoarseScratch* s = st->s;
	unsigned pos = Probe(s, node);
	int slot = s->slotOf[pos];
	if (slot == SLOT_NONE) return Enter(st, node, parent, g, pos);
	if (!(g < s->g[slot])) return RELAX_OK;
	float h = s->f[slot] - s->g[slot];
	s->g[slot]      = g;
	s->f[slot]      = g + h;
	s->parent[slot] = parent;
	if (s->heapPos[slot] >= 0) SiftUp(st, s->heapPos[slot]);
	else Push(st, slot);
	return RELAX_OK;
}

static CoarseResult BuildRoute(const CoarseScratch* s, int goalSlot, CoarseRoute* out)
{
	int count = 0;
	for (int at = goalSlot; at >= 0; at = s->parent[at])
		if (++count > COARSE_ROUTE_MAX) return CS_ROUTE_TOO_LONG;
	int i = count;
	for (int at = goalSlot; at >= 0; at = s->parent[at])
		out->nodes[--i] = s->node[at];
	out->count = count;
	out->cost  = s->g[goalSlot];
	return CS_FOUND;
}

// A C++ allocation failure answers false; a structured exception is never caught here.
bool CoarseScratchInit(CoarseScratch* s, int maxNodes)
{
	if (!s || maxNodes < 1) return false;
	if (maxNodes > COARSE_SCRATCH_MAX) maxNodes = COARSE_SCRATCH_MAX;
	size_t table = 1;
	while (table < (size_t)maxNodes * 2u) table <<= 1;
	try
	{
		s->key.assign(table, 0u);
		s->slotOf.assign(table, SLOT_NONE);
		s->node.assign((size_t)maxNodes, 0u);
		s->g.assign((size_t)maxNodes, 0.0f);
		s->f.assign((size_t)maxNodes, 0.0f);
		s->parent.assign((size_t)maxNodes, -1);
		s->heapPos.assign((size_t)maxNodes, -1);
		s->heap.assign((size_t)maxNodes, 0);
	}
	catch (const std::exception&)
	{
		s->key.clear(); s->slotOf.clear(); s->node.clear(); s->g.clear(); s->f.clear();
		s->parent.clear(); s->heapPos.clear(); s->heap.clear();
		s->touched = 0;
		return false;
	}
	s->touched = 0;
	return true;
}

CoarseResult CoarseSearch(const CoarseGraphOps& ops, unsigned start, unsigned goal, int maxNodes,
                          CoarseScratch* s, CoarseRoute* out)
{
	out->count    = 0;
	out->cost     = 0.0f;
	out->expanded = 0;
	Search st;
	float startPos[3];
	if (!ops.position || !ops.position(ops.ctx, start, startPos) ||
	    !ops.position(ops.ctx, goal, st.goalPos))
		return CS_BAD_ENDPOINT;
	if (start == goal)
	{
		out->count    = 1;
		out->nodes[0] = start;
		return CS_FOUND;
	}
	if (!ops.arcs) return CS_NO_ROUTE;
	if (!s || s->node.empty() || s->slotOf.empty()) return CS_NODE_LIMIT;

	ResetTable(s);
	int size = (int)s->node.size();
	st.ops      = &ops;
	st.s        = s;
	st.cap      = (maxNodes < size) ? maxNodes : size;
	st.heapSize = 0;
	if (st.cap < 1 || Enter(&st, start, -1, 0.0f, Probe(s, start)) == RELAX_LIMIT)
		return CS_NODE_LIMIT;

	CoarseArc arcs[COARSE_ARCS_MAX];
	while (st.heapSize > 0)
	{
		int cur = Pop(&st);
		if (s->node[cur] == goal) return BuildRoute(s, cur, out);
		++out->expanded;
		int count = ops.arcs(ops.ctx, s->node[cur], arcs, COARSE_ARCS_MAX);
		if (count > COARSE_ARCS_MAX) count = COARSE_ARCS_MAX;
		for (int i = 0; i < count; ++i)
		{
			if (!(arcs[i].cost >= 0.0f)) continue;   // a negative or NaN cost is no arc
			if (Relax(&st, arcs[i].to, cur, s->g[cur] + arcs[i].cost) == RELAX_LIMIT)
				return CS_NODE_LIMIT;
		}
	}
	return CS_NO_ROUTE;
}

} // namespace planner
