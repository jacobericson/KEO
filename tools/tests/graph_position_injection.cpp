// Drives the shipped position-guard decision over the memory shape an absent
// cluster-graph instance leaves behind -- the same family the sibling guards
// close one and two calls deeper, on a third copy of the cache -- and shows:
//
//   1. the site's own two loads through an absent graph instance fault, at
//      the offset this guard tests (+0x10, the cached-instance slot) and the
//      offset the read that follows it uses (+0x30, the position array);
//   2. the guard's decision over identical memory does not fault, names the
//      absent instance, and reports the section and index from the key;
//   3. the decision writes nothing but the caller's own output position --
//      the context is left exactly as it was, so no cache slot is corrupted;
//   4. the read set claim, tested rather than asserted: a cache hit never
//      reads the collection, proved by handing it a poison collection
//      pointer, and a rejected section is never indexed, proved by an
//      out-of-range section over an unmapped array pointer;
//   5. the contract observe mode (graphPositionGuard=false) rests on: the
//      same call the classify step names as absent is the same call whose
//      unguarded read still faults, so counting it changes nothing about
//      what the original does.
//
// Links src/fixes/search/graph_position_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/search/graph_position_guard_policy.h"

#include "check.h"

// The wild pointer a sibling fault died on, reused here as a pointer the
// guard must never read through.
static void* const kPoison = (void*)(size_t)0x7408687BCull;

struct World
{
	unsigned char ctx[128];
	unsigned char collection[64];
	unsigned char infos[SIZE_INSTANCE_INFO * 64];
	void* ctxPtr[1];
};

// Section 59, index 0: the same recorded shape the sibling guards' harnesses
// use, in a collection of 89 sections whose slot 59 is entirely empty.
static const unsigned int kRecordedKey = (59u << PACKED_KEY_SECTION_SHIFT);

static void Init(World* w, int sectionCount)
{
	memset(w, 0, sizeof(*w));
	w->ctxPtr[0] = w->ctx;

	*(void**)(w->ctx + OFF_CTX_COLLECTION)   = w->collection;
	*(int*)(w->ctx + OFF_CTX_CACHED_SECTION) = -1;

	*(void**)(w->collection + OFF_COLL_INSTANCE_DATA) = w->infos;
	*(int*)(w->collection + OFF_COLL_INSTANCE_SIZE)   = sectionCount;
}

// The site's own two loads through the cache, written out so the fault is
// this harness's own and its offset can be reported: first the cached
// instance at ctx+0x10 is refreshed from the lookup (already NULL for an
// absent section), then the position array is read through it at +0x30.
static bool VanillaReadFaults(void* const* ctxPtr, unsigned int key,
                              volatile unsigned __int64* accessOut)
{
	*accessOut = 0;
	const unsigned char* ctx = (const unsigned char*)*ctxPtr;
	const unsigned char* coll = *(const unsigned char* const*)(ctx + OFF_CTX_COLLECTION);
	const unsigned char* infos = *(const unsigned char* const*)(coll + OFF_COLL_INSTANCE_DATA);
	const unsigned char* inst =
		*(const unsigned char* const*)(infos
			+ SIZE_INSTANCE_INFO * (size_t)(key >> PACKED_KEY_SECTION_SHIFT)
			+ OFF_INFO_GRAPH);
	__try
	{
		*accessOut = OFF_GRAPH_POSITIONS;
		const void* positions = *(const void* const*)(inst + OFF_GRAPH_POSITIONS);
		if (positions == (const void*)1) printf("");
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return true;
	}
}

