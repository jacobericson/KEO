#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_VISITOR_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_VISITOR_GUARD_POLICY_H

#include <stddef.h>

// Layout, classification and the substitute estimate for the A* site that
// records a search node's cost and its estimated distance still to travel.
// The site is hkaiHashSearchState::setCost with the hierarchical nav-mesh
// heuristic inlined into it. No Windows header and no game pointer here, so a
// host test can fabricate a search state and drive every arm.

// hkaiHashSearchStateBase, offsets including the 16-byte referenced-object
// head the binary carries. Only the four the site touches are named.
const size_t OFF_SS_BEST_NODE     = 48;  // the node kept so an early-out search
const size_t OFF_SS_BEST_COST     = 52;  //   can still return a partial path
const size_t OFF_SS_MAX_PATH_COST = 56;
const size_t OFF_SS_CURRENT_NODE  = 72;  // the node this call is about

// One search node: cost so far, estimate to go, then index, parent and flags.
const size_t OFF_NODE_COST     = 0;
const size_t OFF_NODE_ESTIMATE = 4;
const size_t OFF_NODE_FLAGS    = 14;

// Open or closed. Either means the estimate has already been written once and
// the site reuses it instead of recomputing, so the heuristic is not consulted
// at all -- the engine's own cached path.
const int NODE_FLAGS_VISITED = 3;

// The heuristic turns a packed key into a world position through a graph
// instance cached per section: its argument's first pointer is the holder of
// that cache, and the holder's +16 is the instance for the section the caller
// last looked up. The engine indexes the instance's position array with the
// key's low 22 bits and checks neither pointer; an absent instance is a NULL
// there, and the read that follows it is the fault this guard prevents.
const size_t OFF_HEUR_HOLDER     = 0;
const size_t OFF_HOLDER_INSTANCE = 16;

const unsigned int PACKED_KEY_INDEX_MASK    = 0x3FFFFF;
const int          PACKED_KEY_SECTION_SHIFT = 22;

enum GraphVisitorArm
{
	GRAPH_VISITOR_RUN_ORIGINAL = 0,  // a new node with its instance in place
	GRAPH_VISITOR_VISITED,           // the estimate is cached; the heuristic is not read
	GRAPH_VISITOR_NO_NODE,           // no node record, so nothing to judge
	GRAPH_VISITOR_NO_HEURISTIC,      // the heuristic argument itself is absent
	GRAPH_VISITOR_NO_HOLDER,         // its cache holder is absent
	GRAPH_VISITOR_NO_INSTANCE        // the section's instance is absent
};

// The three arms the guard answers itself. Everything else calls the original,
// including GRAPH_VISITOR_NO_NODE: a call with no node record faults on the
// engine's own instruction exactly as it did before the guard existed, which
// keeps a vanilla fault attributed to vanilla code.
bool GraphVisitorArmSubstitutes(GraphVisitorArm arm);

// Pure, over values already read. The order is the site's own order, and it is
// the reason the guard cannot fault where the engine would not: the flag test
// comes before the holder, so a call whose estimate is cached never reads the
// heuristic argument at all.
GraphVisitorArm ClassifyGraphVisitorCall(const void* node, int flags,
                                         const void* heuristic, const void* holder,
                                         const void* instance);

struct GraphVisitorCall
{
	void*          node;
	const void*    heuristic;
	const void*    holder;
	const void*    instance;
	int            flags;
	unsigned int   section;   // the packed key's top bits: which section is missing
	unsigned int   index;     // and which position in it
	GraphVisitorArm arm;
};

// Reads what the site's own code reads, in its order, and nothing else. A NULL
// search state is left to the original for the same reason as a NULL node.
void InspectGraphVisitorCall(const void* state, const void* heuristic,
                             unsigned int packedKey, GraphVisitorCall* out);

// The estimate a node carries before one has ever been computed for it, and
// the value the engine's own start-node test reads as "no usable estimate".
float GraphVisitorNoEstimate();

// Writes the two node fields the site writes on the path it cannot finish: the
// cost, which the engine stores before it ever looks at the heuristic, and the
// estimate. The early-out node and its cost are deliberately left untouched --
// substituting the estimate keeps the node in the search at the back of the
// queue, whereas claiming to be the closest node yet would aim a partial path
// at a node whose position is exactly what we could not read.
void ApplyGraphVisitorNoEstimate(const GraphVisitorCall* call, float cost);

#endif // KENSHI_ZONE_OPT_FIXES_GRAPH_VISITOR_GUARD_POLICY_H
