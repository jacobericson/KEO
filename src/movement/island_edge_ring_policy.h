#ifndef KEO_ISLAND_EDGE_RING_POLICY_H
#define KEO_ISLAND_EDGE_RING_POLICY_H

// NavMesh::getZoneEdge rays from the destination toward the character and
// keeps the first island face the ray crosses. When the character's own cell
// is far inside a large island, that face sits on the destination cell's own
// near boundary, so the leg the engine walks is as long as the whole island
// and exhausts the search's node cap. Keeping only the island cells within
// Chebyshev radius r of the character's own cell bounds the ray's reach to
// r+1 cells: a ray from the destination to any point outside the ring can
// only enter the ring through one of its own cells -- but only when every
// in-grid cell of the ring is actually present in the list (IslandRingComplete
// below); an incomplete ring can leave the ray no listed face to cross and
// reproduce the very park a radius of 0 would cause.
//
// Pure: no game pointer, no Windows header. The caller (island_edge_ring.cpp)
// resolves each ZoneMap* to its zone-grid index through its own
// ZoneIndexOf and hands the indices here.

// Zone-grid width: idx = gx * ISLAND_RING_GRID_W + gy, matching game.h's
// ZONE_GRID_COUNT (64*64) and islands_inline.h's IdxGX/IdxGY.
const int ISLAND_RING_GRID_W = 64;

// Upper bound on the island list the caller copies onto its own stack before
// filtering it (island_edge_ring.cpp's idxBuf/posBuf). A list past this bound
// is left untouched rather than truncated -- the caller counts it as "big"
// instead. Vanilla's list is every Set B zone carrying the start cell's
// label, so a large contiguous active region (several adjacent squads' worth
// of loaded cells) can exceed a small bound; 256 covers that with two
// int[256] arrays, about 2 KB of stack.
const int RING_COPY_MAX = 256;

// Whether a list of n entries fits within RING_COPY_MAX. false means the
// caller must skip filtering entirely and count the call as "big".
bool IslandRingFitsCopyBound(int n);

// Chebyshev distance between the start cell (sgx,sgy) and a candidate
// (gx,gy), at most r.
bool IslandRingKeep(int sgx, int sgy, int gx, int gy, int r);

// True iff idx[0..n) contains every cell within Chebyshev r of sIdx that
// falls inside the 0..ISLAND_RING_GRID_W-1 grid (a cell off the grid edge is
// not required). The caller filters only when this holds: with the whole
// neighbourhood present, getZoneEdge's ray always has a listed face to
// cross on its way out of the start cell, wherever the target sits; with even
// one neighbour missing the ray can be left with nothing but the start
// cell's own near face, landing the point back inside it.
bool IslandRingComplete(const int* idx, int n, int sIdx, int r);

// Compacts idx[0..n) against the ring around sIdx. keepOut receives, in
// increasing order, the ORIGINAL ARRAY POSITIONS (not the zone indices) of
// the entries kept, so a caller can permute any parallel array (here, the
// lektor's ZoneMap* pointers) by copying data[keepOut[j]] into data[j] for
// j in [0, returned count) -- always a forward, in-place-safe copy, since
// keepOut[j] >= j.
//
// Returns the kept count. 0 means either nothing survives the ring (every
// entry lies outside it) or the call could not run at all (n <= 0, or sIdx
// unresolved, i.e. < 0): the caller's contract in both cases is to leave the
// original list standing rather than publish an empty one.
int IslandRingCompact(const int* idx, int n, int sIdx, int r, int* keepOut);

// Whether the caller should write the compacted list back, without depending
// on config.h: false computes filt/rm and counts them but never writes (the
// run that validates the numbers before the list itself changes); true writes
// whenever something would be removed. kept == 0 (would drop everything) is
// never written either way -- that is the caller's "leave it untouched"
// branch, checked before this is asked.
bool IslandRingShouldWrite(bool ring, int kept);

// How long an edge leg turned out to be, in cells: the ring's whole point is
// to bound this to ~2. island_edge_legs.cpp buckets every leg's start
// distance (|pathDestination - pos| / cellSize) through this so IslandSpan:
// can show whether the ring actually shortened them.
enum IslandLegLenBucket { LEGLEN_LT1 = 0, LEGLEN_1TO2, LEGLEN_2TO3, LEGLEN_3PLUS };
const int ISLAND_LEGLEN_BUCKETS = 4;
IslandLegLenBucket IslandClassifyLegLen(float distance, float cellSize);

#endif // KEO_ISLAND_EDGE_RING_POLICY_H
