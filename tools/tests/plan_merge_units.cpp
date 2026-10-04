// The merge's rules over a ladder fixture: rows A (z 0), B (z 200) and C (z -200) of eleven nodes 100
// apart in x, rungs from A to B and from A to C at every column, and the goal G at (1100, 0) linked
// from each row's last node; every arc costs its x-z length, both ways. The anchor A's route is its row.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "planner/plan_merge.h"
#include "planner/coarse_search.h"
#include "planner/plan_policy.h"

#include "check.h"

using namespace planner;

static const unsigned GOAL = 33;

static std::vector<float>                   s_pos;
static std::vector<std::vector<CoarseArc> > s_arcs;
static CoarseScratch*                       s_scratch = NULL;

static unsigned Node(int row, int col)
{
	return (unsigned)(row * 11 + col);
}

static void Place(unsigned node, float x, float z)
{
	s_pos[node * 3u] = x;
	s_pos[node * 3u + 1u] = 0.0f;
	s_pos[node * 3u + 2u] = z;
}

static void Link(unsigned a, unsigned b)
{
	float dx = s_pos[a * 3u] - s_pos[b * 3u], dz = s_pos[a * 3u + 2u] - s_pos[b * 3u + 2u];
	CoarseArc arc;
	arc.cost = std::sqrt(dx * dx + dz * dz);
	arc.to = b;
	s_arcs[a].push_back(arc);
	arc.to = a;
	s_arcs[b].push_back(arc);
}

static void BuildLadder()
{
	s_pos.assign(34u * 3u, 0.0f);
	s_arcs.assign(34u, std::vector<CoarseArc>());
	const float rowZ[3] = { 0.0f, 200.0f, -200.0f };
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 11; ++c)
			Place(Node(r, c), 100.0f * (float)c, rowZ[r]);
	Place(GOAL, 1100.0f, 0.0f);
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c + 1 < 11; ++c)
			Link(Node(r, c), Node(r, c + 1));
		Link(Node(r, 10), GOAL);
	}
	for (int c = 0; c < 11; ++c)
	{
		Link(Node(0, c), Node(1, c));
		Link(Node(0, c), Node(2, c));
	}
}

static int FxArcs(void* ctx, unsigned node, CoarseArc* out, int max)
{
	(void)ctx;
	if (node >= s_arcs.size())
		return -1;
	int n = 0;
	for (size_t i = 0; i < s_arcs[node].size() && n < max; ++i)
		out[n++] = s_arcs[node][i];
	return n;
}

static bool FxPosition(void* ctx, unsigned node, float out[3])
{
	(void)ctx;
	if (node >= s_arcs.size())
		return false;
	out[0] = s_pos[node * 3u];
	out[1] = s_pos[node * 3u + 1u];
	out[2] = s_pos[node * 3u + 2u];
	return true;
}

static CoarseGraphOps FxOps()
{
	CoarseGraphOps ops;
	memset(&ops, 0, sizeof(ops));
	ops.ctx = NULL;
	ops.arcs = FxArcs;
	ops.position = FxPosition;
	return ops;
}

static bool Route(const CoarseGraphOps& ops, unsigned start, CoarseRoute* out)
{
	return CoarseSearch(ops, start, GOAL, 1024, s_scratch, out) == CS_FOUND;
}

static void CheckMedoid()
{
	const float three[3][2] = { { 0.0f, 0.0f }, { 0.0f, 200.0f }, { 0.0f, -200.0f } };
	float sum = 0.0f;
	CHECK(PlanMergeMedoid(three, 3, &sum) == 0 && std::fabs(sum - 400.0f) < 1e-3f,
	      "merge: the medoid is the member with the least summed distance");
	const float pair[2][2] = { { 0.0f, 0.0f }, { 100.0f, 0.0f } };
	CHECK(PlanMergeMedoid(pair, 2, NULL) == 0, "merge: a tie goes to the lower selection index");
	const float line[3][2] = { { 0.0f, 0.0f }, { 10.0f, 0.0f }, { 20.0f, 0.0f } };
	CHECK(PlanMergeMedoid(line, 3, NULL) == 1 && PlanMergeMedoid(line, 0, NULL) == -1,
	      "merge: the middle of a line is its medoid, and no member has none");
	CHECK(PlanMergeLeader(PLANNER_ON, 2) == 2 && PlanMergeLeader(PLANNER_ON, -1) == 0, "merge: on leads with the medoid");
	CHECK(PlanMergeLeader(PLANNER_OBSERVE, 2) == 0 && PlanMergeLeader(PLANNER_OFF, 2) == 0,
	      "merge: observe and off lead with the first member");
}

