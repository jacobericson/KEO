#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_EXPAND_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_EXPAND_GUARD_POLICY_H

#include <stddef.h>

#include "fixes/search/graph_visitor_guard_policy.h"  // PACKED_KEY_* : the same key split

// Layout and classification for the A* iteration that pops the next node from
// the open set and expands it. Before it can expand anything it needs the
// popped node's own world position, which it reads through the graph instance
// of the node's section -- the same per-section cache the node-cost site uses,
// one slot over. Neither the cached pointer nor the two arrays reached through
// it are checked. No Windows header and no game pointer here, so a host test
// can fabricate an open set, a visitor and a collection and drive every arm.

// The open set: a pointer to packed (key, cost) pairs, then its size. The pop
// returns the key of the pair at the front, read before the heap is disturbed,
// so the same value can be peeked without consuming the node.
const size_t OFF_OPENSET_DATA = 0;
const size_t OFF_OPENSET_SIZE = 8;

// The graph visitor, which is also the object that caches one graph instance
// per section. Slot +8 serves the node being expanded and is keyed at +104;
// slot +16 serves the neighbour the node-cost site estimates and is keyed at
// +108. This guard is the +8 slot's; graph_visitor_guard is the +16 slot's.
const size_t OFF_VIS_COLLECTION     = 0;
const size_t OFF_VIS_CACHED_GRAPH   = 8;
const size_t OFF_VIS_CACHED_SECTION = 104;

// hkaiStreamingCollection: m_instances is an array of 48-byte InstanceInfo
// records whose +16 is the section's graph instance.
const size_t OFF_COLL_INSTANCE_DATA = 32;
const size_t OFF_COLL_INSTANCE_SIZE = 40;
const size_t SIZE_INSTANCE_INFO     = 48;
const size_t OFF_INFO_GRAPH         = 16;

// The graph instance's two arrays, both indexed by the packed key's low bits:
// the node records at +16 and the node positions at +48.
const size_t OFF_GRAPH_NODES     = 16;
const size_t OFF_GRAPH_POSITIONS = 48;

enum GraphExpandArm
{
	// Judged: the section's graph instance and both of its arrays are there,
	// so the game's own expansion runs untouched.
	GRAPH_EXPAND_RUN_ORIGINAL = 0,

	// Unjudged. Nothing could be tested, so the original runs and a fault
	// stays on the engine's own instruction instead of moving into the mod.
	GRAPH_EXPAND_NO_OPEN_SET,        // no open set, or it is empty
	GRAPH_EXPAND_NO_VISITOR,         // no visitor, so no cache and no collection
	GRAPH_EXPAND_NO_INSTANCE_ARRAY,  // the collection has no instance array

	// Answered by the guard: the node cannot be expanded at all.
	GRAPH_EXPAND_BAD_SECTION,        // the key names a section the collection does not have
	GRAPH_EXPAND_NO_INSTANCE,        // the section has no graph instance
	GRAPH_EXPAND_NO_NODES,           // it has one, but no node array
	GRAPH_EXPAND_NO_POSITIONS        // it has one, but no position array
};

// The four arms the guard answers itself.
bool GraphExpandArmSubstitutes(GraphExpandArm arm);

// True for the three arms that could not judge the call. Kept apart from the
// judged arm so "the instance was tested and was there" is never counted
// together with "nothing was tested", which is what makes a quiet counter
// readable.
bool GraphExpandArmUnjudged(GraphExpandArm arm);

struct GraphExpandCall
{
	const void*    collection;
	const void*    instance;
	unsigned int   key;        // the packed key at the front of the open set
	unsigned int   section;    // its top bits: which section the node is in
	unsigned int   index;      // and which node within that section
	int            fromCache;  // 1 = the visitor's cached slot, 0 = a fresh lookup
	GraphExpandArm arm;
};

// Reads what the expansion's own preamble reads, in its order: the key at the
// front of the open set, the collection, the cache key, then either the
// freshly looked-up instance or the cached one, then the two arrays. The one
// read the engine does not make is the instance array's size, used to bound
// the section index before indexing with it -- a field of the object the
// engine already dereferences eight bytes earlier.
void InspectGraphExpandCall(const void* openSet, const void* visitor,
                            GraphExpandCall* out);

#endif // KENSHI_ZONE_OPT_FIXES_GRAPH_EXPAND_GUARD_POLICY_H
