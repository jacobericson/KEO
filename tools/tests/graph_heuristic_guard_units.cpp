// The hierarchical heuristic guard's classification, over a fabricated visitor, coarse search
// and collection at the real offsets: each site's slot and section field, the no-collection
// rules, the section bound, the seed's early return and goal walk, the return-address rule and
// the centre fallback.

#include <cstdio>
#include <cstring>
#include "fixes/search/graph_heuristic_guard_policy.h"

#include "check.h"

struct World
{
	unsigned char visitor[0x80];
	unsigned char coarse[0x40];
	unsigned char collection[64];
	unsigned char infos[SIZE_INSTANCE_INFO * 4];
	unsigned char graphA[64];
	unsigned char graphB[64];
};

// Four sections, 1 and 2 holding graph instances, nothing cached in any slot, and a coarse
// search with room in its open set.
static void Init(World* w)
{
	memset(w, 0, sizeof(*w));
	*(void**)(w->visitor + OFF_HVIS_COLLECTION) = w->collection;
	*(int*)(w->visitor + OFF_HVIS_SEC_ADJACENT) = -1;
	*(int*)(w->visitor + OFF_HVIS_SEC_SEED)     = -1;
	*(int*)(w->visitor + OFF_HVIS_SEC_CENTRE)   = -1;

	*(void**)(w->collection + OFF_COLL_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_COLL_INSTANCE_SIZE)   = 4;
	*(void**)(w->infos + SIZE_INSTANCE_INFO * 1 + OFF_INFO_GRAPH) = w->graphA;
	*(void**)(w->infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = w->graphB;

	*(int*)(w->coarse + OFF_COARSE_OPEN_COUNT) = 8;
	*(int*)(w->coarse + OFF_COARSE_OPEN_CAP)   = 0;
}

static unsigned Key(unsigned section, unsigned index)
{
	return (section << PACKED_KEY_SECTION_SHIFT) | index;
}

static void SetGraph(World* w, unsigned section, void* graph)
{
	*(void**)(w->infos + SIZE_INSTANCE_INFO * section + OFF_INFO_GRAPH) = graph;
}

static void CheckAdjacent()
{
	World w;
	GraphHeuristicCall c;

	Init(&w);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(2, 5), &c);
	Check(c.arm == GH_RUN && c.section == 2 && c.key == Key(2, 5) && c.collection == w.collection,
	      "adjacent: an instance in place runs the original");

	Init(&w);
	SetGraph(&w, 2, 0);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(2, 5), &c);
	Check(c.arm == GH_NO_INSTANCE, "adjacent: a NULL looked-up instance fires");

	// A cache hit never consults the array: a present array entry does not rescue it.
	Init(&w);
	*(int*)(w.visitor + OFF_HVIS_SEC_ADJACENT)    = 2;
	*(void**)(w.visitor + OFF_HVIS_SLOT_ADJACENT) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(2, 5), &c);
	Check(c.arm == GH_NO_INSTANCE, "adjacent: a NULL cached instance fires");

	Init(&w);
	*(int*)(w.visitor + OFF_HVIS_SEC_ADJACENT)    = 2;
	*(void**)(w.visitor + OFF_HVIS_SLOT_ADJACENT) = w.graphA;
	SetGraph(&w, 2, 0);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(2, 5), &c);
	Check(c.arm == GH_RUN, "adjacent: a cache hit does not consult the array");

	// No collection: the slot at +8 is read whatever its section says.
	Init(&w);
	*(void**)(w.visitor + OFF_HVIS_COLLECTION) = 0;
	*(void**)(w.visitor + OFF_HVIS_FALLBACK)   = w.graphA;
	*(int*)(w.visitor + OFF_HVIS_SEC_ADJACENT) = 3;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(1, 0), &c);
	bool fallbackRuns = c.arm == GH_RUN;
	*(void**)(w.visitor + OFF_HVIS_FALLBACK) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(1, 0), &c);
	Check(fallbackRuns && c.arm == GH_NO_COLLECTION, "adjacent: no collection reads the fallback slot");

	Init(&w);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(4, 0), &c);
	bool pastCount = c.arm == GH_BAD_SECTION;
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(0, 0), &c);
	Check(pastCount && c.arm == GH_BAD_SECTION, "adjacent: a section past the count fires");

	Init(&w);
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_ADJACENT, Key(2, 0), &c);
	Check(c.arm == GH_UNJUDGED, "adjacent: an absent instance array is not judged");

	InspectGraphHeuristicKey(0, GH_SITE_ADJACENT, Key(2, 0), &c);
	Check(c.arm == GH_UNJUDGED, "adjacent: no visitor is not judged");
}

