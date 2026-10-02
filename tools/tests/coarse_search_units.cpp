// The coarse A* over small fixture graphs the suite describes through its own operations struct:
// node keys index a position table, arcs are listed per node in the order they are reported.

#include <cmath>
#include <cstdio>
#include <vector>
#include "planner/coarse_search.h"
#include "planner/plan_policy.h"

#include "check.h"

using namespace planner;

struct Graph
{
	std::vector<float>                   pos;      // three floats per node
	std::vector<int>                     hasPos;
	std::vector<std::vector<CoarseArc> > arcs;
};

static void Reset(Graph* g, int n)
{
	g->pos.assign((size_t)n * 3u, 0.0f);
	g->hasPos.assign((size_t)n, 0);
	g->arcs.assign((size_t)n, std::vector<CoarseArc>());
}

static void At(Graph* g, unsigned node, float x, float y, float z)
{
	g->pos[node * 3u] = x;
	g->pos[node * 3u + 1u] = y;
	g->pos[node * 3u + 2u] = z;
	g->hasPos[node] = 1;
}

static void Arc(Graph* g, unsigned from, unsigned to, float cost)
{
	CoarseArc a;
	a.to = to;
	a.cost = cost;
	g->arcs[from].push_back(a);
}

static int GraphArcs(void* ctx, unsigned node, CoarseArc* out, int max)
{
	const Graph* g = (const Graph*)ctx;
	if (node >= (unsigned)g->arcs.size()) return -1;
	int n = (int)g->arcs[node].size();
	if (n > max) n = max;
	for (int i = 0; i < n; ++i) out[i] = g->arcs[node][(size_t)i];
	return n;
}

static bool GraphPosition(void* ctx, unsigned node, float out[3])
{
	const Graph* g = (const Graph*)ctx;
	if (node >= (unsigned)g->hasPos.size() || !g->hasPos[node]) return false;
	out[0] = g->pos[node * 3u];
	out[1] = g->pos[node * 3u + 1u];
	out[2] = g->pos[node * 3u + 2u];
	return true;
}

static CoarseGraphOps Ops(Graph* g)
{
	CoarseGraphOps ops;
	ops.ctx = g;
	ops.arcs = GraphArcs;
	ops.position = GraphPosition;
	return ops;
}

static bool RouteIs(const CoarseRoute& r, const unsigned* want, int n)
{
	if (r.count != n) return false;
	for (int i = 0; i < n; ++i)
		if (r.nodes[i] != want[i]) return false;
	return true;
}

static bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// The ravine: start 0 and goal 7 are 5 units apart with no arc between them; the only way round is
// a loop through nodes 1-6, each reporting its forward arc first. Node 8 is a dead end beside the
// goal, reachable from the start.
static void BuildRavine(Graph* g)
{
	Reset(g, 9);
	At(g, 0, 0.0f, 0.0f, 0.0f);
	At(g, 7, 5.0f, 0.0f, 0.0f);
	At(g, 1, -200.0f, 0.0f, 0.0f);
	At(g, 2, -200.0f, 0.0f, 300.0f);
	At(g, 3, 0.0f, 0.0f, 500.0f);
	At(g, 4, 200.0f, 0.0f, 500.0f);
	At(g, 5, 200.0f, 0.0f, 300.0f);
	At(g, 6, 200.0f, 0.0f, 0.0f);
	At(g, 8, 3.0f, 0.0f, 10.0f);
	for (unsigned i = 0; i < 7; ++i)
	{
		float d[3] = { g->pos[(i + 1) * 3] - g->pos[i * 3], 0.0f, g->pos[(i + 1) * 3 + 2] - g->pos[i * 3 + 2] };
		Arc(g, i, i + 1, std::sqrt(d[0] * d[0] + d[2] * d[2]));
	}
	for (unsigned i = 1; i < 8; ++i)
	{
		float d[3] = { g->pos[(i - 1) * 3] - g->pos[i * 3], 0.0f, g->pos[(i - 1) * 3 + 2] - g->pos[i * 3 + 2] };
		Arc(g, i, i - 1, std::sqrt(d[0] * d[0] + d[2] * d[2]));
	}
	Arc(g, 0, 8, 10.4f);
	Arc(g, 8, 0, 10.4f);
}

