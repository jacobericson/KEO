#include <cstdio>
#include <cstring>
#include <cfloat>
#include "fixes/search/graph_visitor_guard_policy.h"

#include "check.h"

// A stand-in search state, node, heuristic and holder, laid out at the offsets
// the guard uses. Everything is a plain byte block so the test can put a NULL,
// or a poison value, anywhere the engine would have a pointer.
struct Fake
{
	unsigned char state[96];
	unsigned char node[16];
	unsigned char heur[32];
	unsigned char holder[32];
	unsigned char instance[64];
};

static void Init(Fake* f)
{
	memset(f, 0, sizeof(*f));
	*(void**)(f->state + OFF_SS_CURRENT_NODE) = f->node;
	*(float*)(f->state + OFF_SS_BEST_COST)    = 1234.0f;
	*(int*)(f->state + OFF_SS_BEST_NODE)      = -1;
	*(void**)(f->heur + OFF_HEUR_HOLDER)      = f->holder;
	*(void**)(f->holder + OFF_HOLDER_INSTANCE) = f->instance;
	*(float*)(f->node + OFF_NODE_COST)     = FLT_MAX;
	*(float*)(f->node + OFF_NODE_ESTIMATE) = FLT_MAX;
	*(short*)(f->node + OFF_NODE_FLAGS)    = 0;
}

static GraphVisitorArm Arm(const Fake* f, unsigned int key)
{
	GraphVisitorCall c;
	InspectGraphVisitorCall(f->state, f->heur, key, &c);
	return c.arm;
}

