// The expand guard's classification, over fabricated memory at the real
// offsets: every arm in both polarities, the order the arms are decided in,
// the key split, the two cache paths, and the section bound.

#include <cstdio>
#include <cstring>
#include "fixes/search/graph_expand_guard_policy.h"

#include "check.h"

struct World
{
	unsigned char openSet[16];
	unsigned int  pairs[8];
	unsigned char visitor[128];
	unsigned char collection[64];
	unsigned char infos[SIZE_INSTANCE_INFO * 4];
	unsigned char graph[64];
	unsigned char nodes[64];
	unsigned char positions[64];
};

// A world where everything is in place: one node queued, a collection with
// four sections, section 2 holding a graph instance with both arrays.
static void Init(World* w, unsigned int key)
{
	memset(w, 0, sizeof(*w));
	w->pairs[0] = key;
	*(void**)(w->openSet + OFF_OPENSET_DATA) = w->pairs;
	*(int*)(w->openSet + OFF_OPENSET_SIZE)   = 1;

	*(void**)(w->graph + OFF_GRAPH_NODES)     = w->nodes;
	*(void**)(w->graph + OFF_GRAPH_POSITIONS) = w->positions;

	*(void**)(w->collection + OFF_COLL_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_COLL_INSTANCE_SIZE)   = 4;
	*(void**)(w->infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = w->graph;

	*(void**)(w->visitor + OFF_VIS_COLLECTION)   = w->collection;
	*(int*)(w->visitor + OFF_VIS_CACHED_SECTION) = -1;   // nothing cached yet
	*(void**)(w->visitor + OFF_VIS_CACHED_GRAPH) = 0;
}

// Section 2, node 7.
static const unsigned int kKey = (2u << PACKED_KEY_SECTION_SHIFT) | 7u;

int main()
{
	World w;
	GraphExpandCall c;

	// The key split, against the key the recorded fault carried and the one
	// this suite uses.
	InspectGraphExpandCall(0, 0, &c);
	Check(c.arm == GRAPH_EXPAND_NO_OPEN_SET, "no open set is not judged");

	Init(&w, 0x0EC00000u);
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.key == 0x0EC00000u, "the key is peeked from the front of the open set");
	Check(c.section == 59 && c.index == 0, "and split as the engine splits it");
	Check(c.arm == GRAPH_EXPAND_BAD_SECTION, "section 59 is outside a four-section collection");
	Check(c.fromCache == 0, "on the lookup path");

	// An empty open set is not judged, and no key is invented for it.
	Init(&w, kKey);
	*(int*)(w.openSet + OFF_OPENSET_SIZE) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_OPEN_SET, "an empty open set is not judged");
	Check(c.key == 0, "and carries no key");
	Init(&w, kKey);
	*(void**)(w.openSet + OFF_OPENSET_DATA) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_OPEN_SET, "an open set with no pairs is not judged");

	// No visitor: the key is still read, because the engine reads it first.
	Init(&w, kKey);
	InspectGraphExpandCall(w.openSet, 0, &c);
	Check(c.arm == GRAPH_EXPAND_NO_VISITOR, "no visitor is not judged");
	Check(c.key == kKey, "and the key is already known");

	// Everything in place.
	Init(&w, kKey);
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_RUN_ORIGINAL, "a whole section runs the original");
	Check(c.instance == w.graph, "with the instance from the lookup");
	Check(c.fromCache == 0, "and the lookup path recorded");
	Check(!GraphExpandArmSubstitutes(c.arm) && !GraphExpandArmUnjudged(c.arm),
	      "the judged arm is neither substituting nor unjudged");

	// The absent instance, looked up fresh: the recorded shape, one slot over.
	Init(&w, kKey);
	*(void**)(w.infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE, "an absent instance fires");
	Check(c.fromCache == 0, "and is reported as looked up");
	Check(GraphExpandArmSubstitutes(c.arm), "on a substituting arm");

	// The same absence served out of the visitor's cache instead.
	Init(&w, kKey);
	*(int*)(w.visitor + OFF_VIS_CACHED_SECTION) = 2;
	*(void**)(w.visitor + OFF_VIS_CACHED_GRAPH) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE, "a cached NULL fires too");
	Check(c.fromCache == 1, "and is reported as cached");

	// A cache hit on a present instance takes the cached pointer, not the
	// array's -- so the two are told apart by giving them different values.
	Init(&w, kKey);
	*(int*)(w.visitor + OFF_VIS_CACHED_SECTION) = 2;
	*(void**)(w.visitor + OFF_VIS_CACHED_GRAPH) = w.graph;
	*(void**)(w.infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_RUN_ORIGINAL, "a cache hit does not consult the array");
	Check(c.fromCache == 1 && c.instance == w.graph, "and uses the cached pointer");

	// No collection at all: the engine falls back on the cached slot, whatever
	// it holds, and so does the guard.
	Init(&w, kKey);
	*(void**)(w.visitor + OFF_VIS_COLLECTION)   = 0;
	*(void**)(w.visitor + OFF_VIS_CACHED_GRAPH) = w.graph;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_RUN_ORIGINAL, "no collection uses the cached slot");
	Check(c.fromCache == 1, "on the cached path");
	Init(&w, kKey);
	*(void**)(w.visitor + OFF_VIS_COLLECTION)   = 0;
	*(void**)(w.visitor + OFF_VIS_CACHED_GRAPH) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE, "no collection and an empty slot fires");

	// The instance array itself absent, on a cache miss: nothing was tested,
	// so the original runs and faults on the engine's own instruction.
	Init(&w, kKey);
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_INSTANCE_ARRAY, "an absent instance array is not judged");
	Check(GraphExpandArmUnjudged(c.arm), "and lands in the unjudged group");
	Check(!GraphExpandArmSubstitutes(c.arm), "not the firing one");

	// The section bound is checked before the array is indexed, so an
	// out-of-range section fires even when the array pointer is absent.
	Init(&w, kKey);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE)   = 2;
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_BAD_SECTION, "the bound is tested before the array");
	Init(&w, kKey);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_BAD_SECTION, "an empty collection fires");
	Init(&w, kKey);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE) = 3;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_RUN_ORIGINAL, "the last in-range section does not fire");

	// The two second-level pointers are distinct, and each is its own arm.
	Init(&w, kKey);
	*(void**)(w.graph + OFF_GRAPH_NODES) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_NODES, "an instance with no node array fires");
	Init(&w, kKey);
	*(void**)(w.graph + OFF_GRAPH_POSITIONS) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_POSITIONS, "an instance with no position array fires");
	Init(&w, kKey);
	*(void**)(w.graph + OFF_GRAPH_NODES)     = 0;
	*(void**)(w.graph + OFF_GRAPH_POSITIONS) = 0;
	InspectGraphExpandCall(w.openSet, w.visitor, &c);
	Check(c.arm == GRAPH_EXPAND_NO_NODES,
	      "with both absent the node array is named, the order the engine reads them in");

	// The group predicates partition the arms, which is what makes the
	// counters close.
	{
		const GraphExpandArm arms[8] = {
			GRAPH_EXPAND_RUN_ORIGINAL, GRAPH_EXPAND_NO_OPEN_SET,
			GRAPH_EXPAND_NO_VISITOR, GRAPH_EXPAND_NO_INSTANCE_ARRAY,
			GRAPH_EXPAND_BAD_SECTION, GRAPH_EXPAND_NO_INSTANCE,
			GRAPH_EXPAND_NO_NODES, GRAPH_EXPAND_NO_POSITIONS };
		int judged = 0, unjudged = 0, fired = 0;
		for (int i = 0; i < 8; ++i)
		{
			bool s = GraphExpandArmSubstitutes(arms[i]);
			bool u = GraphExpandArmUnjudged(arms[i]);
			Check(!(s && u), "no arm is both substituting and unjudged");
			if (s) ++fired; else if (u) ++unjudged; else ++judged;
		}
		Check(judged == 1 && unjudged == 3 && fired == 4,
		      "the eight arms partition one judged, three unjudged and four firing");
	}

	return CheckExit("graph_expand_guard_units");
}