static float RouteArcSum(const Graph& g, const CoarseRoute& r)
{
	float sum = 0.0f;
	for (int i = 0; i + 1 < r.count; ++i)
	{
		const std::vector<CoarseArc>& list = g.arcs[r.nodes[i]];
		float best = -1.0f;
		for (size_t k = 0; k < list.size(); ++k)
			if (list[k].to == r.nodes[i + 1] && (best < 0.0f || list[k].cost < best)) best = list[k].cost;
		if (best < 0.0f) return -1.0f;
		sum += best;
	}
	return sum;
}

static void CheckRavine(CoarseScratch* s)
{
	Graph g;
	BuildRavine(&g);
	CoarseGraphOps ops = Ops(&g);
	CoarseRoute r;
	CoarseResult res = CoarseSearch(ops, 0, 7, COARSE_SCRATCH_MAX, s, &r);
	unsigned want[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	Check(res == CS_FOUND && RouteIs(r, want, 8), "search: the loop route is found");
	Check(res == CS_FOUND && Near(r.cost, RouteArcSum(g, r)) && r.cost > 1000.0f,
	      "search: the route cost is the sum of its arcs");
	Check(r.expanded >= 7, "search: the loop route expands every loop node");

	// The same scratch serves the next search without a stale entry.
	res = CoarseSearch(ops, 7, 0, COARSE_SCRATCH_MAX, s, &r);
	unsigned back[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };
	Check(res == CS_FOUND && RouteIs(r, back, 8), "search: a reused scratch finds the reverse route");
	res = CoarseSearch(ops, 8, 7, COARSE_SCRATCH_MAX, s, &r);
	unsigned fromDeadEnd[9] = { 8, 0, 1, 2, 3, 4, 5, 6, 7 };
	Check(res == CS_FOUND && RouteIs(r, fromDeadEnd, 9), "search: a reused scratch starts clean");
}

static void CheckDisconnected(CoarseScratch* s)
{
	Graph g;
	Reset(&g, 4);
	At(&g, 0, 0.0f, 0.0f, 0.0f);
	At(&g, 1, 10.0f, 0.0f, 0.0f);
	At(&g, 2, 20.0f, 0.0f, 0.0f);
	At(&g, 3, 30.0f, 0.0f, 0.0f);
	Arc(&g, 0, 1, 10.0f);
	Arc(&g, 1, 0, 10.0f);
	Arc(&g, 2, 3, 10.0f);
	Arc(&g, 3, 2, 10.0f);
	CoarseRoute r;
	CoarseResult res = CoarseSearch(Ops(&g), 0, 3, COARSE_SCRATCH_MAX, s, &r);
	Check(res == CS_NO_ROUTE && r.count == 0, "search: a disconnected pair has no route");
}

// Start 0, goal 9, two middle nodes at mirrored positions with equal arc costs: the lower key wins.
static void BuildDiamond(Graph* g, unsigned left, unsigned right)
{
	Reset(g, 10);
	At(g, 0, 0.0f, 0.0f, 0.0f);
	At(g, 9, 100.0f, 0.0f, 0.0f);
	At(g, left, 50.0f, 0.0f, 50.0f);
	At(g, right, 50.0f, 0.0f, -50.0f);
	float c = std::sqrt(50.0f * 50.0f * 2.0f);
	Arc(g, 0, left, c);
	Arc(g, 0, right, c);
	Arc(g, left, 9, c);
	Arc(g, right, 9, c);
}

static void CheckTies(CoarseScratch* s)
{
	Graph g;
	CoarseRoute r;
	BuildDiamond(&g, 5, 3);
	CoarseResult res = CoarseSearch(Ops(&g), 0, 9, COARSE_SCRATCH_MAX, s, &r);
	unsigned viaThree[3] = { 0, 3, 9 };
	bool first = res == CS_FOUND && RouteIs(r, viaThree, 3);
	BuildDiamond(&g, 3, 5);
	res = CoarseSearch(Ops(&g), 0, 9, COARSE_SCRATCH_MAX, s, &r);
	Check(first && res == CS_FOUND && RouteIs(r, viaThree, 3), "search: equal costs break ties by node key");
}

// Node 1 is closed first through the dear arc 0->1 (f 110); node 2 (f 140) then reaches it for 15
// through an arc shorter than the straight line, so node 1 must be re-opened for the cheap route.
static void CheckReopen(CoarseScratch* s)
{
	Graph g;
	Reset(&g, 4);
	At(&g, 0, 0.0f, 0.0f, 0.0f);
	At(&g, 1, 50.0f, 0.0f, 0.0f);
	At(&g, 2, -30.0f, 0.0f, 0.0f);
	At(&g, 3, 100.0f, 0.0f, 0.0f);
	Arc(&g, 0, 1, 60.0f);
	Arc(&g, 0, 2, 10.0f);
	Arc(&g, 2, 1, 5.0f);
	Arc(&g, 1, 3, 100.0f);
	CoarseRoute r;
	CoarseResult res = CoarseSearch(Ops(&g), 0, 3, COARSE_SCRATCH_MAX, s, &r);
	unsigned want[4] = { 0, 2, 1, 3 };
	Check(res == CS_FOUND && RouteIs(r, want, 4) && Near(r.cost, 115.0f),
	      "search: a node reached again cheaper is re-opened");
}

static void BuildChain(Graph* g, int n)
{
	Reset(g, n);
	for (int i = 0; i < n; ++i) At(g, (unsigned)i, 10.0f * (float)i, 0.0f, 0.0f);
	for (int i = 0; i + 1 < n; ++i)
	{
		Arc(g, (unsigned)i, (unsigned)(i + 1), 10.0f);
		Arc(g, (unsigned)(i + 1), (unsigned)i, 10.0f);
	}
}

static void CheckBudget(CoarseScratch* s)
{
	Graph g;
	BuildChain(&g, 20);
	CoarseRoute r;
	CoarseResult limited = CoarseSearch(Ops(&g), 0, 19, 5, s, &r);
	bool limitedOk = limited == CS_NODE_LIMIT && r.count == 0;
	CoarseResult full = CoarseSearch(Ops(&g), 0, 19, 20, s, &r);
	Check(limitedOk && full == CS_FOUND && r.count == 20, "search: the node budget ends the search");

	CoarseScratch small;
	Check(CoarseScratchInit(&small, 8), "search: a small scratch initialises");
	CoarseResult capped = CoarseSearch(Ops(&g), 0, 19, COARSE_SCRATCH_MAX, &small, &r);
	Check(capped == CS_NODE_LIMIT, "search: the budget is capped at the scratch's size");

	BuildChain(&g, COARSE_ROUTE_MAX + 1);
	CoarseResult longRoute = CoarseSearch(Ops(&g), 0, (unsigned)COARSE_ROUTE_MAX, COARSE_SCRATCH_MAX, s, &r);
	Check(longRoute == CS_ROUTE_TOO_LONG && r.count == 0, "search: a route over the route bound is refused");
	CoarseResult atBound = CoarseSearch(Ops(&g), 0, (unsigned)(COARSE_ROUTE_MAX - 1), COARSE_SCRATCH_MAX, s, &r);
	Check(atBound == CS_FOUND && r.count == COARSE_ROUTE_MAX, "search: a route at the route bound is found");
}

static void CheckEndpoints(CoarseScratch* s)
{
	Graph g;
	BuildChain(&g, 4);
	CoarseRoute r;
	CoarseResult res = CoarseSearch(Ops(&g), 2, 2, COARSE_SCRATCH_MAX, s, &r);
	Check(res == CS_FOUND && r.count == 1 && r.nodes[0] == 2u && r.cost == 0.0f,
	      "search: start equals goal is a one-node route");

	g.hasPos[3] = 0;
	CoarseResult noGoal = CoarseSearch(Ops(&g), 0, 3, COARSE_SCRATCH_MAX, s, &r);
	CoarseResult noStart = CoarseSearch(Ops(&g), 3, 0, COARSE_SCRATCH_MAX, s, &r);
	CoarseResult unknown = CoarseSearch(Ops(&g), 0, 99, COARSE_SCRATCH_MAX, s, &r);
	Check(noGoal == CS_BAD_ENDPOINT && noStart == CS_BAD_ENDPOINT && unknown == CS_BAD_ENDPOINT,
	      "search: an endpoint without a position is refused");
}

// A lake: node 1 is all water; the route through it is 200 long, the one round it by node 2 is 300.
// The arc callback weighs each arc by its two nodes' water bytes at the context's multiplier.
namespace coarse_search_units_detail {

struct WaterGraph
{
	Graph g;
	int   water[4];
	float m;
};

} // namespace coarse_search_units_detail
using namespace coarse_search_units_detail;

static int WaterArcs(void* ctx, unsigned node, CoarseArc* out, int max)
{
	WaterGraph* w = (WaterGraph*)ctx;
	int n = GraphArcs(&w->g, node, out, max);
	for (int i = 0; i < n; ++i)
		out[i].cost = PlanWaterArcCost(out[i].cost, w->m, w->water[node], w->water[out[i].to]);
	return n;
}

static bool WaterPosition(void* ctx, unsigned node, float out[3])
{
	return GraphPosition(&((WaterGraph*)ctx)->g, node, out);
}

static void CheckLake(CoarseScratch* s)
{
	WaterGraph w;
	Reset(&w.g, 4);
	At(&w.g, 0, 0.0f, 0.0f, 0.0f);
	At(&w.g, 1, 100.0f, 0.0f, 0.0f);
	At(&w.g, 2, 100.0f, 0.0f, 111.8f);
	At(&w.g, 3, 200.0f, 0.0f, 0.0f);
	Arc(&w.g, 0, 1, 100.0f);
	Arc(&w.g, 1, 3, 100.0f);
	Arc(&w.g, 0, 2, 150.0f);
	Arc(&w.g, 2, 3, 150.0f);
	w.water[0] = 0;
	w.water[1] = 255;
	w.water[2] = 0;
	w.water[3] = 0;
	CoarseGraphOps ops;
	ops.ctx = &w;
	ops.arcs = WaterArcs;
	ops.position = WaterPosition;
	CoarseRoute* r = new CoarseRoute;
	const unsigned across[3] = { 0, 1, 3 };
	const unsigned round[3] = { 0, 2, 3 };
	w.m = 1.0f;
	CoarseResult r1 = CoarseSearch(ops, 0, 3, COARSE_SCRATCH_MAX, s, r);
	Check(r1 == CS_FOUND && RouteIs(*r, across, 3) && Near(r->cost, 200.0f), "search: at m 1 the route crosses the lake");
	w.m = 5.0f;
	CoarseResult r5 = CoarseSearch(ops, 0, 3, COARSE_SCRATCH_MAX, s, r);
	Check(r5 == CS_FOUND && RouteIs(*r, round, 3) && Near(r->cost, 300.0f), "search: at m 5 the route goes round the lake");
	delete r;
}

int main()
{
	CoarseScratch* s = new CoarseScratch;
	Check(CoarseScratchInit(s, COARSE_SCRATCH_MAX), "search: the scratch initialises at its bound");
	Check(!CoarseScratchInit(s, 0) && CoarseScratchInit(s, COARSE_SCRATCH_MAX),
	      "search: a zero-node scratch is refused");
	CheckRavine(s);
	CheckDisconnected(s);
	CheckTies(s);
	CheckReopen(s);
	CheckBudget(s);
	CheckEndpoints(s);
	CheckLake(s);
	delete s;
	return CheckExit("coarse_search_units");
}
