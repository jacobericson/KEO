// Drives the shipped guard decision with the pointer that was NULL in the two
// recorded faults, and shows three things in one run:
//
//   1. the engine's own read sequence through that NULL faults, at the same
//      offset the crash records carry;
//   2. the guard's decision path over the identical memory does not fault, and
//      leaves the node carrying the cost and no estimate;
//   3. a node whose estimate is already cached is judged without the heuristic
//      argument being read at all -- proved by handing it a poison pointer.
//
// Links src/fixes/search/graph_visitor_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include "fixes/search/graph_visitor_guard_policy.h"

#include "check.h"

// The offset the engine loads out of the graph instance once it has it: the
// position array, read before anything is indexed. Both crash records carry
// this as the accessed address with a NULL base.
static const size_t OFF_INSTANCE_POSITIONS = 0x30;

struct Fake
{
	unsigned char state[96];
	unsigned char node[16];
	unsigned char heur[32];
	unsigned char holder[32];
};

static void Init(Fake* f, void* instance)
{
	memset(f, 0, sizeof(*f));
	*(void**)(f->state + OFF_SS_CURRENT_NODE)  = f->node;
	*(float*)(f->state + OFF_SS_BEST_COST)     = FLT_MAX;
	*(int*)(f->state + OFF_SS_BEST_NODE)       = -1;
	*(float*)(f->state + OFF_SS_MAX_PATH_COST) = FLT_MAX;
	*(void**)(f->heur + OFF_HEUR_HOLDER)       = f->holder;
	*(void**)(f->holder + OFF_HOLDER_INSTANCE) = instance;
	*(float*)(f->node + OFF_NODE_COST)     = FLT_MAX;
	*(float*)(f->node + OFF_NODE_ESTIMATE) = FLT_MAX;
	*(short*)(f->node + OFF_NODE_FLAGS)    = 0;
}

// The two loads the engine makes after the flag test, written out so the fault
// is this harness's own and its address can be reported.
static bool VanillaReadFaults(const void* heuristic, unsigned __int64* accessOut)
{
	*accessOut = 0;
	__try
	{
		const unsigned char* holder = *(const unsigned char* const*)heuristic;
		const unsigned char* inst   = *(const unsigned char* const*)(holder + OFF_HOLDER_INSTANCE);
		const void* positions = *(const void* const*)(inst + OFF_INSTANCE_POSITIONS);
		if (positions == (const void*)1) printf("");   // keep the load
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*accessOut = OFF_INSTANCE_POSITIONS;
		return true;
	}
}

static bool GuardDecisionFaults(const void* state, const void* heuristic,
                                unsigned int key, GraphVisitorCall* out)
{
	__try
	{
		InspectGraphVisitorCall(state, heuristic, key, out);
		if (GraphVisitorArmSubstitutes(out->arm))
			ApplyGraphVisitorNoEstimate(out, 42.5f);
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return true;
	}
}

int main()
{
	Fake f;

	// 1. The unguarded sequence, with the recorded NULL in place.
	Init(&f, 0);
	unsigned __int64 access = 0;
	Check(VanillaReadFaults(f.heur, &access),
	      "the engine's own read through an absent instance faults");
	printf("  vanilla read: access=0x%02llX (the crash records carry 0x30)\n", access);

	// 2. The guard over the very same memory.
	Init(&f, 0);
	GraphVisitorCall c;
	Check(!GuardDecisionFaults(f.state, f.heur, 0x0EC00000u, &c),
	      "the guard's decision over the same memory does not fault");
	Check(c.arm == GRAPH_VISITOR_NO_INSTANCE, "and names the absent instance");
	Check(c.section == 59 && c.index == 0, "with the section and index from the record");
	Check(*(float*)(f.node + OFF_NODE_COST) == 42.5f, "the node carries its cost");
	Check(*(float*)(f.node + OFF_NODE_ESTIMATE) == FLT_MAX, "and no estimate");
	Check(*(int*)(f.state + OFF_SS_BEST_NODE) == -1, "the early-out node is not claimed");
	Check(*(short*)(f.node + OFF_NODE_FLAGS) == 0, "and nothing is latched");

	// What the caller then computes with those two fields, at the engine's
	// default weight of 1.0: the node loses its queue test against any finite
	// limit, and sits at the very back of the queue when there is none.
	{
		float cost = *(float*)(f.node + OFF_NODE_COST);
		float est  = *(float*)(f.node + OFF_NODE_ESTIMATE);
		float f_finiteLimit = est * 1.0f + cost;
		Check(!(f_finiteLimit <= 1.0e9f), "against a finite path-cost limit the node is dropped");
		Check(f_finiteLimit >= 1.0e38f, "and otherwise it is queued last");
	}

	// 3. A cached node, with a poison heuristic pointer -- the wild pointer one
	// of the sibling faults died on. The guard must not read through it.
	Init(&f, 0);
	*(short*)(f.node + OFF_NODE_FLAGS) = 1;
	Check(!GuardDecisionFaults(f.state, (void*)(size_t)0x7408687BCull, 0x0EC00000u, &c),
	      "a cached node is judged without touching the heuristic argument");
	Check(c.arm == GRAPH_VISITOR_VISITED, "and is handed to the original");
	Check(c.holder == 0 && c.instance == 0, "having read nothing through it");
	Check(*(float*)(f.node + OFF_NODE_COST) == FLT_MAX, "and written nothing");

	// 4. A whole state still runs the game's own code.
	unsigned char instance[64];
	memset(instance, 0, sizeof(instance));
	Init(&f, instance);
	Check(!GuardDecisionFaults(f.state, f.heur, 0x0EC00000u, &c), "a whole state does not fault");
	Check(c.arm == GRAPH_VISITOR_RUN_ORIGINAL, "and is handed to the original");
	Check(*(float*)(f.node + OFF_NODE_COST) == FLT_MAX, "with nothing written by the guard");

	return CheckExit("graph_visitor_injection");
}