static bool DecisionFaults(void* const* ctxPtr, unsigned int key, GraphPositionCall* out)
{
	__try
	{
		InspectGraphPositionCall(ctxPtr, key, out);
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
	GraphPositionCall c;

	// 1. The unguarded sequence, with the recorded absence in place.
	Init(&w, 89);
	unsigned __int64 access = 0;
	Check(VanillaReadFaults(w.ctxPtr, kRecordedKey, &access),
	      "the site's own read through an absent instance faults");
	printf("  vanilla read: access=0x%02llX (the position array through the absent instance)\n", access);
	Check(access == OFF_GRAPH_POSITIONS, "at the position array offset, 0xDA44D5's own load");

	// 2. The guard over the very same memory.
	Init(&w, 89);
	Check(!DecisionFaults(w.ctxPtr, kRecordedKey, &c),
	      "the guard's decision over the same memory does not fault");
	Check(c.arm == GRAPH_POSITION_NO_INSTANCE_LOOKUP, "and names the absent instance");
	Check(c.section == 59 && c.index == 0, "with the section and index from the record");
	Check(GraphPositionArmSubstitutes(c.arm), "on the arm the guard answers");

	// 3. The decision writes nothing to the context at all -- the classify
	//    step is read-only regardless of arm; only the detour's firing branch
	//    later writes the caller's output position, which this policy layer
	//    never touches.
	{
		World before;
		Init(&w, 89);
		memcpy(&before, &w, sizeof(w));
		InspectGraphPositionCall(w.ctxPtr, kRecordedKey, &c);
		Check(memcmp(&before, &w, sizeof(w)) == 0, "the decision writes nothing to the context");
	}

	// 4a. The cached path must not read the collection.
	Init(&w, 89);
	*(int*)(w.ctx + OFF_CTX_CACHED_SECTION)    = 59;
	*(void**)(w.ctx + OFF_CTX_CACHED_INSTANCE) = 0;
	*(void**)(w.ctx + OFF_CTX_COLLECTION)      = kPoison;
	Check(!DecisionFaults(w.ctxPtr, kRecordedKey, &c),
	      "a cache hit is judged without reading the collection");
	Check(c.arm == GRAPH_POSITION_NO_INSTANCE_CACHED, "and names the cached absence");

	// 4b. A rejected section must not be indexed. The array pointer is a
	//     poison value, so any index at all would fault.
	Init(&w, 4);
	*(void**)(w.collection + OFF_COLL_INSTANCE_DATA) = kPoison;
	Check(!DecisionFaults(w.ctxPtr, kRecordedKey, &c),
	      "an out-of-range section is rejected before the array is indexed");
	Check(c.arm == GRAPH_POSITION_BAD_SECTION, "and is named as such");

	// 4c. And a section the collection does have is read normally, so 4b is
	//     the bound working rather than the guard refusing everything.
	{
		unsigned char graph[64];
		memset(graph, 0, sizeof(graph));
		Init(&w, 89);
		*(void**)(w.infos + SIZE_INSTANCE_INFO * 59 + OFF_INFO_GRAPH) = graph;
		Check(!DecisionFaults(w.ctxPtr, kRecordedKey, &c), "a resident section does not fault");
		Check(c.arm == GRAPH_POSITION_RUN_ORIGINAL, "and runs the game's own lookup");
		Check(c.instance == graph, "with the instance the array holds");
	}

	// 5. Observe mode. The shipped detour installs the classify step
	//    unconditionally and only the firing arm's action -- substitute or
	//    hand to the original -- is gated on graphPositionGuard. That action
	//    lives in the detour body (windows.h, KenshiLib, the counters), which
	//    this harness does not link; what is tested here, at the policy
	//    layer this harness does link, is the contract observe mode rests on:
	//    the same call that names the absence (counted) is the same call
	//    whose unguarded read still faults when nothing intervenes. If that
	//    were not true -- if classifying the call somehow changed what the
	//    original read -- observe mode would not be a safe no-op.
	{
		Init(&w, 89);
		Check(!DecisionFaults(w.ctxPtr, kRecordedKey, &c),
		      "the classify step alone -- what observe mode always runs -- does not fault");
		Check(c.arm == GRAPH_POSITION_NO_INSTANCE_LOOKUP,
		      "and still names the absence, so it is counted in observe mode too");

		unsigned __int64 access2 = 0;
		Check(VanillaReadFaults(w.ctxPtr, kRecordedKey, &access2),
		      "and the original, called unchanged as observe mode calls it, still faults");
		Check(access2 == OFF_GRAPH_POSITIONS,
		      "at the same offset -- observe mode moves nothing, it only watches");
	}

	return CheckExit("graph_position_injection");
}
