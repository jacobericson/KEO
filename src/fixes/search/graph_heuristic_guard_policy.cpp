// graph_heuristic_guard_policy.cpp - Replicates the instance lookups of the hierarchical
// heuristic's three sites and classifies what each would dereference. Pure: any thread, no
// lock, reads only the objects it is handed.
#include "fixes/search/graph_heuristic_guard_policy.h"

namespace graph_heuristic_guard_policy_detail {

// A site's one-slot instance cache, carried locally so a walk over several keys sees the slot
// as the site leaves it: every fresh lookup overwrites the slot and its section.
struct SlotState
{
	const void* collection;
	const void* fallback;        // read in place of a lookup while there is no collection
	const void* cached;
	int         cachedSection;
	bool        hasFallback;     // false: with no collection, a cache miss dereferences it anyway
};

// The heuristic object's layout, fixed by the game: a changed offset fails the build.
typedef char StartClusterAt0x18[OFF_HEUR_START_CLUSTER == 0x18 ? 1 : -1];
typedef char CoarseAt0x180[OFF_HEUR_COARSE == 0x180 ? 1 : -1];
typedef char GoalPointsAt0x240[OFF_HEUR_GOAL_POINTS == 0x240 ? 1 : -1];

} // namespace graph_heuristic_guard_policy_detail
using namespace graph_heuristic_guard_policy_detail;

static void LoadSlot(const unsigned char* vis, GraphHeuristicSite site, SlotState* s)
{
	size_t slot = OFF_HVIS_SLOT_SEED, sec = OFF_HVIS_SEC_SEED;
	s->hasFallback = true;
	if (site == GH_SITE_ADJACENT)
	{
		slot = OFF_HVIS_SLOT_ADJACENT;
		sec  = OFF_HVIS_SEC_ADJACENT;
	}
	else if (site == GH_SITE_CENTRE)
	{
		slot = OFF_HVIS_SLOT_CENTRE;
		sec  = OFF_HVIS_SEC_CENTRE;
		s->hasFallback = false;
	}
	s->collection    = *(const void* const*)(vis + OFF_HVIS_COLLECTION);
	s->fallback      = *(const void* const*)(vis + OFF_HVIS_FALLBACK);
	s->cached        = *(const void* const*)(vis + slot);
	s->cachedSection = *(const int*)(vis + sec);
}

// One key, in the site's own order: the no-collection rule, the cache test, then a lookup the
// site leaves unbounded and this bounds by the collection's instance count.
static GraphHeuristicArm ResolveKey(SlotState* s, unsigned key, GraphHeuristicCall* out)
{
	const unsigned section = key >> PACKED_KEY_SECTION_SHIFT;
	out->key     = key;
	out->section = section;

	GraphHeuristicArm arm;
	if (!s->collection && s->hasFallback)
	{
		arm = s->fallback ? GH_RUN : GH_NO_COLLECTION;
	}
	else if (s->cachedSection == (int)section)
	{
		arm = s->cached ? GH_RUN : GH_NO_INSTANCE;
	}
	else if (!s->collection)
	{
		arm = GH_NO_COLLECTION;
	}
	else
	{
		const unsigned char* coll = (const unsigned char*)s->collection;
		const int count = *(const int*)(coll + OFF_COLL_INSTANCE_SIZE);
		const void* infos = *(const void* const*)(coll + OFF_COLL_INSTANCE_DATA);
		if (count <= 0 || section >= (unsigned)count)
		{
			arm = GH_BAD_SECTION;
		}
		else if (!infos)
		{
			arm = GH_UNJUDGED;
		}
		else
		{
			const unsigned char* info = (const unsigned char*)infos + SIZE_INSTANCE_INFO * (size_t)section;
			s->cached        = *(const void* const*)(info + OFF_INFO_GRAPH);
			s->cachedSection = (int)section;
			arm = s->cached ? GH_RUN : GH_NO_INSTANCE;
		}
	}
	out->arm = arm;
	return arm;
}

void InspectGraphHeuristicKey(const void* visitor, GraphHeuristicSite site, unsigned key, GraphHeuristicCall* out)
{
	out->collection = 0;
	out->key        = key;
	out->section    = key >> PACKED_KEY_SECTION_SHIFT;
	out->arm        = GH_UNJUDGED;
	if (!visitor)
		return;

	SlotState s;
	LoadSlot((const unsigned char*)visitor, site, &s);
	out->collection = s.collection;
	ResolveKey(&s, key, out);
}

void InspectGraphHeuristicSeed(const void* coarseSearch, const void* visitor, const unsigned* goalKeys,
                               unsigned count, unsigned startKey, GraphHeuristicCall* out)
{
	out->collection = 0;
	out->key        = startKey;
	out->section    = startKey >> PACKED_KEY_SECTION_SHIFT;
	out->arm        = GH_UNJUDGED;
	if (!coarseSearch)
		return;

	// The site's own first test, a signed compare, taken before it reads the visitor.
	const unsigned char* coarse = (const unsigned char*)coarseSearch;
	if (*(const int*)(coarse + OFF_COARSE_OPEN_CAP) >= *(const int*)(coarse + OFF_COARSE_OPEN_COUNT))
	{
		out->arm = GH_EARLY_RETURN;
		return;
	}
	if (!visitor)
		return;

	SlotState s;
	LoadSlot((const unsigned char*)visitor, GH_SITE_SEED, &s);
	out->collection = s.collection;
	if (ResolveKey(&s, startKey, out) != GH_RUN)
		return;

	// The site walks the goals only for a positive signed count.
	if ((int)count <= 0)
		return;
	if (!goalKeys)
	{
		out->arm = GH_UNJUDGED;
		return;
	}
	for (unsigned i = 0; i < count; ++i)
	{
		if (goalKeys[i] == 0xFFFFFFFFu)
			continue;
		if (ResolveKey(&s, goalKeys[i], out) != GH_RUN)
			return;
	}
}

bool GraphHeuristicArmFires(GraphHeuristicArm arm)
{
	return arm == GH_BAD_SECTION || arm == GH_NO_COLLECTION || arm == GH_NO_INSTANCE;
}

bool GraphHeuristicSeedWritesStartCluster(unsigned returnRva, bool inExe)
{
	return inExe && returnRva == GH_RET_HEURISTIC_INIT;
}

GraphPositionVec4 GraphHeuristicCentreFallback(const GraphPositionVec4* goal0)
{
	return goal0 ? *goal0 : GraphPositionFarSentinel();
}

int* GraphHeuristicStartCluster(void* heuristic)
{
	return (int*)((unsigned char*)heuristic + OFF_HEUR_START_CLUSTER);
}

void* GraphHeuristicOfSeedCoarse(void* coarseSearch)
{
	return (unsigned char*)coarseSearch - OFF_HEUR_COARSE;
}

const GraphPositionVec4* GraphHeuristicCentreFallbackPoint(const void* heuristic)
{
	return *(const GraphPositionVec4* const*)((const unsigned char*)heuristic + OFF_HEUR_GOAL_POINTS);
}

void GraphHeuristicMakeEuclidean(void* heuristic)
{
	*GraphHeuristicStartCluster(heuristic) = -1;
}
