#include "movement/island_span_policy.h"

int IslandCellSpan(int ax, int ay, int bx, int by)
{
	int sx = ax - bx, sy = ay - by;
	if (sx < 0) sx = -sx;
	if (sy < 0) sy = -sy;
	return (sx > sy) ? sx : sy;
}

bool IslandFarSpanFlips(bool vanilla, int labelA, int span, int farSpan)
{
	if (!vanilla || farSpan <= 0) return false;
	if (labelA <= 0 || span < 0) return false;
	return span >= farSpan;
}

int IslandSpanBucket(int span)
{
	if (span < 1) return -1;
	int b = span - 1;
	return (b < ISLAND_SPAN_BUCKETS - 1) ? b : ISLAND_SPAN_BUCKETS - 1;
}

IslandDecision IslandDecide(bool vanilla, bool planned, bool planFlip, bool spanFlip)
{
	IslandDecision d;
	if (planned)
	{
		d.answer = planFlip ? false : vanilla;
		d.final = true;
		d.countFlip = false;
	}
	else if (spanFlip)
	{
		d.answer = false;
		d.final = true;
		d.countFlip = true;
	}
	else
	{
		d.answer = vanilla;
		d.final = false;
		d.countFlip = false;
	}
	return d;
}

IslandEdgePark IslandClassifyEdgePark(bool movingToEdge, bool idle, float haltDist,
                                      float wpDist, float destDist)
{
	if (!movingToEdge || !idle || !(destDist > 100.0f)) return EDGEPARK_NONE;
	if (haltDist < 10.0f) return EDGEPARK_HALTED;
	return (wpDist < 20.0f) ? EDGEPARK_AT_EDGE : EDGEPARK_SHORT_LEG;
}