static void CheckBias()
{
	BuildLadder();
	CoarseGraphOps inner = FxOps();
	CoarseRoute anchor, member, own;
	CHECK(Route(inner, Node(0, 0), &anchor) && anchor.count == 12 && anchor.nodes[11] == GOAL,
	      "merge fixture: the anchor walks its own row to the goal");
	PlanMergeSet set;
	PlanMergeSetFrom(anchor.nodes, anchor.count, &set);
	CHECK(set.count == 12 && PlanMergeSetHas(set, Node(0, 5)) && !PlanMergeSetHas(set, Node(1, 5)),
	      "merge: the set holds the anchor route's nodes");

	PlanMergeBias bias = { &inner, &set, 3.0f };
	CoarseGraphOps biased;
	PlanMergeBiasOps(&bias, &biased);
	float p[3] = { 0.0f, 0.0f, 0.0f };
	CHECK(biased.position(biased.ctx, Node(0, 3), p) && std::fabs(p[0] - 100.0f) < 1e-3f,
	      "merge: the biased heuristic is the straight line over k");

	// B at (0, 200): its own optimum is its row, 1000 + sqrt(100^2 + 200^2); under the bias it crosses to
	// A at once, 1300 at full price, a 6 % detour.
	CHECK(Route(inner, Node(1, 0), &own) && std::fabs(own.cost - 1223.607f) < 0.01f,
	      "merge fixture: B's own optimum is its own row");
	bool found = Route(biased, Node(1, 0), &member);
	int step = -1;
	int join = found ? PlanMergeJoinIndex(anchor.nodes, anchor.count, set, member.nodes, member.count, &step) : -2;
	float real = PlanMergeRecost(inner, member.nodes, member.count);
	CHECK(found && join == 0 && step == 1 && std::fabs(real - 1300.0f) < 0.01f,
	      "merge: under the bias the parallel member joins the anchor's route");
	CHECK(PlanMergeJoins(real, own.cost, 15) && PlanMergeDetourPercent(real, own.cost) == 6,
	      "merge: a 6 % detour joins under a 15 % cap");
	CHECK(!PlanMergeJoins(real, own.cost, 5), "merge: a route over the cap goes alone");
	const int joinB = join;

	// C at (0, -200), the other parallel row, mirrors B: under the bias it crosses to A at once, 1300.
	found = Route(biased, Node(2, 0), &member);
	step = -1;
	int joinC = found ? PlanMergeJoinIndex(anchor.nodes, anchor.count, set, member.nodes, member.count, &step) : -2;
	real = PlanMergeRecost(inner, member.nodes, member.count);
	CHECK(found && joinC == 0 && step == 1 && std::fabs(real - 1300.0f) < 0.01f,
	      "merge: under the bias the second parallel member joins the anchor's route too");

	// At k = 1 B and C keep their rows and meet the anchor's route only at the goal.
	PlanMergeBias none = { &inner, &set, 1.0f };
	CoarseGraphOps unbiased;
	PlanMergeBiasOps(&none, &unbiased);
	found = Route(unbiased, Node(1, 0), &member);
	CHECK(found && PlanMergeJoinIndex(anchor.nodes, anchor.count, set, member.nodes, member.count, &step) == -1,
	      "merge: sharing only the goal is no join");
	found = Route(unbiased, Node(2, 0), &member);
	CHECK(found && PlanMergeJoinIndex(anchor.nodes, anchor.count, set, member.nodes, member.count, &step) == -1,
	      "merge: at k = 1 the parallel routes separate");

	// D, a straggler at (500, 200), is past the others: it crosses to A's sixth node, 800 against 723.6.
	found = Route(biased, Node(1, 5), &member);
	Route(inner, Node(1, 5), &own);
	join = found ? PlanMergeJoinIndex(anchor.nodes, anchor.count, set, member.nodes, member.count, &step) : -2;
	real = PlanMergeRecost(inner, member.nodes, member.count);
	CHECK(join == 5 && PlanMergeJoins(real, own.cost, 15), "merge: a straggler past the others joins further along");

	// The gather index from the four searches' own joins: the anchor (0), B, C and the straggler.
	const int joins[4] = { 0, joinB, joinC, join };
	const int start[3] = { 0, -1, -1 };
	CHECK(PlanMergeGatherIndex(joins, 4) == 5, "merge: the group gathers at its most advanced join");
	CHECK(PlanMergeGatherIndex(start, 3) == 0 && PlanMergeGatherIndex(start, 0) == 0,
	      "merge: with no join past the start the gather index is 0");
	const unsigned broken[2] = { Node(1, 0), Node(2, 0) };
	CHECK(PlanMergeRecost(inner, broken, 2) == -1.0f && PlanMergeRecost(inner, broken, 1) == 0.0f,
	      "merge: a step without an arc re-costs as -1, and one node costs nothing");
}

static void CheckSample()
{
	unsigned nodes[100];
	for (unsigned i = 0; i < 100; ++i)
		nodes[i] = i * 7u;
	unsigned out[64];
	int n = PlanMergeSample(nodes, 100, out, 64);
	CHECK(n == 64 && out[0] == 0u && out[63] == 99u * 7u, "merge: a long prefix is sampled with its ends kept");
	n = PlanMergeSample(nodes, 10, out, 64);
	CHECK(n == 10 && out[9] == 63u, "merge: a short prefix is kept whole");
	CHECK(PlanMergeSample(nodes, 0, out, 64) == 0 && PlanMergeSample(nodes, 5, out, 1) == 1 && out[0] == 0u,
	      "merge: an empty prefix samples nothing, and one slot keeps the first node");
}

int main()
{
	s_scratch = new CoarseScratch;
	CHECK(CoarseScratchInit(s_scratch, 1024), "merge fixture: the scratch initialises");
	CheckMedoid();
	CheckBias();
	CheckSample();
	return CheckExit("plan_merge_units");
}
