// Drives the shipped expand-guard decision over the memory shape the recorded
// fault leaves behind, one cache slot over from the node-cost site, and shows
// four things in one run:
//
//   1. the engine's own two loads through an absent graph instance fault, at
//      the two offsets the expansion reads them from -- 0x10 first, then 0x30;
//   2. the guard's decision over identical memory does not fault, names the
//      absent instance, and reports the section and node from the recorded key;
//   3. a peek of the open set does not consume the node, so the guard sees the
//      key the pop would have returned without disturbing the heap;
//   4. the read set claim, tested rather than asserted: on the cached path the
//      guard never touches the collection, proved by handing it a poison
//      collection pointer, and it never reads past a section it has rejected,
//      proved by an out-of-range section over an unmapped array pointer.
//
// Links src/fixes/search/graph_expand_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/search/graph_expand_guard_policy.h"

#include "check.h"

// The wild pointer a sibling fault died on, reused here as a pointer the guard
// must never read through.
static void* const kPoison = (void*)(size_t)0x7408687BCull;

struct World
{
	unsigned char openSet[16];
	unsigned int  pairs[8];
	unsigned char visitor[128];
	unsigned char collection[64];
	unsigned char infos[SIZE_INSTANCE_INFO * 64];
};

// The recorded key: section 59, node 0, in a collection of 89 sections whose
// slot 59 is entirely empty.
static const unsigned int kRecordedKey = 0x0EC00000u;

static void Init(World* w, unsigned int key, int sectionCount)
{
	memset(w, 0, sizeof(*w));
	w->pairs[0] = key;
	w->pairs[1] = 0x41200000u;              // its queue key, never read here
	w->pairs[2] = 0x00800001u;              // a second node, to prove no pop
	*(void**)(w->openSet + OFF_OPENSET_DATA) = w->pairs;
	*(int*)(w->openSet + OFF_OPENSET_SIZE)   = 2;

	*(void**)(w->collection + OFF_COLL_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_COLL_INSTANCE_SIZE)   = sectionCount;

	*(void**)(w->visitor + OFF_VIS_COLLECTION)   = w->collection;
	*(int*)(w->visitor + OFF_VIS_CACHED_SECTION) = -1;
	*(void**)(w->visitor + OFF_VIS_CACHED_GRAPH) = 0;
}

// The expansion's own two loads through the instance, written out so the fault
// is this harness's own and its offset can be reported.
static bool VanillaReadFaults(const void* visitor, unsigned int key,
                              volatile unsigned __int64* accessOut)
{
	*accessOut = 0;
	const unsigned char* vis = (const unsigned char*)visitor;
	const unsigned char* coll = *(const unsigned char* const*)(vis + OFF_VIS_COLLECTION);
	const unsigned char* infos = *(const unsigned char* const*)(coll + OFF_COLL_INSTANCE_DATA);
	const unsigned char* inst =
		*(const unsigned char* const*)(infos
			+ SIZE_INSTANCE_INFO * (size_t)(key >> PACKED_KEY_SECTION_SHIFT)
			+ OFF_INFO_GRAPH);
	__try
	{
		// DA58F1: the node record, through the array at +16.
		const void* nodes = *(const void* const*)(inst + OFF_GRAPH_NODES);
		if (nodes == (const void*)1) printf("");
		*accessOut = OFF_GRAPH_POSITIONS;
		// DA5938: the node position, through the array at +48. Only reachable
		// if the load above somehow did not fault.
		const void* pos = *(const void* const*)(inst + OFF_GRAPH_POSITIONS);
		if (pos == (const void*)1) printf("");
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*accessOut = *accessOut ? *accessOut : OFF_GRAPH_NODES;
		return true;
	}
}

static bool DecisionFaults(const void* openSet, const void* visitor,
                           GraphExpandCall* out)
{
	__try
	{
		InspectGraphExpandCall(openSet, visitor, out);
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return true;
	}
}

int main()
{
	World w;
	GraphExpandCall c;

	// 1. The unguarded sequence, with the recorded absence in place. Slot 59
	//    of an 89-section collection is empty, exactly as the dump reads it.
	Init(&w, kRecordedKey, 89);
	unsigned __int64 access = 0;
	Check(VanillaReadFaults(w.visitor, kRecordedKey, &access),
	      "the expansion's own read through an absent instance faults");
	printf("  vanilla read: access=0x%02llX (the first of the expansion's two loads)\n", access);
	Check(access == OFF_GRAPH_NODES,
	      "and the first load is the node array at +0x10, which dominates the +0x30 one");

	// 2. The guard over the very same memory.
	Init(&w, kRecordedKey, 89);
	Check(!DecisionFaults(w.openSet, w.visitor, &c),
	      "the guard's decision over the same memory does not fault");
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE, "and names the absent instance");
	Check(c.section == 59 && c.index == 0, "with the section and node from the record");
	Check(c.fromCache == 0, "reporting a fresh lookup, not a cached NULL");
	Check(GraphExpandArmSubstitutes(c.arm), "on the arm the guard answers");

	// 3. The peek does not consume anything: the open set is byte-identical
	//    afterwards, and the front key is the one the pop would return.
	{
		World before;
		Init(&w, kRecordedKey, 89);
		memcpy(&before, &w, sizeof(w));
		InspectGraphExpandCall(w.openSet, w.visitor, &c);
		Check(memcmp(&before, &w, sizeof(w)) == 0,
		      "the decision writes nothing at all -- no pop, no cache update");
		Check(c.key == w.pairs[0], "and reads the key the pop would have returned");
		Check(*(int*)(w.openSet + OFF_OPENSET_SIZE) == 2, "with the open set still two deep");
	}

	// 4a. The cached path must not read the collection.
	Init(&w, kRecordedKey, 89);
	*(void**)(w.visitor + OFF_VIS_COLLECTION)   = kPoison;
	*(int*)(w.visitor + OFF_VIS_CACHED_SECTION) = 59;
	*(void**)(w.visitor + OFF_VIS_CACHED_GRAPH) = 0;
	Check(!DecisionFaults(w.openSet, w.visitor, &c),
	      "a cache hit is judged without reading the collection");
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE && c.fromCache == 1,
	      "and names the cached absence");

	// 4b. A rejected section must not be indexed. The array pointer is a
	//     poison value, so any index at all would fault.
	Init(&w, kRecordedKey, 4);
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = kPoison;
	Check(!DecisionFaults(w.openSet, w.visitor, &c),
	      "an out-of-range section is rejected before the array is indexed");
	Check(c.arm == GRAPH_EXPAND_BAD_SECTION, "and is named as such");

	// 4c. And a section the collection does have is read normally, so 4b is
	//     the bound working rather than the guard refusing everything.
	{
		unsigned char graph[64], nodes[16], positions[16];
		memset(graph, 0, sizeof(graph));
		*(void**)(graph + OFF_GRAPH_NODES)     = nodes;
		*(void**)(graph + OFF_GRAPH_POSITIONS) = positions;
		Init(&w, kRecordedKey, 89);
		*(void**)(w.infos + SIZE_INSTANCE_INFO * 59 + OFF_INFO_GRAPH) = graph;
		Check(!DecisionFaults(w.openSet, w.visitor, &c), "a resident section does not fault");
		Check(c.arm == GRAPH_EXPAND_RUN_ORIGINAL, "and runs the game's own expansion");
		Check(c.instance == graph, "with the instance the array holds");
	}

	return CheckExit("graph_expand_injection");
}
