#include "movement/island_edge_ring_policy.h"

namespace island_edge_ring_policy_detail {

// idx = gx * ISLAND_RING_GRID_W + gy (islands_inline.h's IdxGX/IdxGY). Returns
// false for an out-of-range index instead of decoding garbage.
bool Decode(int idx, int* gx, int* gy)
{
	if (idx < 0) return false;
	*gx = idx / ISLAND_RING_GRID_W;
	*gy = idx % ISLAND_RING_GRID_W;
	return true;
}

} // namespace
using namespace island_edge_ring_policy_detail;

bool IslandRingFitsCopyBound(int n)
{
	return n >= 0 && n <= RING_COPY_MAX;
}

bool IslandRingKeep(int sgx, int sgy, int gx, int gy, int r)
{
	int dx = sgx - gx; if (dx < 0) dx = -dx;
	int dy = sgy - gy; if (dy < 0) dy = -dy;
	return (dx > dy ? dx : dy) <= r;
}

namespace island_edge_ring_policy_detail {

bool Contains(const int* idx, int n, int want)
{
	for (int i = 0; i < n; ++i)
		if (idx[i] == want) return true;
	return false;
}

} // namespace
using namespace island_edge_ring_policy_detail;

bool IslandRingComplete(const int* idx, int n, int sIdx, int r)
{
	int sgx, sgy;
	if (n <= 0 || !Decode(sIdx, &sgx, &sgy))
		return false;

	for (int dx = -r; dx <= r; ++dx)
	{
		for (int dy = -r; dy <= r; ++dy)
		{
			int gx = sgx + dx, gy = sgy + dy;
			if (gx < 0 || gx >= ISLAND_RING_GRID_W || gy < 0 || gy >= ISLAND_RING_GRID_W)
				continue;   // off the grid edge: not required
			if (!Contains(idx, n, gx * ISLAND_RING_GRID_W + gy))
				return false;
		}
	}
	return true;
}

int IslandRingCompact(const int* idx, int n, int sIdx, int r, int* keepOut)
{
	int sgx, sgy;
	if (n <= 0 || !Decode(sIdx, &sgx, &sgy))
		return 0;

	int kept = 0;
	for (int i = 0; i < n; ++i)
	{
		int gx, gy;
		if (!Decode(idx[i], &gx, &gy)) continue;
		if (IslandRingKeep(sgx, sgy, gx, gy, r))
			keepOut[kept++] = i;
	}
	return kept;
}

bool IslandRingShouldWrite(bool ring, int kept)
{
	return ring && kept > 0;
}

IslandLegLenBucket IslandClassifyLegLen(float distance, float cellSize)
{
	if (!(cellSize > 0.0f)) return LEGLEN_3PLUS;   // unresolved cell size: don't under-report
	float cells = distance / cellSize;
	if (cells < 1.0f) return LEGLEN_LT1;
	if (cells < 2.0f) return LEGLEN_1TO2;
	if (cells < 3.0f) return LEGLEN_2TO3;
	return LEGLEN_3PLUS;
}
