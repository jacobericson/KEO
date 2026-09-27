#include "fixes/search/graph_visitor_guard_policy.h"

#include <float.h>

bool GraphVisitorArmSubstitutes(GraphVisitorArm arm)
{
	return arm == GRAPH_VISITOR_NO_HEURISTIC
	    || arm == GRAPH_VISITOR_NO_HOLDER
	    || arm == GRAPH_VISITOR_NO_INSTANCE;
}

GraphVisitorArm ClassifyGraphVisitorCall(const void* node, int flags,
                                         const void* heuristic, const void* holder,
                                         const void* instance)
{
	if (!node)
		return GRAPH_VISITOR_NO_NODE;
	if ((flags & NODE_FLAGS_VISITED) != 0)
		return GRAPH_VISITOR_VISITED;
	if (!heuristic)
		return GRAPH_VISITOR_NO_HEURISTIC;
	if (!holder)
		return GRAPH_VISITOR_NO_HOLDER;
	if (!instance)
		return GRAPH_VISITOR_NO_INSTANCE;
	return GRAPH_VISITOR_RUN_ORIGINAL;
}

void InspectGraphVisitorCall(const void* state, const void* heuristic,
                             unsigned int packedKey, GraphVisitorCall* out)
{
	out->node      = 0;
	out->heuristic = heuristic;
	out->holder    = 0;
	out->instance = 0;
	out->flags    = 0;
	out->section  = packedKey >> PACKED_KEY_SECTION_SHIFT;
	out->index    = packedKey & PACKED_KEY_INDEX_MASK;

	if (!state)
	{
		out->arm = GRAPH_VISITOR_RUN_ORIGINAL;
		return;
	}

	const unsigned char* ss = (const unsigned char*)state;
	out->node = *(void* const*)(ss + OFF_SS_CURRENT_NODE);
	if (!out->node)
	{
		out->arm = GRAPH_VISITOR_NO_NODE;
		return;
	}

	const unsigned char* node = (const unsigned char*)out->node;
	out->flags = *(const short*)(node + OFF_NODE_FLAGS);
	if ((out->flags & NODE_FLAGS_VISITED) != 0)
	{
		out->arm = GRAPH_VISITOR_VISITED;
		return;
	}

	if (heuristic)
	{
		const unsigned char* h = (const unsigned char*)heuristic;
		out->holder = *(const void* const*)(h + OFF_HEUR_HOLDER);
		if (out->holder)
		{
			const unsigned char* holder = (const unsigned char*)out->holder;
			out->instance = *(const void* const*)(holder + OFF_HOLDER_INSTANCE);
		}
	}

	out->arm = ClassifyGraphVisitorCall(out->node, out->flags, heuristic,
	                                    out->holder, out->instance);
}

float GraphVisitorNoEstimate()
{
	return FLT_MAX;
}

void ApplyGraphVisitorNoEstimate(const GraphVisitorCall* call, float cost)
{
	unsigned char* node = (unsigned char*)call->node;
	if (!node)
		return;
	*(float*)(node + OFF_NODE_COST)     = cost;
	*(float*)(node + OFF_NODE_ESTIMATE) = GraphVisitorNoEstimate();
}
