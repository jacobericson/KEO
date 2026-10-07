// audit_scene_rules.h - The scene and render probes' pure rules: a transforms level's size bin, a
// visible object's class by two vtable slots, the render queues a cull fork walks, the state-object
// would-skip test, the empty-draw and consumed-flag tests, the pass-group map walk and a barrier
// worker's kind. No Windows or game header, so the host tests build it.

#ifndef KENSHI_FRAME_AUDIT_SCENE_RULES_H
#define KENSHI_FRAME_AUDIT_SCENE_RULES_H

#include <stddef.h>

namespace scenerules
{

// A depth level by its node count.
enum LevelBinKind { LB_EMPTY, LB_SMALL, LB_MID, LB_LARGE };

inline int LevelBin(size_t nodes)
{
	return nodes == 0 ? LB_EMPTY : (nodes <= 64 ? LB_SMALL : (nodes <= 512 ? LB_MID : LB_LARGE));
}

// A batch when the instance-cull slot holds one of the two batch overrides (a positive list: a class
// from another module holds an import thunk there), an entity when the render-queue slot is the
// entity's, anything else other.
enum VisClassKind { VC_OTHER, VC_ENTITY, VC_BATCH };

inline int VisClass(size_t queueSlot, size_t cullSlot, size_t entityQueue, size_t batchCull, size_t vtfCull)
{
	if (cullSlot == batchCull || cullSlot == vtfCull)
		return VC_BATCH;
	return queueSlot == entityQueue ? VC_ENTITY : VC_OTHER;
}

// The render queues one cull fork walks for a manager: [first, min(numQueues, last)), each block
// `stride` bytes with its object count at `countOffset`.
struct QueueCount
{
	int    queues, empty;
	size_t objects;
};

inline void CountQueues(const unsigned char* blocks, size_t stride, size_t countOffset,
                        size_t first, size_t last, size_t numQueues, QueueCount* out)
{
	size_t end = numQueues < last ? numQueues : last;
	for (size_t rq = first; rq < end; ++rq)
	{
		size_t n = *(const size_t*)(blocks + rq * stride + countOffset);
		++out->queues;
		out->objects += n;
		if (n == 0)
			++out->empty;
	}
}

// A flagged state the render system would re-create from a description equal to the one that made
// the still-bound object (and, for depth-stencil, with the same stencil reference).
inline bool WouldSkip(bool flagged, bool shadowHeld, bool boundSet, bool descEqual, bool refEqual)
{
	return flagged && shadowHeld && boundSet && descEqual && refEqual;
}

// RenderOperation's vertex data pointer and VertexData's vertex count (a qword), as _render's first two
// tests read them.
const size_t OP_VERTEX_DATA = 0x0, VD_VERTEX_COUNT = 0x30;

// _render returns before its state section when the operation has no vertex data or no vertices: it
// consumes no changed flag and binds nothing.
inline bool DrawsNothing(const void* op)
{
	const unsigned char* vd = *(const unsigned char* const*)((const unsigned char*)op + OP_VERTEX_DATA);
	return vd == NULL || *(const unsigned long long*)(vd + VD_VERTEX_COUNT) == 0;
}

// A flagged state counts as remade only when the draw consumed its changed flag (the byte reads 0 after).
inline bool FlagConsumed(unsigned char changedAfter)
{
	return changedAfter == 0;
}

// Its description becomes the bound object's only when the flag was consumed and an object is bound.
inline bool TakesShadow(unsigned char changedAfter, bool boundSet)
{
	return FlagConsumed(changedAfter) && boundSet;
}

// A barrier worker by the semaphore it waits on or releases: unclassed until both of the main
// barrier's handles are published (they are written one at a time), then main when it is one of
// them, else another scene manager's.
enum SlotKindValue { SLOT_UNCLASSED, SLOT_MAIN, SLOT_OTHER };

inline int SlotKind(long long sem, long long main0, long long main1)
{
	if (main0 == 0 || main1 == 0)
		return SLOT_UNCLASSED;
	return sem == main0 || sem == main1 ? SLOT_MAIN : SLOT_OTHER;
}

// The pass-group map, an MSVC red-black tree whose leaves link to the head.
const size_t NODE_LEFT = 0x00, NODE_PARENT = 0x08, NODE_RIGHT = 0x10, NODE_VALUE = 0x20, NODE_ISNIL = 0x29;

inline const unsigned char* Link(const unsigned char* node, size_t off)
{
	return *(const unsigned char* const*)(node + off);
}

inline bool IsNil(const unsigned char* node)
{
	return node[NODE_ISNIL] != 0;
}

// In-order successor; the head after the last node.
inline const unsigned char* MapNext(const unsigned char* node)
{
	const unsigned char* r = Link(node, NODE_RIGHT);
	if (!IsNil(r))
	{
		node = r;
		for (const unsigned char* l = Link(node, NODE_LEFT); !IsNil(l); l = Link(l, NODE_LEFT))
			node = l;
		return node;
	}
	const unsigned char* p = Link(node, NODE_PARENT);
	while (!IsNil(p) && node == Link(p, NODE_RIGHT))
	{
		node = p;
		p = Link(p, NODE_PARENT);
	}
	return p;
}

// Nodes walked from the head's leftmost link (at most maxNodes), and those whose renderable list
// (begin and end at +0 and +8 of the value) is not empty.
inline size_t MapWalk(const unsigned char* head, size_t maxNodes, size_t* nonEmpty)
{
	size_t n = 0;
	*nonEmpty = 0;
	for (const unsigned char* node = Link(head, NODE_LEFT); node != head && n < maxNodes; node = MapNext(node))
	{
		const unsigned char* list = Link(node, NODE_VALUE);
		if (list && Link(list, 0) != Link(list, 8))
			++*nonEmpty;
		++n;
	}
	return n;
}

} // namespace scenerules

#endif
