// coarse_graph_cross.cpp - The store's read-time cross resolution: a node's borders resolved
// against the neighbouring sections' blocks into cross arcs, each border counted by outcome. Main
// thread, during a search; it reads only published blocks, which stay valid until the next retire
// drain, takes no lock and adds its counts with interlocked adds.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/coarse_graph.h"

#include <math.h>
#include <string.h>

namespace planner {

static void Midpoint(const float* a, const float* b, float* out)
{
	for (int k = 0; k < 3; ++k)
		out[k] = (a[k] + b[k]) * 0.5f;
}

// The first border of nb with (oppUid, face) == (uid, face), or -1.
static int FindBorder(const CgBlock* nb, int uid, int face)
{
	int lo = 0, hi = nb->borderCount;
	while (lo < hi)
	{
		int mid = lo + (hi - lo) / 2;
		const CgBorder& m = nb->borders[mid];
		if (m.oppUid < uid || (m.oppUid == uid && m.face < face))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < nb->borderCount && nb->borders[lo].oppUid == uid && nb->borders[lo].face == face)
		return lo;
	return -1;
}

static float Dist2(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// The first border of nb toward uid (borders sort by oppUid), or nb->borderCount.
static int RunStart(const CgBlock* nb, int uid)
{
	int lo = 0, hi = nb->borderCount;
	while (lo < hi)
	{
		int mid = lo + (hi - lo) / 2;
		if (nb->borders[mid].oppUid < uid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool TargetOk(const CgBlock* nb, int node)
{
	return node >= 0 && node < nb->nodeCount;
}

// A far border carrying both of br's faces, swapped, with a target node; -1 when none.
static int FindMirrored(const CgBlock* a, const CgBorder& br, const CgBlock* nb)
{
	int i = FindBorder(nb, a->uid, br.oppFace);
	if (i < 0)
		return -1;
	for (; i < nb->borderCount && nb->borders[i].oppUid == a->uid && nb->borders[i].face == br.oppFace; ++i)
		if (nb->borders[i].oppFace == br.face && TargetOk(nb, nb->borders[i].from))
			return i;
	return -1;
}

// Among nb's borders toward a, one naming br's connection by either face, with a target node and
// its portal within CG_PORTAL_SLACK of br's; the nearest portal wins; -1 when none.
static int FindNamedAtPortal(const CgBlock* a, const CgBorder& br, const CgBlock* nb)
{
	const float slack2 = CG_PORTAL_SLACK * CG_PORTAL_SLACK;
	int best = -1;
	float bestD = 0.0f;
	for (int i = RunStart(nb, a->uid); i < nb->borderCount && nb->borders[i].oppUid == a->uid; ++i)
	{
		const CgBorder& f = nb->borders[i];
		if ((f.face != br.oppFace && f.oppFace != br.face) || !TargetOk(nb, f.from))
			continue;
		float d = Dist2(f.portal, br.portal);
		if (!(d <= slack2))
			continue;
		if (best < 0 || d < bestD)
		{
			best = i;
			bestD = d;
		}
	}
	return best;
}

// The node of nb whose box, widened by CG_PORTAL_SLACK on every axis, holds p and whose centre is
// nearest p; -1 when none.
static int PickByPortal(const CgBlock* nb, const float* p)
{
	int best = -1;
	float bestD = 0.0f;
	for (int n = 0; n < nb->nodeCount; ++n)
	{
		const CgNode& node = nb->nodes[n];
		bool inside = true;
		for (int ax = 0; ax < 3; ++ax)
			if (!(p[ax] >= node.boxMin[ax] - CG_PORTAL_SLACK && p[ax] <= node.boxMax[ax] + CG_PORTAL_SLACK))
				inside = false;
		float d = Dist2(node.centre, p);
		if (!inside || !(d >= 0.0f))
			continue;
		if (best < 0 || d < bestD)
		{
			best = n;
			bestD = d;
		}
	}
	return best;
}

// The rule's steps 2-5 for border br of a against its far block nb: the class and, unless the
// border is dropped, the target node.
static int Resolve(const CgBlock* a, const CgBorder& br, const CgBlock* nb, int* target)
{
	int m = FindMirrored(a, br, nb);
	if (m >= 0)
	{
		*target = nb->borders[m].from;
		return CG_BORDER_MIRRORED;
	}
	int f = FindNamedAtPortal(a, br, nb);
	if (f >= 0)
	{
		*target = nb->borders[f].from;
		return CG_BORDER_ONE_SIDED;
	}
	if (nb->source == CG_LIVE || nb->source == CG_BASE)
	{
		int n = PickByPortal(nb, br.portal);
		if (n >= 0)
		{
			*target = n;
			return CG_BORDER_ONE_SIDED;
		}
	}
	*target = -1;
	return CG_BORDER_DROPPED;
}

int CgClassifyBorder(const CgBlock* a, const CgBorder& br, const CgBlock* nb)
{
	int target = -1;
	return (a && nb) ? Resolve(a, br, nb, &target) : CG_BORDER_DROPPED;
}

int CgCrossArcs(const CgBlock* a, int node, CgNeighbourFn neighbourOf, void* ctx, CgResolved* out, int max)
{
	if (!a || !neighbourOf || node < 0 || node >= a->nodeCount || max <= 0)
		return 0;
	const CgNode& from = a->nodes[node];
	float best[CG_NODE_ARCS_MAX];               // each written arc's longest edge, squared
	int written = 0;
	long noBlock = 0, dropped = 0, oneSided = 0;
	for (int k = 0; k < from.borderCount; ++k)
	{
		int bi = a->nodeBorders[from.firstBorder + k];
		if (bi < 0 || bi >= a->borderCount)
			continue;
		const CgBorder& br = a->borders[bi];
		int dir = -1;
		const CgBlock* nb = neighbourOf(ctx, br.oppUid, &dir);
		if (!nb)
		{
			++noBlock;
			continue;
		}
		int target = -1;
		int cls = Resolve(a, br, nb, &target);
		if (cls == CG_BORDER_DROPPED)
		{
			++dropped;
			continue;
		}
		if (cls == CG_BORDER_ONE_SIDED)
			++oneSided;
		float len = Dist2(br.a, br.b);
		int j = 0;
		while (j < written && !(out[j].dirIndex == dir && out[j].node == target))
			++j;
		if (j == written)
		{
			if (written >= max || written >= CG_NODE_ARCS_MAX)
				continue;
			out[j].dirIndex = dir;
			out[j].node = target;
			out[j].cost = sqrtf(Dist2(from.centre, nb->nodes[target].centre));
			out[j].water = nb->nodes[target].water;
			best[j] = -1.0f;
			++written;
		}
		if (len > best[j])
		{
			best[j] = len;
			Midpoint(br.a, br.b, out[j].portal);
			memcpy(out[j].edgeA, br.a, sizeof(out[j].edgeA));
			memcpy(out[j].edgeB, br.b, sizeof(out[j].edgeB));
		}
	}
	if (noBlock || dropped || oneSided)
		CgStatsAddCross(noBlock, dropped, oneSided);
	return written;
}

} // namespace planner
