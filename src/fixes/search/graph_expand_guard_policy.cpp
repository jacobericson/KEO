#include "fixes/search/graph_expand_guard_policy.h"

bool GraphExpandArmSubstitutes(GraphExpandArm arm)
{
	return arm == GRAPH_EXPAND_BAD_SECTION
	    || arm == GRAPH_EXPAND_NO_INSTANCE
	    || arm == GRAPH_EXPAND_NO_NODES
	    || arm == GRAPH_EXPAND_NO_POSITIONS;
}

bool GraphExpandArmUnjudged(GraphExpandArm arm)
{
	return arm == GRAPH_EXPAND_NO_OPEN_SET
	    || arm == GRAPH_EXPAND_NO_VISITOR
	    || arm == GRAPH_EXPAND_NO_INSTANCE_ARRAY;
}

void InspectGraphExpandCall(const void* openSet, const void* visitor,
                            GraphExpandCall* out)
{
	out->collection = 0;
	out->instance   = 0;
	out->key        = 0;
	out->section    = 0;
	out->index      = 0;
	out->fromCache  = 0;

	if (!openSet)
	{
		out->arm = GRAPH_EXPAND_NO_OPEN_SET;
		return;
	}

	const unsigned char* os = (const unsigned char*)openSet;
	const void* pairs = *(const void* const*)(os + OFF_OPENSET_DATA);
	const int   count = *(const int*)(os + OFF_OPENSET_SIZE);
	if (!pairs || count <= 0)
	{
		// Nothing to pop. The engine's caller tests the same field before it
		// calls, so this is the caller's own guarantee checked at our site
		// rather than borrowed from it.
		out->arm = GRAPH_EXPAND_NO_OPEN_SET;
		return;
	}

	out->key     = *(const unsigned int*)pairs;
	out->section = out->key >> PACKED_KEY_SECTION_SHIFT;
	out->index   = out->key & PACKED_KEY_INDEX_MASK;

	if (!visitor)
	{
		out->arm = GRAPH_EXPAND_NO_VISITOR;
		return;
	}

	const unsigned char* vis = (const unsigned char*)visitor;
	out->collection = *(const void* const*)(vis + OFF_VIS_COLLECTION);

	// The engine's own branch: a fresh lookup only when there is a collection
	// and the cache holds another section. On the other arm it uses whatever
	// the slot already holds, including a NULL cached earlier.
	if (out->collection
	    && *(const int*)(vis + OFF_VIS_CACHED_SECTION) != (int)out->section)
	{
		const unsigned char* coll = (const unsigned char*)out->collection;
		const int infoCount = *(const int*)(coll + OFF_COLL_INSTANCE_SIZE);
		if (infoCount <= 0 || out->section >= (unsigned int)infoCount)
		{
			// The engine would index the array with this section anyway -- the
			// key's top ten bits reach 1023, so up to 48 KB past its start --
			// and dereference whatever it found there. There is no node to
			// expand either way.
			out->arm = GRAPH_EXPAND_BAD_SECTION;
			return;
		}

		const void* infos = *(const void* const*)(coll + OFF_COLL_INSTANCE_DATA);
		if (!infos)
		{
			out->arm = GRAPH_EXPAND_NO_INSTANCE_ARRAY;
			return;
		}

		const unsigned char* info = (const unsigned char*)infos
		                          + SIZE_INSTANCE_INFO * (size_t)out->section;
		out->instance  = *(const void* const*)(info + OFF_INFO_GRAPH);
		out->fromCache = 0;
	}
	else
	{
		out->instance  = *(const void* const*)(vis + OFF_VIS_CACHED_GRAPH);
		out->fromCache = 1;
	}

	if (!out->instance)
	{
		out->arm = GRAPH_EXPAND_NO_INSTANCE;
		return;
	}

	const unsigned char* graph = (const unsigned char*)out->instance;
	if (!*(const void* const*)(graph + OFF_GRAPH_NODES))
	{
		out->arm = GRAPH_EXPAND_NO_NODES;
		return;
	}
	if (!*(const void* const*)(graph + OFF_GRAPH_POSITIONS))
	{
		out->arm = GRAPH_EXPAND_NO_POSITIONS;
		return;
	}

	out->arm = GRAPH_EXPAND_RUN_ORIGINAL;
}