int main()
{
	// --- the classification, each arm, ordering pinned ------------------
	Check(ClassifyGraphVisitorCall(0, 0, (void*)1, (void*)1, (void*)1) == GRAPH_VISITOR_NO_NODE,
	      "no node record outranks everything else");
	// The flag test comes before the heuristic: this is what makes the guard
	// unable to fault where the engine would not.
	Check(ClassifyGraphVisitorCall((void*)1, 1, 0, 0, 0) == GRAPH_VISITOR_VISITED,
	      "an open node is cached, whatever the heuristic holds");
	Check(ClassifyGraphVisitorCall((void*)1, 2, 0, 0, 0) == GRAPH_VISITOR_VISITED,
	      "a closed node is cached too");
	Check(ClassifyGraphVisitorCall((void*)1, 4, (void*)1, (void*)1, (void*)1) == GRAPH_VISITOR_RUN_ORIGINAL,
	      "a start flag alone is not a cached estimate");
	Check(ClassifyGraphVisitorCall((void*)1, 0, 0, 0, 0) == GRAPH_VISITOR_NO_HEURISTIC,
	      "an absent heuristic is its own arm");
	Check(ClassifyGraphVisitorCall((void*)1, 0, (void*)1, 0, 0) == GRAPH_VISITOR_NO_HOLDER,
	      "an absent holder is its own arm");
	Check(ClassifyGraphVisitorCall((void*)1, 0, (void*)1, (void*)1, 0) == GRAPH_VISITOR_NO_INSTANCE,
	      "an absent instance is the recorded crash");
	Check(ClassifyGraphVisitorCall((void*)1, 0, (void*)1, (void*)1, (void*)1) == GRAPH_VISITOR_RUN_ORIGINAL,
	      "a new node with its instance runs the game's own code");

	Check(GraphVisitorArmSubstitutes(GRAPH_VISITOR_NO_INSTANCE), "the instance arm substitutes");
	Check(GraphVisitorArmSubstitutes(GRAPH_VISITOR_NO_HOLDER), "the holder arm substitutes");
	Check(GraphVisitorArmSubstitutes(GRAPH_VISITOR_NO_HEURISTIC), "the heuristic arm substitutes");
	Check(!GraphVisitorArmSubstitutes(GRAPH_VISITOR_RUN_ORIGINAL), "a whole call does not");
	Check(!GraphVisitorArmSubstitutes(GRAPH_VISITOR_VISITED), "a cached call does not");
	Check(!GraphVisitorArmSubstitutes(GRAPH_VISITOR_NO_NODE),
	      "a call with no node is left to the engine's own fault");

	// --- reading a real layout ------------------------------------------
	Fake f;
	Init(&f);
	Check(Arm(&f, 0x0C00000B) == GRAPH_VISITOR_RUN_ORIGINAL, "a whole state reads as whole");

	*(void**)(f.holder + OFF_HOLDER_INSTANCE) = 0;
	Check(Arm(&f, 0x0C00000B) == GRAPH_VISITOR_NO_INSTANCE, "an empty instance slot is seen");

	Init(&f);
	*(void**)(f.heur + OFF_HEUR_HOLDER) = 0;
	Check(Arm(&f, 0x0C00000B) == GRAPH_VISITOR_NO_HOLDER, "an empty holder is seen");

	Init(&f);
	*(void**)(f.state + OFF_SS_CURRENT_NODE) = 0;
	Check(Arm(&f, 0x0C00000B) == GRAPH_VISITOR_NO_NODE, "an empty node pointer is seen");

	// A cached node with a poison heuristic: the guard must not dereference
	// it, because the engine does not either.
	Init(&f);
	*(short*)(f.node + OFF_NODE_FLAGS) = 1;
	{
		GraphVisitorCall c;
		InspectGraphVisitorCall(f.state, (void*)(size_t)0x7408687BCull, 0x0C00000B, &c);
		Check(c.arm == GRAPH_VISITOR_VISITED, "a cached node is judged without the heuristic");
		Check(c.holder == 0 && c.instance == 0, "and without reading through it");
	}

	// --- the key split, from the two recorded faults --------------------
	{
		GraphVisitorCall c;
		Init(&f);
		InspectGraphVisitorCall(f.state, f.heur, 0x0C00000Bu, &c);
		Check(c.section == 48 && c.index == 11, "the key splits into section and index");
		InspectGraphVisitorCall(f.state, f.heur, 0x0EC00000u, &c);
		Check(c.section == 59 && c.index == 0, "and again for an index of zero");
	}

	// --- the substitution -----------------------------------------------
	Check(GraphVisitorNoEstimate() == FLT_MAX,
	      "the substitute estimate is the one a fresh node already carries");
	{
		Init(&f);
		*(void**)(f.holder + OFF_HOLDER_INSTANCE) = 0;
		*(float*)(f.node + OFF_NODE_ESTIMATE) = 7.0f;
		*(float*)(f.node + OFF_NODE_COST)     = 0.0f;
		GraphVisitorCall c;
		InspectGraphVisitorCall(f.state, f.heur, 0x0C00000B, &c);
		ApplyGraphVisitorNoEstimate(&c, 42.5f);
		Check(*(float*)(f.node + OFF_NODE_COST) == 42.5f,
		      "the cost is stored, as the engine stores it before looking at all");
		Check(*(float*)(f.node + OFF_NODE_ESTIMATE) == FLT_MAX, "the estimate is the sentinel");
		Check(*(float*)(f.state + OFF_SS_BEST_COST) == 1234.0f,
		      "the early-out node's cost is untouched");
		Check(*(int*)(f.state + OFF_SS_BEST_NODE) == -1, "and so is the early-out node");
		Check(*(short*)(f.node + OFF_NODE_FLAGS) == 0,
		      "nothing is latched, so a later visit tries the estimate again");
	}
	// With no node there is nothing to write and nothing to fault on.
	{
		GraphVisitorCall c;
		memset(&c, 0, sizeof(c));
		c.arm = GRAPH_VISITOR_NO_NODE;
		ApplyGraphVisitorNoEstimate(&c, 1.0f);
	}

	// The substituted estimate must lose the caller's queue test against any
	// finite limit, and must never claim to be the closest node yet.
	{
		float est = GraphVisitorNoEstimate();
		Check(!(est < 1234.0f), "the sentinel never beats a real best cost");
		Check(!(est < est), "nor an untouched one, which starts at the sentinel");
		Check(est * 1.0f + 10.0f > 1.0e30f, "and stays enormous once weighted");
	}

	return CheckExit("graph_visitor_guard_units");
}
