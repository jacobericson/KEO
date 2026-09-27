// The position guard's classification, over fabricated memory at the real
// offsets: every arm in both polarities, the order the arms are decided in,
// the key split, the two ways an absence is reached (fresh lookup, cached
// slot, and the no-collection fallback), and the section bound.

#include <cstdio>
#include <cstring>
#include "fixes/search/graph_position_guard_policy.h"

#include "check.h"

struct World
{
	unsigned char ctx[128];
	unsigned char collection[64];
	unsigned char infos[SIZE_INSTANCE_INFO * 4];
	unsigned char graph[64];
	void* ctxPtr[1];
};

// A world where everything is in place: a collection with four sections,
// section 2 holding a graph instance.
static void Init(World* w)
{
	memset(w, 0, sizeof(*w));
	w->ctxPtr[0] = w->ctx;

	*(void**)(w->ctx + OFF_CTX_COLLECTION) = w->collection;
	*(int*)(w->ctx + OFF_CTX_CACHED_SECTION) = -1;   // nothing cached yet

	*(void**)(w->collection + OFF_COLL_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_COLL_INSTANCE_SIZE)   = 4;
	*(void**)(w->infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = w->graph;
}

// Section 2, index 7.
static const unsigned int kKey = (2u << PACKED_KEY_SECTION_SHIFT) | 7u;

int main()
{
	World w;
	GraphPositionCall c;

	// No context at all is not judged.
	InspectGraphPositionCall(0, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_CTX, "a NULL context pointer is not judged");
	void* nullCtx[1] = { 0 };
	InspectGraphPositionCall(nullCtx, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_CTX, "a context pointer to NULL is not judged either");

	// Everything in place: a fresh lookup finds the instance.
	Init(&w);
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.section == 2 && c.index == 7, "the key is split the way the site splits it");
	Check(c.arm == GRAPH_POSITION_RUN_ORIGINAL, "a resident section runs the original");
	Check(c.instance == w.graph, "with the instance from the lookup");
	Check(!GraphPositionArmSubstitutes(c.arm) && !GraphPositionArmUnjudged(c.arm),
	      "the judged arm is neither substituting nor unjudged");

	// The absent instance, looked up fresh.
	Init(&w);
	*(void**)(w.infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_INSTANCE_LOOKUP, "an absent instance fires, looked up");
	Check(GraphPositionArmSubstitutes(c.arm), "on a substituting arm");

	// The same absence served out of the context's cache instead: the
	// collection must not be consulted at all.
	Init(&w);
	*(int*)(w.ctx + OFF_CTX_CACHED_SECTION)  = 2;
	*(void**)(w.ctx + OFF_CTX_CACHED_INSTANCE) = 0;
	*(void**)(w.ctx + OFF_CTX_COLLECTION)      = (void*)(size_t)0x7408687BCull; // poison
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_INSTANCE_CACHED, "a cached NULL fires too");
	// The collection pointer is still recorded (the site loads it unconditionally
	// too), but a cache hit never dereferences it -- the injection harness
	// proves that with the same poison value, where a dereference would fault.

	// A cache hit on a present instance takes the cached pointer, not the
	// array's, so the two are told apart by giving them different values.
	Init(&w);
	*(int*)(w.ctx + OFF_CTX_CACHED_SECTION)    = 2;
	*(void**)(w.ctx + OFF_CTX_CACHED_INSTANCE) = w.graph;
	*(void**)(w.infos + SIZE_INSTANCE_INFO * 2 + OFF_INFO_GRAPH) = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_RUN_ORIGINAL, "a cache hit does not consult the array");
	Check(c.instance == w.graph, "and uses the cached pointer");

	// No collection at all: the site falls back on a single fixed slot,
	// whatever it holds, and so does the guard.
	Init(&w);
	*(void**)(w.ctx + OFF_CTX_COLLECTION) = 0;
	*(void**)(w.ctx + OFF_CTX_FALLBACK)   = w.graph;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_RUN_ORIGINAL, "no collection uses the fallback slot");
	Init(&w);
	*(void**)(w.ctx + OFF_CTX_COLLECTION) = 0;
	*(void**)(w.ctx + OFF_CTX_FALLBACK)   = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION, "no collection and an empty slot fires");

	// The instance array itself absent, on a cache miss: nothing was tested,
	// so the original runs and faults on the engine's own instruction.
	Init(&w);
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_NO_ARRAY, "an absent instance array is not judged");
	Check(GraphPositionArmUnjudged(c.arm), "and lands in the unjudged group");
	Check(!GraphPositionArmSubstitutes(c.arm), "not the firing one");

	// The section bound is checked before the array is indexed, so an
	// out-of-range section fires even when the array pointer is absent.
	Init(&w);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE)   = 2;
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_BAD_SECTION, "the bound is tested before the array");
	Init(&w);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE) = 0;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_BAD_SECTION, "an empty collection fires");
	Init(&w);
	*(int*)(w.collection + OFF_COLL_INSTANCE_SIZE) = 3;
	InspectGraphPositionCall(w.ctxPtr, kKey, &c);
	Check(c.arm == GRAPH_POSITION_RUN_ORIGINAL, "the last in-range section does not fire");

	// The sentinel: far on every axis, and not FLT_MAX (which would overflow
	// on squaring and read back as distance zero).
	{
		GraphPositionVec4 v = GraphPositionFarSentinel();
		Check(v.x > 1.0e5f && v.y > 1.0e5f && v.z > 1.0e5f,
		      "the sentinel is far past any in-game coordinate");
		Check(v.x < 1.0e30f, "and nowhere near FLT_MAX, so squaring it cannot overflow");
	}

	// The group predicates partition the arms, which is what makes the
	// counters close.
	{
		const GraphPositionArm arms[7] = {
			GRAPH_POSITION_RUN_ORIGINAL, GRAPH_POSITION_NO_CTX, GRAPH_POSITION_NO_ARRAY,
			GRAPH_POSITION_BAD_SECTION, GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION,
			GRAPH_POSITION_NO_INSTANCE_LOOKUP, GRAPH_POSITION_NO_INSTANCE_CACHED };
		int run = 0, unjudged = 0, fired = 0;
		for (int i = 0; i < 7; ++i)
		{
			bool s = GraphPositionArmSubstitutes(arms[i]);
			bool u = GraphPositionArmUnjudged(arms[i]);
			Check(!(s && u), "no arm is both substituting and unjudged");
			if (s) ++fired; else if (u) ++unjudged; else ++run;
		}
		Check(run == 1 && unjudged == 2 && fired == 4,
		      "the seven arms partition one run, two unjudged and four firing");
	}

	return CheckExit("graph_position_guard_units");
}
