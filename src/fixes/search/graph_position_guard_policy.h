#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_POLICY_H

#include <stddef.h>

#include "fixes/search/graph_visitor_guard_policy.h"  // PACKED_KEY_* : the same key split

// Layout and classification for the node-position helper the cluster-graph
// search (checkFaceConnectivity's own graph, not the navmesh A*) calls to turn
// a packed key into a world position: once while filling a node's border-point
// array in the search's setup, and once for the query point itself in the
// search's heuristic. Both callers hand it a small per-search context with its
// own one-instance cache, a slot over from the one graph_visitor_guard and
// graph_expand_guard each own -- a third copy of the same cache shape, on a
// different function, reached from a different caller chain. No Windows
// header and no game pointer here, so a host test can fabricate the context
// and drive every arm.

// The context object (`*ctxPtr`): a streaming collection, a fallback instance
// used only while there is no collection, the one-slot instance cache, and the
// section that slot was filled for.
const size_t OFF_CTX_COLLECTION      = 0;
const size_t OFF_CTX_FALLBACK        = 8;
const size_t OFF_CTX_CACHED_INSTANCE = 16;
const size_t OFF_CTX_CACHED_SECTION  = 0x6C;

// hkaiStreamingCollection: the same 48-byte InstanceInfo stride the sibling
// guards confirmed in the binary.
const size_t OFF_COLL_INSTANCE_DATA = 32;
const size_t OFF_COLL_INSTANCE_SIZE = 40;
const size_t SIZE_INSTANCE_INFO     = 48;
const size_t OFF_INFO_GRAPH         = 16;

// The graph instance's position array, indexed by the packed key's low bits.
// The same field graph_expand_guard's OFF_GRAPH_POSITIONS names one call
// deeper -- this site is the one that reads it first.
const size_t OFF_GRAPH_POSITIONS = 48;

enum GraphPositionArm
{
	// Judged: the instance is in place, so the game's own lookup runs.
	GRAPH_POSITION_RUN_ORIGINAL = 0,

	// Unjudged: the site's own preamble could not be replicated, so nothing
	// was tested and the original runs. A fault here stays on the engine's
	// own instruction, not this one.
	GRAPH_POSITION_NO_CTX,          // the context pointer itself is absent
	GRAPH_POSITION_NO_ARRAY,        // the collection has no instance array

	// Answered by the guard: the instance the site would dereference is
	// absent, reached by one of three different roads, or the section named
	// by the key is outside the collection entirely.
	GRAPH_POSITION_BAD_SECTION,             // the key names a section the collection lacks
	GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION, // no collection, and the fallback slot is empty
	GRAPH_POSITION_NO_INSTANCE_LOOKUP,        // a fresh lookup found no instance
	GRAPH_POSITION_NO_INSTANCE_CACHED         // the cached slot already held no instance
};

// The four arms the guard answers itself.
bool GraphPositionArmSubstitutes(GraphPositionArm arm);

// The two arms that could not be judged at all.
bool GraphPositionArmUnjudged(GraphPositionArm arm);

struct GraphPositionCall
{
	const void*      collection;
	const void*      instance;
	unsigned int     section;
	unsigned int     index;
	GraphPositionArm arm;
};

// Reads what the site's own preamble reads, in its order: whether there is a
// collection, whether the cache already holds this section, and either the
// cached instance or a fresh lookup bounded by the collection's own instance
// count.
void InspectGraphPositionCall(void* const* ctxPtr, unsigned int packedKey,
                              GraphPositionCall* out);

// A plain four-float carrier so this header stays free of any SIMD or
// platform type; the detour reinterprets it as the __m128 the site's own
// callers expect.
struct GraphPositionVec4 { float x, y, z, w; };

// The position a search node with no readable position is given: far enough
// past any in-game coordinate that the site's own Euclidean-distance math
// (a Newton-refined reciprocal square root, then a running minimum across up
// to sixteen candidates) reports it as farther than every real point,
// so it never wins a "closest" comparison and never reads as "right here".
// Chosen well short of FLT_MAX on purpose: squaring a real coordinate's
// difference against FLT_MAX would overflow to +inf, and 1/sqrt(+inf) is 0,
// turning "unreadable" into "distance zero" -- the opposite of deprioritized.
GraphPositionVec4 GraphPositionFarSentinel();

#endif // KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_POLICY_H
