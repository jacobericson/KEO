// coarse_search.h - A* over a coarse graph described by an operations struct: world-frame arc
// costs, a straight-line heuristic, a node budget. Pure: no Windows, KenshiLib or game header.
// The caller owns the scratch and runs one search at a time on it; main thread in the game.
#ifndef KENSHI_ZONE_OPT_PLANNER_COARSE_SEARCH_H
#define KENSHI_ZONE_OPT_PLANNER_COARSE_SEARCH_H

#include <vector>

namespace planner {

const int COARSE_ROUTE_MAX   = 512;      // nodes on a returned route
const int COARSE_ARCS_MAX    = 64;       // arcs one node may report per expansion
const int COARSE_SCRATCH_MAX = 131072;   // nodes one search may touch

struct CoarseArc { unsigned to; float cost; };

struct CoarseGraphOps
{
	void* ctx;
	// Writes node's arcs (at most max); returns the count, or -1 when the node is unknown.
	int  (*arcs)(void* ctx, unsigned node, CoarseArc* out, int max);
	// The node's world position; false when unknown.
	bool (*position)(void* ctx, unsigned node, float out[3]);
};

// Allocated once, with new, when the planner arms (never a namespace- or file-scope object);
// reused by every search.
struct CoarseScratch
{
	std::vector<unsigned> key;        // open-addressed node -> slot table (size a power of two)
	std::vector<int>      slotOf;
	std::vector<unsigned> node;       // per touched node: its key, g, f, parent, heap position
	std::vector<float>    g, f;
	std::vector<int>      parent, heapPos, heap;
	int touched;
};
bool CoarseScratchInit(CoarseScratch* s, int maxNodes);   // false on an allocation failure

enum CoarseResult { CS_FOUND = 0, CS_NO_ROUTE, CS_NODE_LIMIT, CS_BAD_ENDPOINT, CS_ROUTE_TOO_LONG };

struct CoarseRoute
{
	int      count;                     // nodes, start first, goal last
	unsigned nodes[COARSE_ROUTE_MAX];
	float    cost;                      // world units
	int      expanded;                  // nodes expanded
};

CoarseResult CoarseSearch(const CoarseGraphOps& ops, unsigned start, unsigned goal, int maxNodes,
                          CoarseScratch* s, CoarseRoute* out);

} // namespace planner

#endif
