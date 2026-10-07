// A navmesh patch box across a cell border: the engine's span, the per-cell slice, the route, the
// per-cell action and the deferred decision, single-threaded.
#include <cstdio>
#include <limits>
#include "navmesh/construction/splice_seam_policy.h"
#include "navmesh/cache/nm_force_rebuild_policy.h"

#include "check.h"

using namespace navmesh;

static const SeamGrid kGrid = { -147456.0f, -147456.0f, 4608.0f };

static bool Near(float a, float b)
{
	const float d = a - b;
	return d < 0.0001f && d > -0.0001f;
}

static SeamSpan Span(float cx, float cy, float cz, float hx, float hy, float hz)
{
	const float box[6] = { cx, cy, cz, hx, hy, hz };
	SeamSpan s;
	SeamSpanOf(box, kGrid, &s);
	return s;
}

static bool CellAt(const SeamSpan& s, int i, int x, int z)
{
	return i < s.count && s.x[i] == x && s.z[i] == z;
}

static void CheckSpan()
{
	SeamSpan s = Span(-74000.0f, 1180.0f, -26000.0f, 50.0f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 1 && CellAt(s, 0, 15, 26), "span: a box inside one cell is one cell");

	s = Span(-73700.0f, 1170.0f, -26640.0f, 60.0f, 30.0f, 100.0f);
	Check(!s.overflow && s.count == 2 && CellAt(s, 0, 15, 26) && CellAt(s, 1, 16, 26),
	      "span: a wall box across the border between cells 15 and 16 is two cells");

	s = Span(-74000.0f, 1180.0f, -27648.0f, 50.0f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 2 && CellAt(s, 0, 15, 25) && CellAt(s, 1, 15, 26),
	      "span: a box across a z border is two cells");

	s = Span(-73728.0f, 1170.0f, -27648.0f, 50.0f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 4 && CellAt(s, 0, 15, 25) && CellAt(s, 1, 15, 26)
	      && CellAt(s, 2, 16, 25) && CellAt(s, 3, 16, 26),
	      "span: a corner box is four cells in the engine's order");

	// Max x is exactly -73728: floorf gives 16, and the cell-16 slice has zero extent.
	s = Span(-73778.0f, 1170.0f, -26000.0f, 50.0f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 1 && CellAt(s, 0, 15, 26), "span: a max exactly on the border is one cell");

	s = Span(-73678.0f, 1170.0f, -26000.0f, 50.0f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 1 && CellAt(s, 0, 16, 26), "span: a min exactly on the border is one cell");

	// Max x is -73727.9921875, one float step past the border.
	s = Span(-73778.0f, 1170.0f, -26000.0f, 50.0078125f, 30.0f, 50.0f);
	Check(!s.overflow && s.count == 2 && CellAt(s, 0, 15, 26) && CellAt(s, 1, 16, 26),
	      "span: one float step over the border is two cells");

	s = Span(-147500.0f, 1170.0f, -26000.0f, 10.0f, 30.0f, 10.0f);
	Check(!s.overflow && s.count == 1 && CellAt(s, 0, -1, 26), "span: a box west of the origin floors to cell -1, not 0");

	s = Span(147456.0f, 1170.0f, -26000.0f, 10.0f, 30.0f, 10.0f);
	Check(!s.overflow && s.count == 2 && CellAt(s, 0, 63, 26) && CellAt(s, 1, 64, 26),
	      "span: a box past the east edge lists cell 64");

	s = Span(-74000.0f, 1170.0f, -26000.0f, 4700.0f, 30.0f, 50.0f);
	Check(s.overflow && s.count == 0, "span: a box over two cells on an axis overflows");

	const SeamSpan zeroY = Span(-73700.0f, 1170.0f, -26640.0f, 60.0f, 0.0f, 100.0f);
	const SeamSpan negY = Span(-73700.0f, 1170.0f, -26640.0f, 60.0f, -1.0f, 100.0f);
	Check(!zeroY.overflow && zeroY.count == 0 && !negY.overflow && negY.count == 0,
	      "span: a zero or negative y half is no cell");

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const SeamSpan nanSpan = Span(nan, 1170.0f, -26640.0f, 60.0f, 30.0f, 100.0f);
	const SeamSpan infSpan = Span(-73700.0f, 1170.0f, -26640.0f, 60.0f, 30.0f, inf);
	Check(nanSpan.overflow && nanSpan.count == 0 && infSpan.overflow && infSpan.count == 0,
	      "span: a NaN or infinite coordinate overflows");

	const float box[6] = { -73700.0f, 1170.0f, -26640.0f, 60.0f, 30.0f, 100.0f };
	const SeamGrid noSize = { -147456.0f, -147456.0f, 0.0f };
	SeamSpanOf(box, noSize, &s);
	Check(s.overflow && s.count == 0, "span: no cell size overflows");
}

static void Clip(const float box[6], const float bounds[6], float out[6])
{
	for (int i = 0; i < 6; ++i)
		out[i] = 1e30f;
	SeamClip(box, bounds, out);
}

static bool Is(const float out[6], float cx, float cy, float cz, float hx, float hy, float hz)
{
	return Near(out[0], cx) && Near(out[1], cy) && Near(out[2], cz)
	    && Near(out[3], hx) && Near(out[4], hy) && Near(out[5], hz);
}