static void CheckCentre()
{
	World w;
	GraphHeuristicCall c;

	// The centre reads its own slot at +24 and section at +0x70, not the adjacency test's.
	Init(&w);
	*(int*)(w.visitor + OFF_HVIS_SEC_ADJACENT)    = 2;
	*(void**)(w.visitor + OFF_HVIS_SLOT_ADJACENT) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(2, 1), &c);
	bool ownSlot = c.arm == GH_RUN;
	*(int*)(w.visitor + OFF_HVIS_SEC_CENTRE)    = 2;
	*(void**)(w.visitor + OFF_HVIS_SLOT_CENTRE) = 0;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(2, 1), &c);
	Check(ownSlot && c.arm == GH_NO_INSTANCE, "centre: its own slot and section decide");

	Init(&w);
	SetGraph(&w, 1, 0);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(1, 1), &c);
	Check(c.arm == GH_NO_INSTANCE, "centre: a NULL looked-up instance fires");

	// No collection: the centre has no fallback, so a cache miss would read through NULL.
	Init(&w);
	*(void**)(w.visitor + OFF_HVIS_COLLECTION)  = 0;
	*(void**)(w.visitor + OFF_HVIS_FALLBACK)    = w.graphA;
	*(int*)(w.visitor + OFF_HVIS_SEC_CENTRE)    = 1;
	*(void**)(w.visitor + OFF_HVIS_SLOT_CENTRE) = w.graphA;
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(2, 1), &c);
	Check(c.arm == GH_NO_COLLECTION, "centre: no collection with a stale slot fires");
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(1, 1), &c);
	Check(c.arm == GH_RUN, "centre: no collection with a matching slot runs the original");

	Init(&w);
	InspectGraphHeuristicKey(w.visitor, GH_SITE_CENTRE, Key(9, 0), &c);
	Check(c.arm == GH_BAD_SECTION, "centre: a section past the count fires");
}

