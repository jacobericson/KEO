#include "fixes/search/graph_position_guard_policy.h"

bool GraphPositionArmSubstitutes(GraphPositionArm arm)
{
	return arm == GRAPH_POSITION_BAD_SECTION
	    || arm == GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION
	    || arm == GRAPH_POSITION_NO_INSTANCE_LOOKUP
	    || arm == GRAPH_POSITION_NO_INSTANCE_CACHED;
}

bool GraphPositionArmUnjudged(GraphPositionArm arm)
{
	return arm == GRAPH_POSITION_NO_CTX
	    || arm == GRAPH_POSITION_NO_ARRAY;
}

void InspectGraphPositionCall(void* const* ctxPtr, unsigned int packedKey,
                              GraphPositionCall* out)
{
	out->collection = 0;
	out->instance   = 0;
	out->section    = packedKey >> PACKED_KEY_SECTION_SHIFT;
	out->index      = packedKey & PACKED_KEY_INDEX_MASK;

	if (!ctxPtr)
	{
		out->arm = GRAPH_POSITION_NO_CTX;
		return;
	}

	const unsigned char* ctx = (const unsigned char*)*ctxPtr;
	if (!ctx)
	{
		out->arm = GRAPH_POSITION_NO_CTX;
		return;
	}

	out->collection = *(const void* const*)(ctx + OFF_CTX_COLLECTION);

	if (!out->collection)
	{
		// No streaming context at all: the site takes the fallback slot
		// unconditionally, and so does the guard.
		out->instance = *(const void* const*)(ctx + OFF_CTX_FALLBACK);
		out->arm = out->instance ? GRAPH_POSITION_RUN_ORIGINAL
		                         : GRAPH_POSITION_NO_INSTANCE_NO_COLLECTION;
		return;
	}

	const int cachedSection = *(const int*)(ctx + OFF_CTX_CACHED_SECTION);
	if (cachedSection == (int)out->section)
	{
		// A cache hit: the site never touches the collection on this arm, and
		// neither does the guard.
		out->instance = *(const void* const*)(ctx + OFF_CTX_CACHED_INSTANCE);
		out->arm = out->instance ? GRAPH_POSITION_RUN_ORIGINAL
		                         : GRAPH_POSITION_NO_INSTANCE_CACHED;
		return;
	}

	const unsigned char* coll = (const unsigned char*)out->collection;
	const int count = *(const int*)(coll + OFF_COLL_INSTANCE_SIZE);
	if (count <= 0 || out->section >= (unsigned int)count)
	{
		// The site would index the instance array with this section anyway,
		// unbounded, and dereference whatever it found. There is no position
		// to read either way.
		out->arm = GRAPH_POSITION_BAD_SECTION;
		return;
	}

	const void* infos = *(const void* const*)(coll + OFF_COLL_INSTANCE_DATA);
	if (!infos)
	{
		// Nothing to test the section against. Left to the original: it
		// faults on its own instruction, at a different address than the one
		// this guard answers for.
		out->arm = GRAPH_POSITION_NO_ARRAY;
		return;
	}

	const unsigned char* info = (const unsigned char*)infos
	                          + SIZE_INSTANCE_INFO * (size_t)out->section;
	out->instance = *(const void* const*)(info + OFF_INFO_GRAPH);
	out->arm = out->instance ? GRAPH_POSITION_RUN_ORIGINAL
	                         : GRAPH_POSITION_NO_INSTANCE_LOOKUP;
}

GraphPositionVec4 GraphPositionFarSentinel()
{
	GraphPositionVec4 v;
	v.x = v.y = v.z = 1.0e6f;
	v.w = 0.0f;
	return v;
}
