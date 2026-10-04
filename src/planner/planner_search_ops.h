// planner_search_ops.h - The coarse search's operations over the coarse-graph store (CoarseGraphOps'
// arcs, position and inboundMirrored) and the two store reads they share: a node's block and a
// neighbour's block by uid. Pure over the store's read interface (coarse_graph.h) and the arc rules
// (plan_policy.h), with no game header, so a host harness links it with the store's own sources.
// The game calls it on the main thread only, where the store is read.
#pragma once
#include "planner/coarse_graph.h"
#include "planner/coarse_search.h"
#include "planner/plan_build.h"

namespace planner {

const CgBlock* NeighbourOf(void* ctx, int uid, int* dirIndexOut);
const CgBlock* BlockOfKey(unsigned key, int* idx);
int  AdapterArcs(void* ctx, unsigned key, CoarseArc* out, int max);
int  AdapterInboundMirrored(void* ctx, unsigned key);
bool AdapterPosition(void* ctx, unsigned key, float out[3]);

// The operations at the prices p (ctx; NULL prices water as land and acid as water).
void AdapterOps(const PlanSearchParams* p, CoarseGraphOps* out);

} // namespace planner