static void CheckSeed()
{
	World w;
	GraphHeuristicCall c;
	unsigned goals[3];

	// The early return comes before the visitor is read: a poisoned visitor proves it.
	Init(&w);
	*(int*)(w.coarse + OFF_COARSE_OPEN_CAP) = 8;
	InspectGraphHeuristicSeed(w.coarse, (const void*)(size_t)0x7408687BCull, 0, 0, Key(1, 0), &c);
	Check(c.arm == GH_EARLY_RETURN && !GraphHeuristicArmFires(c.arm), "seed: the early return reads nothing");

	Init(&w);
	*(int*)(w.coarse + OFF_COARSE_OPEN_CAP) = -1;
	InspectGraphHeuristicSeed(w.coarse, w.visitor, 0, 0, Key(1, 0), &c);
	Check(c.arm == GH_RUN, "seed: the early return compares signed");

	Init(&w);
	goals[0] = Key(2, 3);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_RUN && c.key == Key(2, 3), "seed: every instance in place runs the original");

	Init(&w);
	SetGraph(&w, 1, 0);
	goals[0] = Key(2, 3);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_NO_INSTANCE && c.key == Key(1, 0), "seed: an absent start-section instance fires");

	Init(&w);
	SetGraph(&w, 2, 0);
	goals[0] = Key(1, 4);
	goals[1] = Key(2, 3);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 2, Key(1, 0), &c);
	Check(c.arm == GH_NO_INSTANCE && c.key == Key(2, 3) && c.section == 2,
	      "seed: an absent goal-section instance fires");

	Init(&w);
	goals[0] = 0xFFFFFFFFu;
	goals[1] = Key(1, 2);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 2, Key(2, 0), &c);
	Check(c.arm == GH_RUN && c.key == Key(1, 2), "seed: a goal key of 0xFFFFFFFF is skipped");

	// The start's lookup overwrites the slot, so a stale entry for a goal's section is gone
	// by the time that goal is read.
	Init(&w);
	*(int*)(w.visitor + OFF_HVIS_SEC_SEED)    = 2;
	*(void**)(w.visitor + OFF_HVIS_SLOT_SEED) = w.graphB;
	SetGraph(&w, 2, 0);
	goals[0] = Key(2, 3);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_NO_INSTANCE && c.section == 2,
	      "seed: a lookup overwrites the slot before the next key reads it");

	// A goal in the start's section reads what the start's lookup just cached.
	Init(&w);
	goals[0] = Key(1, 6);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_RUN, "seed: a goal in the start's section reads the fresh slot");

	Init(&w);
	SetGraph(&w, 2, 0);
	goals[0] = Key(2, 3);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 0x80000000u, Key(1, 0), &c);
	Check(c.arm == GH_RUN, "seed: a negative signed count walks no goal");

	Init(&w);
	goals[0] = Key(7, 0);
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_BAD_SECTION, "seed: a goal section past the count fires");

	Init(&w);
	*(void**)(w.visitor + OFF_HVIS_COLLECTION) = 0;
	*(void**)(w.visitor + OFF_HVIS_FALLBACK)   = 0;
	InspectGraphHeuristicSeed(w.coarse, w.visitor, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_NO_COLLECTION, "seed: no collection and no fallback fires");

	InspectGraphHeuristicSeed(w.coarse, 0, goals, 1, Key(1, 0), &c);
	Check(c.arm == GH_UNJUDGED, "seed: no visitor is not judged");

	Check(GraphHeuristicSeedWritesStartCluster(GH_RET_HEURISTIC_INIT, true)
	      && !GraphHeuristicSeedWritesStartCluster(GH_RET_HEURISTIC_INIT, false)
	      && !GraphHeuristicSeedWritesStartCluster(0x3AADADu, true),
	      "seed: only the heuristic init's caller turns the heuristic off");
	Check(!GraphHeuristicSeedWritesStartCluster(GH_RET_PATH_EXISTS, true),
	      "seed: the pathExists caller's context gets no heuristic write");
}

static void CheckFallbackAndArms()
{
	GraphPositionVec4 goal0 = { 1.0f, 2.0f, 3.0f, 4.0f };
	GraphPositionVec4 a = GraphHeuristicCentreFallback(&goal0);
	GraphPositionVec4 b = GraphHeuristicCentreFallback(0);
	GraphPositionVec4 sentinel = GraphPositionFarSentinel();
	Check(a.x == 1.0f && a.y == 2.0f && a.z == 3.0f && a.w == 4.0f
	      && b.x == sentinel.x && b.y == sentinel.y && b.z == sentinel.z && b.w == sentinel.w,
	      "centre fallback: goal 0, or the far sentinel");

	Check(!GraphHeuristicArmFires(GH_RUN), "arm GH_RUN does not fire");
	Check(!GraphHeuristicArmFires(GH_UNJUDGED), "arm GH_UNJUDGED does not fire");
	Check(!GraphHeuristicArmFires(GH_EARLY_RETURN), "arm GH_EARLY_RETURN does not fire");
	Check(GraphHeuristicArmFires(GH_BAD_SECTION), "arm GH_BAD_SECTION fires");
	Check(GraphHeuristicArmFires(GH_NO_COLLECTION), "arm GH_NO_COLLECTION fires");
	Check(GraphHeuristicArmFires(GH_NO_INSTANCE), "arm GH_NO_INSTANCE fires");
}

int main()
{
	CheckAdjacent();
	CheckCentre();
	CheckSeed();
	CheckFallbackAndArms();
	return CheckExit("graph_heuristic_guard_units");
}