static void CheckClip()
{
	// Cell (15, 26)'s bounds as ZoneMap::init sets them: its grid square on x and z, [0, 9800] on y.
	const float wall[6] = { -73700.0f, 1170.0f, -26640.0f, 60.0f, 30.0f, 100.0f };
	const float cell[6] = { -76032.0f, 4900.0f, -25344.0f, 2304.0f, 4900.0f, 2304.0f };
	float out[6];
	Clip(wall, cell, out);
	Check(Is(out, -73744.0f, 1170.0f, -26640.0f, 16.0f, 30.0f, 100.0f),
	      "clip: the cell-15 slice of a box across the border ends on the border");

	const float box[6] = { 0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 10.0f };
	const float bounds[6] = { 5.0f, 6.0f, 7.0f, 10.0f, 10.0f, 10.0f };
	Clip(box, bounds, out);
	Check(Is(out, 2.5f, 3.0f, 3.5f, 7.5f, 7.0f, 6.5f), "clip: all three axes are the overlap of the two boxes");
}

static SeamSpan SpanOfCount(int count, bool overflow)
{
	SeamSpan s;
	s.count = count;
	s.overflow = overflow;
	for (int i = 0; i < SEAM_MAX_CELLS; ++i)
		s.x[i] = s.z[i] = 0;
	return s;
}

static void CheckRoute()
{
	const SeamSpan one = SpanOfCount(1, false), two = SpanOfCount(2, false), over = SpanOfCount(0, true);
	Check(SeamRouteOf(one, true, true) == SEAM_ORIGINAL && SeamRouteOf(one, false, true) == SEAM_ORIGINAL
	      && SeamRouteOf(one, false, false) == SEAM_ORIGINAL,
	      "route: a one-cell box goes to the original");
	Check(SeamRouteOf(over, true, true) == SEAM_ORIGINAL && SeamRouteOf(over, false, true) == SEAM_ORIGINAL,
	      "route: an overflow goes to the original");
	Check(SeamRouteOf(two, true, true) == SEAM_ACT, "route: a multi-cell box on the main thread is acted on");
	Check(SeamRouteOf(two, false, true) == SEAM_RING && SeamRouteOf(two, false, false) == SEAM_RING,
	      "route: off the main thread a multi-cell box goes to the ring");
	Check(SeamRouteOf(two, true, false) == SEAM_ORIGINAL,
	      "route: on the main thread with the world not ok the original keeps it");
}

static void CheckCell()
{
	bool noTerrain = true;
	const int skips[] = { NM_SKIP_NONE, NM_SKIP_NO_ZONE, NM_SKIP_PRIVATE, NM_SKIP_NOT_ACCESSIBLE,
	                      NM_SKIP_NO_CONTENT, NM_SKIP_NO_TERRAIN };
	for (int i = 0; i < (int)(sizeof(skips) / sizeof(skips[0])); ++i)
		noTerrain = noTerrain && SeamCellOf(true, false, skips[i]) == SEAM_CELL_SKIP;
	Check(noTerrain, "cell: no terrain is skipped as the engine skips it");
	Check(SeamCellOf(false, false, NM_SKIP_NO_ZONE) == SEAM_CELL_SKIP && SeamCellOf(false, true, NM_SKIP_NO_ZONE) == SEAM_CELL_SKIP,
	      "cell: no zone is skipped");
	Check(SeamCellOf(true, true, NM_SKIP_NONE) == SEAM_CELL_FORCE, "cell: an eligible cell is forced");
	Check(SeamCellOf(true, true, NM_SKIP_PRIVATE) == SEAM_CELL_PARTIAL, "cell: a cell the mod is loading takes the partial");
	Check(SeamCellOf(true, true, NM_SKIP_NOT_ACCESSIBLE) == SEAM_CELL_PARTIAL, "cell: an inaccessible cell takes the partial");
	Check(SeamCellOf(true, true, NM_SKIP_NO_CONTENT) == SEAM_CELL_PARTIAL, "cell: a cell with no content takes the partial");
}

static SpliceGate Clear()
{
	SpliceGate g;
	g.mainCount = 0;
	g.backCount = 0;
	g.queuesClear = true;
	g.worldOk = true;
	g.cellsReady = true;
	g.cellGone = false;
	return g;
}

static void CheckDeferred()
{
	SpliceGate g = Clear();
	Check(SeamDeferredActs(g), "deferred: all clear acts");
	g = Clear(); g.cellGone = true; g.cellsReady = false;
	Check(SeamDeferredActs(g), "deferred: a gone cell acts");
	g.worldOk = false;
	Check(!SeamDeferredActs(g), "deferred: a gone cell waits while the world is not ok");
	g = Clear(); g.mainCount = 1;
	Check(!SeamDeferredActs(g), "deferred: a main count waits");
	g = Clear(); g.backCount = 1;
	Check(!SeamDeferredActs(g), "deferred: a back count waits");
	g = Clear(); g.queuesClear = false;
	Check(!SeamDeferredActs(g), "deferred: queues not clear wait");
	g = Clear(); g.cellsReady = false;
	Check(!SeamDeferredActs(g), "deferred: a cell not ready waits");
}

int main()
{
	CheckSpan();
	CheckClip();
	CheckRoute();
	CheckCell();
	CheckDeferred();
	return CheckExit("splice_seam_policy_units");
}
