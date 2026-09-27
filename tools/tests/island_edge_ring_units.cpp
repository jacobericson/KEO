#include <cstdio>
#include "movement/island_edge_ring_policy.h"

#include "check.h"

static int Idx(int gx, int gy) { return gx * ISLAND_RING_GRID_W + gy; }

int main()
{
	// IslandRingKeep: Chebyshev <= r.
	Check(IslandRingKeep(10, 10, 10, 10, 1), "the start cell is always kept");
	Check(IslandRingKeep(10, 10, 10, 10, 0), "radius 0 still keeps the start cell");
	Check(IslandRingKeep(10, 10, 11, 10, 1), "an axis neighbour is kept at r=1");
	Check(IslandRingKeep(10, 10, 11, 11, 1), "a diagonal neighbour is kept at r=1 (Chebyshev, not Euclidean)");
	Check(!IslandRingKeep(10, 10, 11, 10, 0), "r=0 drops an axis neighbour");
	Check(!IslandRingKeep(10, 10, 11, 11, 0), "r=0 drops the diagonal neighbour too");
	Check(!IslandRingKeep(10, 10, 12, 10, 1), "span 2 is dropped at r=1");
	Check(IslandRingKeep(10, 10, 12, 10, 2), "span 2 is kept at r=2");

	// IslandRingCompact: the 8-neighbourhood plus the start, at r=1.
	{
		int sIdx = Idx(10, 10);
		int idx[9] = {
			Idx(10, 10),                 // start itself
			Idx(9, 9), Idx(9, 10), Idx(9, 11),
			Idx(10, 9),            Idx(10, 11),
			Idx(11, 9), Idx(11, 10), Idx(11, 11),
		};
		int keep[9];
		int n = IslandRingCompact(idx, 9, sIdx, 1, keep);
		Check(n == 9, "the start cell and its 8 neighbours all survive r=1");
		for (int i = 0; i < n; ++i)
			Check(keep[i] == i, "every position 0..8 is its own position when nothing is dropped");
	}

	// A far cell is dropped; the kept entries still name the right positions.
	{
		int sIdx = Idx(10, 10);
		int idx[3] = { Idx(10, 10), Idx(12, 10), Idx(10, 11) };
		int keep[3];
		int n = IslandRingCompact(idx, 3, sIdx, 1, keep);
		Check(n == 2, "span-2 cell is dropped, the other two survive");
		Check(keep[0] == 0 && keep[1] == 2, "surviving positions are 0 (start) and 2 (near neighbour)");
	}

	// The all-dropped case: nothing in range.
	{
		int sIdx = Idx(0, 0);
		int idx[2] = { Idx(30, 30), Idx(40, 40) };
		int keep[2];
		int n = IslandRingCompact(idx, 2, sIdx, 1, keep);
		Check(n == 0, "every entry outside the ring compacts to 0");
	}

	// The start cell on the grid's own edge: no negative-index decode trouble.
	{
		int sIdx = Idx(0, 0);
		int idx[4] = { Idx(0, 0), Idx(0, 1), Idx(1, 0), Idx(63, 63) };
		int keep[4];
		int n = IslandRingCompact(idx, 4, sIdx, 1, keep);
		Check(n == 3, "the corner cell keeps itself and its two in-bounds neighbours");
		Check(keep[0] == 0 && keep[1] == 1 && keep[2] == 2, "the far corner (63,63) is dropped, not misdecoded");
	}
	{
		int sIdx = Idx(63, 63);
		int idx[3] = { Idx(63, 63), Idx(63, 62), Idx(0, 0) };
		int keep[3];
		int n = IslandRingCompact(idx, 3, sIdx, 1, keep);
		Check(n == 2, "the opposite corner decodes just as cleanly");
	}

	// Unresolved start / empty input: leave-untouched cases.
	{
		int idx[1] = { Idx(5, 5) };
		int keep[1];
		Check(IslandRingCompact(idx, 1, -1, 1, keep) == 0, "an unresolved start (sIdx < 0) compacts to 0");
		Check(IslandRingCompact(idx, 0, Idx(5, 5), 1, keep) == 0, "an empty input compacts to 0");
	}

	// IslandRingFitsCopyBound: the caller's stack scratch bound.
	Check(IslandRingFitsCopyBound(RING_COPY_MAX), "a list at the bound fits and would be filtered");
	Check(IslandRingFitsCopyBound(0), "an empty list fits");
	Check(!IslandRingFitsCopyBound(RING_COPY_MAX + 1), "a list one past the bound is counted as big");

	// A list at exactly RING_COPY_MAX (256): fits the bound, and the ring
	// still filters it correctly -- a full 3x3 neighbourhood plus far cells
	// filling the rest of the buffer.
	{
		Check(RING_COPY_MAX == 256, "this case assumes the current bound of 256");
		int sIdx = Idx(10, 10);
		int idx[RING_COPY_MAX];
		int full9[9] = {
			Idx(10, 10),
			Idx(9, 9), Idx(9, 10), Idx(9, 11),
			Idx(10, 9),            Idx(10, 11),
			Idx(11, 9), Idx(11, 10), Idx(11, 11),
		};
		for (int i = 0; i < 9; ++i) idx[i] = full9[i];
		for (int i = 9; i < RING_COPY_MAX; ++i) idx[i] = Idx(30, 30);   // far, filtered out

		Check(IslandRingFitsCopyBound(RING_COPY_MAX), "a 256-entry list still fits the bound");
		Check(IslandRingComplete(idx, RING_COPY_MAX, sIdx, 1), "a 256-entry list with a full ring is complete");

		int keep[RING_COPY_MAX];
		int n = IslandRingCompact(idx, RING_COPY_MAX, sIdx, 1, keep);
		Check(n == 9, "a 256-entry list filters down to the 9-cell ring");
	}

	// IslandRingComplete: the ring is only safe to filter when every in-grid
	// neighbour of the start cell is actually in the list.
	{
		int sIdx = Idx(10, 10);
		int full[9] = {
			Idx(10, 10),
			Idx(9, 9), Idx(9, 10), Idx(9, 11),
			Idx(10, 9),            Idx(10, 11),
			Idx(11, 9), Idx(11, 10), Idx(11, 11),
		};
		Check(IslandRingComplete(full, 9, sIdx, 1), "the full 3x3 neighbourhood is complete");

		// The full ring plus a far cell is still complete, and still filters
		// the far cell out -- completeness never depends on what else is in
		// the list.
		int fullPlusFar[10];
		for (int i = 0; i < 9; ++i) fullPlusFar[i] = full[i];
		fullPlusFar[9] = Idx(30, 30);
		Check(IslandRingComplete(fullPlusFar, 10, sIdx, 1), "an extra far cell doesn't break completeness");
		{
			int keep[10];
			int n = IslandRingCompact(fullPlusFar, 10, sIdx, 1, keep);
			Check(n == 9, "the full-ring case still filters the far cell out");
		}

		// The start cell itself missing from the list.
		int noStart[8] = {
			Idx(9, 9), Idx(9, 10), Idx(9, 11),
			Idx(10, 9),            Idx(10, 11),
			Idx(11, 9), Idx(11, 10), Idx(11, 11),
		};
		Check(!IslandRingComplete(noStart, 8, sIdx, 1), "a missing start cell is incomplete");

		// A missing crossing neighbour: a non-convex island whose ray exits
		// through a diagonal corner cell the list doesn't carry.
		int noCorner[8] = {
			Idx(10, 10),
			Idx(9, 9), Idx(9, 10), Idx(9, 11),
			Idx(10, 9),            Idx(10, 11),
			Idx(11, 9), Idx(11, 10),
			// Idx(11, 11) missing
		};
		Check(!IslandRingComplete(noCorner, 8, sIdx, 1), "a missing diagonal corner is incomplete");

		// A missing off-grid "neighbour" is not required: the corner cell's
		// own 3x3 has only 4 in-grid cells (5 of the 9 fall off the grid).
		int cornerSIdx = Idx(0, 0);
		int cornerFull[4] = { Idx(0, 0), Idx(0, 1), Idx(1, 0), Idx(1, 1) };
		Check(IslandRingComplete(cornerFull, 4, cornerSIdx, 1), "off-grid neighbours are never required");
		int cornerMissingDiag[3] = { Idx(0, 0), Idx(0, 1), Idx(1, 0) };
		Check(!IslandRingComplete(cornerMissingDiag, 3, cornerSIdx, 1), "but the in-grid diagonal (1,1) is still required");
	}

	// IslandRingShouldWrite: false computes and counts but never writes; true
	// writes only when something would actually be removed.
	Check(!IslandRingShouldWrite(false, 3), "false computes and counts but never writes");
	Check(!IslandRingShouldWrite(false, 0), "false with an empty result still never writes");
	Check(IslandRingShouldWrite(true, 3), "true writes when entries survive");
	Check(!IslandRingShouldWrite(true, 0), "true with kept=0 still never writes -- the caller leaves the list alone");

	// Leg-length buckets, in cells.
	Check(IslandClassifyLegLen(50.0f, 100.0f) == LEGLEN_LT1, "half a cell buckets LEGLEN_LT1");
	Check(IslandClassifyLegLen(100.0f, 100.0f) == LEGLEN_1TO2, "exactly 1 cell buckets 1-2, not <1");
	Check(IslandClassifyLegLen(150.0f, 100.0f) == LEGLEN_1TO2, "1.5 cells buckets 1-2");
	Check(IslandClassifyLegLen(250.0f, 100.0f) == LEGLEN_2TO3, "2.5 cells buckets 2-3");
	Check(IslandClassifyLegLen(1000.0f, 100.0f) == LEGLEN_3PLUS, "10 cells buckets 3+");
	Check(IslandClassifyLegLen(500.0f, 0.0f) == LEGLEN_3PLUS, "an unresolved cell size never under-reports");

	return CheckExit("island_edge_ring_units");
}
