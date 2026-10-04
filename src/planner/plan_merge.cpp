// plan_merge.cpp - The merge's pure rules: the medoid and the leader, the anchor set, the biased
// operations, the re-cost and the cap, the join index, the gather index and the prefix sample. Pure;
// the caller's thread, no lock.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "planner/plan_merge.h"
#include "planner/plan_policy.h"

namespace planner {

int PlanMergeMedoid(const float (*xz)[2], int n, float* sumOut)
{
	int best = -1;
	double bestSum = 0.0;
	for (int i = 0; xz && i < n; ++i)
	{
		double sum = 0.0;
		for (int j = 0; j < n; ++j)
		{
			double dx = xz[i][0] - xz[j][0], dz = xz[i][1] - xz[j][1];
			sum += std::sqrt(dx * dx + dz * dz);
		}
		if (best < 0 || sum < bestSum)
		{
			best = i;
			bestSum = sum;
		}
	}
	if (sumOut)
		*sumOut = (float)bestSum;
	return best;
}

int PlanMergeLeader(int mode, int medoid)
{
	return (mode == PLANNER_ON && medoid > 0) ? medoid : 0;
}

void PlanMergeSetFrom(const unsigned* routeNodes, int n, PlanMergeSet* set)
{
	if (!routeNodes || n < 0)
		n = 0;
	if (n > COARSE_ROUTE_MAX)
		n = COARSE_ROUTE_MAX;
	if (n > 0)
		memcpy(set->nodes, routeNodes, sizeof(unsigned) * (size_t)n);
	std::sort(set->nodes, set->nodes + n);
	set->count = (int)(std::unique(set->nodes, set->nodes + n) - set->nodes);
}

bool PlanMergeSetHas(const PlanMergeSet& set, unsigned node)
{
	return std::binary_search(set.nodes, set.nodes + set.count, node);
}

static int BiasArcs(void* ctx, unsigned node, CoarseArc* out, int max)
{
	const PlanMergeBias* b = (const PlanMergeBias*)ctx;
	int r = b->inner->arcs(b->inner->ctx, node, out, max);
	for (int i = 0; i < r; ++i)
		if (PlanMergeSetHas(*b->set, out[i].to))
			out[i].cost /= b->k;
	return r;
}

static bool BiasPosition(void* ctx, unsigned node, float out[3])
{
	const PlanMergeBias* b = (const PlanMergeBias*)ctx;
	if (!b->inner->position(b->inner->ctx, node, out))
		return false;
	out[0] /= b->k;
	out[1] /= b->k;
	out[2] /= b->k;
	return true;
}

static int BiasInboundMirrored(void* ctx, unsigned node)
{
	const PlanMergeBias* b = (const PlanMergeBias*)ctx;
	return b->inner->inboundMirrored ? b->inner->inboundMirrored(b->inner->ctx, node) : 0;
}

void PlanMergeBiasOps(PlanMergeBias* bias, CoarseGraphOps* out)
{
	if (!(bias->k >= 1.0f))
		bias->k = 1.0f;
	memset(out, 0, sizeof(*out));
	out->ctx = bias;
	out->arcs = BiasArcs;
	out->position = BiasPosition;
	out->inboundMirrored = bias->inner->inboundMirrored ? BiasInboundMirrored : NULL;
}

float PlanMergeRecost(const CoarseGraphOps& inner, const unsigned* nodes, int n)
{
	CoarseArc arcs[COARSE_ARCS_MAX];
	double total = 0.0;
	for (int i = 0; nodes && i + 1 < n; ++i)
	{
		int r = inner.arcs(inner.ctx, nodes[i], arcs, COARSE_ARCS_MAX);
		float step = -1.0f;
		for (int j = 0; j < r; ++j)
			if (arcs[j].to == nodes[i + 1] && (step < 0.0f || arcs[j].cost < step))
				step = arcs[j].cost;
		if (step < 0.0f)
			return -1.0f;
		total += step;
	}
	return (float)total;
}

bool PlanMergeJoins(float realCost, float ownCost, int detourPercent)
{
	if (!(realCost >= 0.0f) || !(ownCost > 0.0f))
		return false;
	return realCost <= ownCost * (1.0f + (float)detourPercent / 100.0f);
}

int PlanMergeDetourPercent(float realCost, float ownCost)
{
	if (!(ownCost > 0.0f) || !(realCost > ownCost))
		return 0;
	return (int)std::floor((realCost / ownCost - 1.0f) * 100.0f);
}

int PlanMergeJoinIndex(const unsigned* anchor, int anchorCount, const PlanMergeSet& set,
                       const unsigned* member, int memberCount, int* memberStep)
{
	for (int s = 0; member && s < memberCount; ++s)
	{
		if (!PlanMergeSetHas(set, member[s]))
			continue;
		for (int j = 0; j + 1 < anchorCount; ++j)
		{
			if (anchor[j] != member[s])
				continue;
			if (memberStep)
				*memberStep = s;
			return j;
		}
		return -1;
	}
	return -1;
}

int PlanMergeGatherIndex(const int* joins, int n)
{
	int best = 0;
	for (int i = 0; joins && i < n; ++i)
		if (joins[i] > best)
			best = joins[i];
	return best;
}

int PlanMergeSample(const unsigned* nodes, int n, unsigned* out, int max)
{
	if (!nodes || n <= 0 || max <= 0)
		return 0;
	if (n <= max)
	{
		memcpy(out, nodes, sizeof(unsigned) * (size_t)n);
		return n;
	}
	if (max == 1)
	{
		out[0] = nodes[0];
		return 1;
	}
	for (int i = 0; i < max; ++i)
		out[i] = nodes[(int)((long long)i * (n - 1) / (max - 1))];
	return max;
}

} // namespace planner
