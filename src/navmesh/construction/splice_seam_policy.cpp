// splice_seam_policy.cpp - see splice_seam_policy.h. Pure.
#include "navmesh/construction/splice_seam_policy.h"
#include "navmesh/cache/nm_force_rebuild_policy.h"
#include <math.h>
#include <float.h>

namespace navmesh {

void SeamSpanOf(const float box[6], const SeamGrid& g, SeamSpan* out)
{
	out->count = 0;
	out->overflow = false;
	const float minX = box[0] - box[3], maxX = box[0] + box[3];
	const float minZ = box[2] - box[5], maxZ = box[2] + box[5];
	if (!_finite(minX) || !_finite(maxX) || !_finite(minZ) || !_finite(maxZ) || !_finite(box[4])
	 || !(g.cellSize > 0.0f) || !(minX <= maxX) || !(minZ <= maxZ))
	{
		out->overflow = true;
		return;
	}
	if (!(box[4] > 0.0f))
		return;
	const int x0 = (int)floorf((minX - g.originX) / g.cellSize);
	const int x1 = (int)floorf((maxX - g.originX) / g.cellSize);
	const int z0 = (int)floorf((minZ - g.originZ) / g.cellSize);
	const int z1 = (int)floorf((maxZ - g.originZ) / g.cellSize);
	if (x1 - x0 > 1 || z1 - z0 > 1)
	{
		out->overflow = true;
		return;
	}
	for (int x = x0; x <= x1; ++x)
		for (int z = z0; z <= z1; ++z)
		{
			const float cx0 = g.originX + (float)x * g.cellSize, cx1 = cx0 + g.cellSize;
			const float cz0 = g.originZ + (float)z * g.cellSize, cz1 = cz0 + g.cellSize;
			const float lox = minX > cx0 ? minX : cx0, hix = maxX < cx1 ? maxX : cx1;
			const float loz = minZ > cz0 ? minZ : cz0, hiz = maxZ < cz1 ? maxZ : cz1;
			if (!(hix > lox) || !(hiz > loz))
				continue;
			out->x[out->count] = x;
			out->z[out->count] = z;
			++out->count;
		}
}

void SeamClip(const float box[6], const float bounds[6], float out[6])
{
	for (int a = 0; a < 3; ++a)
	{
		const float lo1 = box[a] - box[a + 3], hi1 = box[a] + box[a + 3];
		const float lo2 = bounds[a] - bounds[a + 3], hi2 = bounds[a] + bounds[a + 3];
		const float lo = lo1 > lo2 ? lo1 : lo2, hi = hi1 < hi2 ? hi1 : hi2;
		out[a] = (hi + lo) * 0.5f;
		out[a + 3] = (hi - lo) * 0.5f;
	}
}

SeamRoute SeamRouteOf(const SeamSpan& s, bool mainThread, bool worldOk)
{
	if (s.overflow || s.count <= 1)
		return SEAM_ORIGINAL;
	if (!mainThread)
		return SEAM_RING;
	return worldOk ? SEAM_ACT : SEAM_ORIGINAL;
}

SeamCell SeamCellOf(bool haveZone, bool terrain, int eligibleSkip)
{
	if (!haveZone || !terrain)
		return SEAM_CELL_SKIP;
	return eligibleSkip == NM_SKIP_NONE ? SEAM_CELL_FORCE : SEAM_CELL_PARTIAL;
}

bool SeamDeferredActs(const SpliceGate& g)
{
	if (!g.worldOk)
		return false;
	const SpliceStep s = SpliceDecide(g, 0);
	return s == SQ_ISSUE || s == SQ_DROP;
}

} // namespace navmesh
