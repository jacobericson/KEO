#include <cstdio>
#include "movement/island_span_policy.h"

#include "check.h"

int main()
{
	// Span is the Chebyshev distance, symmetric, 0 for one cell.
	Check(IslandCellSpan(46, 33, 46, 33) == 0, "same cell spans 0");
	Check(IslandCellSpan(46, 33, 50, 33) == 4, "(46,33)->(50,33) spans 4");
	Check(IslandCellSpan(50, 33, 46, 33) == 4, "span is symmetric");
	Check(IslandCellSpan(21, 42, 23, 43) == 2, "the worse axis decides");
	Check(IslandCellSpan(10, 10, 11, 11) == 1, "a diagonal neighbour spans 1");

	// The rule at the shipped threshold 2.
	Check(!IslandFarSpanFlips(true, 1, 1, 2), "span 1 keeps the vanilla answer");
	Check(IslandFarSpanFlips(true, 1, 2, 2), "span 2 flips");
	Check(IslandFarSpanFlips(true, 1, 4, 2), "span 4 flips");
	Check(!IslandFarSpanFlips(false, 1, 4, 2), "a vanilla false is never touched");
	Check(!IslandFarSpanFlips(true, 0, 4, 2), "an unlabelled pair is never flipped");
	Check(!IslandFarSpanFlips(true, 1, -1, 2), "an unresolved span is never flipped");
	Check(!IslandFarSpanFlips(true, 1, 6, 0), "farSpan 0 is the rule off");
	Check(!IslandFarSpanFlips(true, 1, 6, -3), "a negative farSpan is the rule off");
	Check(!IslandFarSpanFlips(true, 1, 2, 3), "a higher threshold spares span 2");
	Check(IslandFarSpanFlips(true, 7, 3, 3), "any positive label qualifies");

	// Buckets: 1..5 one each, 6 and wider share the last; every flippable
	// span has one.
	Check(IslandSpanBucket(0) == -1, "span 0 has no bucket");
	Check(IslandSpanBucket(1) == 0, "span 1 -> bucket 0");
	Check(IslandSpanBucket(2) == 1, "span 2 -> bucket 1");
	Check(IslandSpanBucket(5) == 4, "span 5 -> bucket 4");
	Check(IslandSpanBucket(6) == 5, "span 6 -> last bucket");
	Check(IslandSpanBucket(40) == 5, "span 40 -> last bucket");
	for (int n = 1; n <= 8; ++n)
		for (int sp = 0; sp <= 12; ++sp)
			if (IslandFarSpanFlips(true, 1, sp, n))
				Check(IslandSpanBucket(sp) >= 0 && IslandSpanBucket(sp) < ISLAND_SPAN_BUCKETS,
				      "every flipped span has a bucket");

	// Edge parks. haltDist = |destination - pos|; an order still running keeps
	// its destination far away.
	Check(IslandClassifyEdgePark(false, true, 5000.0f, 5.0f, 5000.0f) == EDGEPARK_NONE, "not in edge mode is no edge park");
	Check(IslandClassifyEdgePark(true, false, 5000.0f, 5.0f, 5000.0f) == EDGEPARK_NONE, "a moving character is not parked");
	Check(IslandClassifyEdgePark(true, true, 5000.0f, 5.0f, 50.0f) == EDGEPARK_NONE, "at the destination is not a park");
	Check(IslandClassifyEdgePark(true, true, 5000.0f, 5.0f, 5000.0f) == EDGEPARK_AT_EDGE, "idle at the leg end is the edge park");
	Check(IslandClassifyEdgePark(true, true, 5000.0f, 3000.0f, 5000.0f) == EDGEPARK_SHORT_LEG, "idle short of the leg end");
	Check(IslandClassifyEdgePark(true, true, 5000.0f, 20.0f, 5000.0f) == EDGEPARK_SHORT_LEG, "20 units is outside the edge radius");
	Check(IslandClassifyEdgePark(true, true, 0.0f, 0.0f, 5000.0f) == EDGEPARK_HALTED, "halt: destination and pathDestination on pos");
	Check(IslandClassifyEdgePark(true, true, 2.0f, 3000.0f, 5000.0f) == EDGEPARK_HALTED, "halt wins over the leg-end test");
	Check(IslandClassifyEdgePark(true, true, 10.0f, 5.0f, 5000.0f) == EDGEPARK_AT_EDGE, "10 units from the destination is not a halt");
	Check(IslandClassifyEdgePark(false, true, 0.0f, 0.0f, 5000.0f) == EDGEPARK_NONE, "a halt out of edge mode is not an edge park");

	return CheckExit("island_span_units");
}
