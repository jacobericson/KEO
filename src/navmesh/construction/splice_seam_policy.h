// splice_seam_policy.h - A navmesh patch box across a cell border: the cells the engine's
// NavMesh::generate(Aabb) gives it, the slice a cell's own patch takes, which path a call takes,
// what each cell gets, and when a box held for the main thread is acted on. Pure: no Windows,
// KenshiLib or game header; any thread.
#ifndef KEO_SPLICE_SEAM_POLICY_H
#define KEO_SPLICE_SEAM_POLICY_H

#include "navmesh/construction/splice_queue_policy.h"

namespace navmesh {

const int SEAM_MAX_CELLS = 4;   // two per axis: a box no wider than a cell

// The engine's cell grid, from the NavMesh: worldX, worldY (on z) and cellSize.
struct SeamGrid { float originX, originZ, cellSize; };

// The cells the box splices, in the engine's order (x outer, z inner). count is 0 when no cell
// takes a patch; overflow when the original is to keep the box whatever the count says.
struct SeamSpan
{
	int  count;
	bool overflow;
	int  x[SEAM_MAX_CELLS];
	int  z[SEAM_MAX_CELLS];
};
// Boxes are Ogre::Aabb's layout: centre x, y, z, then half size x, y, z. The engine's span:
// min = centre - half and max = centre + half, then floorf((min - origin) / cellSize) and the
// same for max, on x and on z. A cell counts when the box's overlap with it has positive extent
// on x and on z; none counts when the y half is not positive (addJob refuses either). overflow
// for a non-finite coordinate, no cell size, a max below its min, or more than two cells on an
// axis.
void SeamSpanOf(const float box[6], const SeamGrid& g, SeamSpan* out);

// The part of box inside bounds, on all three axes: the overlap's centre and half size.
void SeamClip(const float box[6], const float bounds[6], float out[6]);

enum SeamRoute { SEAM_ORIGINAL = 0, SEAM_ACT, SEAM_RING };
// One cell or none, or an overflow: the original. More than one cell: acted on in place on the
// main thread while the world is ok (the original otherwise), ringed on any other thread.
SeamRoute SeamRouteOf(const SeamSpan& s, bool mainThread, bool worldOk);

enum SeamCell { SEAM_CELL_SKIP = 0, SEAM_CELL_FORCE, SEAM_CELL_PARTIAL };
// No zone or no terrain sector: skipped, as the engine skips it. Eligible for a forced full
// regeneration (eligibleSkip is NmRebuildEligible's NONE): forced. Present but ineligible: the
// engine's own clipped partial.
SeamCell SeamCellOf(bool haveZone, bool terrain, int eligibleSkip);

// A box held for the main thread: never acted on while the world is not ok; otherwise when the
// wall splice's gate would issue or drop it (a gone cell is the action's to skip or patch).
bool SeamDeferredActs(const SpliceGate& g);

} // namespace navmesh

#endif
