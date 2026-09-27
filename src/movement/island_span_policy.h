#ifndef KENSHI_ZONE_OPT_ISLAND_SPAN_POLICY_H
#define KENSHI_ZONE_OPT_ISLAND_SPAN_POLICY_H

// The far-span routing rule on ZoneMap::isInIsland, and the classification of
// what an edge-mode character is doing afterwards. Pure arithmetic, so it is
// host-testable and the hook, the census and the stuck line share one
// definition of span.

// Chebyshev distance between two zone-grid cells: the number of cells a
// direct path has to cross in the worse axis.
int IslandCellSpan(int ax, int ay, int bx, int by);

// The rule: a vanilla "same island" answer for a labelled pair whose cells
// are farSpan or more apart is answered false, so setDestination routes to the
// island edge instead of sending one direct path. farSpan <= 0 is the rule
// off; span < 0 (not resolvable) and label 0 (neither cell in an island) never
// flip.
bool IslandFarSpanFlips(bool vanilla, int labelA, int span, int farSpan);

// Flipped-answer span buckets: 1, 2, 3, 4, 5, 6 and wider. Every span the
// rule can flip (farSpan >= 1) has one, so the buckets sum to the flips.
const int ISLAND_SPAN_BUCKETS = 6;
int IslandSpanBucket(int span);   // -1 below 1

// A character idle in edge mode, far from its order's destination, is parked.
// CharMovement::halt leaves edge mode set but copies the position into the
// destination, so a destination on the character is an ended order, not a
// park. Otherwise, at the edge (pathDestination within reach) is the recorded
// edge park; with the leg's end still far away the engine found no new edge
// point to ask for.
enum IslandEdgePark
{
	EDGEPARK_NONE,
	EDGEPARK_AT_EDGE,      // |pathDestination - pos| < 20
	EDGEPARK_SHORT_LEG,    // |pathDestination - pos| >= 20
	EDGEPARK_HALTED        // |destination - pos| < 10: halted with edge mode left set
};
const int ISLAND_EDGE_PARK_KINDS = 4;
IslandEdgePark IslandClassifyEdgePark(bool movingToEdge, bool idle, float haltDist,
                                      float wpDist, float destDist);

#endif // KENSHI_ZONE_OPT_ISLAND_SPAN_POLICY_H
