// planner_search_ops.cpp - The coarse search's operations over the coarse-graph store: a node's arcs
// priced by the water and the acid under them at the search's prices, its centre, the goal probe's
// inbound-mirror test, the operations builder, and the two store reads they share. Pure over the
// store's read interface and the arc rules: no game header. The caller's thread; the game calls it
// on the main thread only, where the store is read.
#include "planner/planner_search_ops.h"
#include "planner/plan_policy.h"
#include "planner/planner_acid.h"
#include <string.h>

namespace planner {

// ---- The store adapter ---------------------------------------------------------------------------

// A neighbour section's current block, by uid; NULL when it has no index or no readable block.
const CgBlock* NeighbourOf(void* ctx, int uid, int* dirIndexOut)
{
	(void)ctx;
	int dir = CgIndexOfUid(uid);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return NULL;
	*dirIndexOut = dir;
	return v.block;
}

const CgBlock* BlockOfKey(unsigned key, int* idx)
{
	CgView v;
	if (!CgRead(CgNodeDir(key), &v))
		return NULL;
	*idx = CgNodeIndex(key);
	return (*idx >= 0 && *idx < v.block->nodeCount) ? v.block : NULL;
}

// The node's intra arcs, then its resolved cross arcs, the first max of them, each weighted by the
// water under its two nodes at the search's prices (ctx points at a PlanSearchParams; NULL reads 1, 1).
int AdapterArcs(void* ctx, unsigned key, CoarseArc* out, int max)
{
	PlanSearchParams p = { 1.0f, 1.0f };
	if (ctx)
		p = *(const PlanSearchParams*)ctx;
	int idx;
	const CgBlock* b = BlockOfKey(key, &idx);
	if (!b)
		return -1;
	int dir = CgNodeDir(key);
	int acidHere = PlannerAcidCellIs(dir);
	const CgNode& n = b->nodes[idx];
	int count = 0;
	for (int i = 0; i < n.arcCount && count < max; ++i)
	{
		const CgArc& a = b->arcs[n.firstArc + i];
		out[count].to = CgNodeKey(dir, a.to);
		out[count].cost = PlanAcidArcCost(a.cost, p.m, p.a, n.water, b->nodes[a.to].water, acidHere, acidHere);
		++count;
	}
	if (count < max)
	{
		CgResolved res[CG_NODE_ARCS_MAX];
		int r = CgCrossArcs(b, idx, NeighbourOf, NULL, res, max - count);
		for (int i = 0; i < r; ++i)
		{
			out[count].to = CgNodeKey(res[i].dirIndex, res[i].node);
			out[count].cost = PlanAcidArcCost(res[i].cost, p.m, p.a, n.water, res[i].water, acidHere, PlannerAcidCellIs(res[i].dirIndex));
			++count;
		}
	}
	return count;
}

// Whether every arc into the node that its own borders show has its reverse among the node's arcs: no
// list cut at the cap, and each of the node's borders resolved against a readable far block through a
// border that block names (read as a save block, the far block's geometry is withheld, so a border it
// alone resolves reads dropped). Only the node's own borders are read, so two arcs stay unseen: a cross
// arc into the node from a block whose borders name nothing toward it, and a one-way arc inside the
// node's own block.
int AdapterInboundMirrored(void* ctx, unsigned key)
{
	(void)ctx;
	int idx, dir;
	const CgBlock* b = BlockOfKey(key, &idx);
	if (!b || b->nodes[idx].arcCount + b->nodes[idx].borderCount > CG_NODE_ARCS_MAX)
		return 0;
	for (int k = 0; k < b->nodes[idx].borderCount; ++k)
	{
		int bi = b->nodeBorders[b->nodes[idx].firstBorder + k];
		if (bi < 0 || bi >= b->borderCount)
			continue;
		const CgBlock* nb = NeighbourOf(NULL, b->borders[bi].oppUid, &dir);
		if (!nb)
			return 0;
		CgBlock named = *nb;
		named.source = CG_SAVE;
		if (CgClassifyBorder(b, b->borders[bi], &named) == CG_BORDER_DROPPED)
			return 0;
	}
	return 1;
}

bool AdapterPosition(void* ctx, unsigned key, float out[3])
{
	(void)ctx;
	int idx;
	const CgBlock* b = BlockOfKey(key, &idx);
	if (!b)
		return false;
	memcpy(out, b->nodes[idx].centre, sizeof(float) * 3);
	return true;
}

// The coarse search's operations over the store at the prices p (ctx; p outlives the search; NULL
// reads 1, 1): the priced arcs, the node centres and the goal probe's inbound-mirror test.
void AdapterOps(const PlanSearchParams* p, CoarseGraphOps* out)
{
	memset(out, 0, sizeof(*out));
	out->ctx = (void*)p;
	out->arcs = AdapterArcs;
	out->position = AdapterPosition;
	out->inboundMirrored = AdapterInboundMirrored;
}

} // namespace planner
