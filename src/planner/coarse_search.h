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
const int COARSE_PROBE_MAX   = 512;      // nodes the goal-side probe may gather

struct CoarseArc { unsigned to; float cost; };

struct CoarseGraphOps
{
	void* ctx;
	// Writes node's arcs (at most max); returns the count, or -1 when the node is unknown.
	int  (*arcs)(void* ctx, unsigned node, CoarseArc* out, int max);
	// The node's world position; false when unknown.
	bool (*position)(void* ctx, unsigned node, float out[3]);
	// Non-zero when every arc into node has its reverse among node's own reported arcs (its cost
	// may differ); zero when that is not known, or the node is unknown. NULL: no goal-side probe.
	// The store adapter answers zero for a node whose border was dropped, resolved only by the far
	// block's geometry, or aimed at a far block it could not read, and for a node whose intra arcs
	// plus borders pass COARSE_ARCS_MAX.
	int  (*inboundMirrored)(void* ctx, unsigned node);
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
	int      expanded;                  // nodes expanded, the probe's included
	int      probed;                    // nodes the goal-side probe expanded
};

// The goal-side probe: the nodes reachable from goal over ops' arcs, gathered breadth first, at
// most maxNodes of them (clamped to COARSE_PROBE_MAX). CPV_MET when it reaches start; CPV_CLOSED
// when the goal's closure is exhausted within the bound without it and every node expanded answered
// inboundMirrored; CPV_OPEN when the closure passes the bound, when an expanded node does not answer
// inboundMirrored, or when ops has no inboundMirrored. *expanded receives the nodes it expanded.
// Pure; a stack table, no allocation.
enum CoarseProbeVerdict { CPV_MET = 0, CPV_CLOSED, CPV_OPEN };
CoarseProbeVerdict CoarseProbeGoal(const CoarseGraphOps& ops, unsigned goal, unsigned start, int maxNodes,
                                   int* expanded);

// With ops.inboundMirrored set, a goal the probe finds CPV_CLOSED answers CS_NO_ROUTE at once: no
// arc enters a closure whose every node mirrors its inbound arcs, so the start outside it cannot
// reach the goal. Otherwise the A* runs from start.
CoarseResult CoarseSearch(const CoarseGraphOps& ops, unsigned start, unsigned goal, int maxNodes,
                          CoarseScratch* s, CoarseRoute* out);
// Searches the probe answered CS_NO_ROUTE, since the process started; the caller's thread.
long CoarseProbeRefusals();

} // namespace planner

#endif
