// plan_merge.h - A run-together order's merge onto one route, pure: the anchor (the member nearest the
// others), the biased search's operations, the full-price re-cost and the detour cap, the join index,
// the gather index and the prefix sample. No Windows, KenshiLib or game header; main thread in the game.
#pragma once
#include "planner/coarse_search.h"

namespace planner {

const int   PLAN_MERGE_MEMBERS  = 32;       // at least MAX_FORMATION_MEMBERS (30)
const float PLAN_MERGE_WALK_OFF = 4608.0f;  // one cell: a gather walk this far from its biased prefix left it

// The member with the least summed x-z distance to the others; ties go to the lower index; *sumOut its
// sum (when not NULL). -1, and a sum of 0, for n <= 0.
int PlanMergeMedoid(const float (*xz)[2], int n, float* sumOut);
// The member the formation leads with: the medoid in on; the first member in observe and off, which
// move nothing.
int PlanMergeLeader(int mode, int medoid);

// The anchor route's nodes, sorted once for the biased arcs' lookups. Scratch the caller owns.
struct PlanMergeSet { unsigned nodes[COARSE_ROUTE_MAX]; int count; };
// The set of routeNodes[0..n) (n held to 0..COARSE_ROUTE_MAX), sorted, each node once.
void PlanMergeSetFrom(const unsigned* routeNodes, int n, PlanMergeSet* set);
bool PlanMergeSetHas(const PlanMergeSet& set, unsigned node);

// A biased search: the full-price operations, the anchor route's set and the bias k (held to >= 1).
struct PlanMergeBias { const CoarseGraphOps* inner; const PlanMergeSet* set; float k; };
// Operations over bias: an arc whose far node is in the set costs its full price over k, every other
// arc its full price; every position is the inner one over k, so the straight-line heuristic is divided
// by k and stays a lower bound (no arc costs less than its length over k). out->ctx points at bias.
void PlanMergeBiasOps(PlanMergeBias* bias, CoarseGraphOps* out);

// The route's cost at full price: along nodes[0..n), the cheapest arc inner reports from each node to
// the next, summed; -1 when a step has no such arc (the graph changed under the route).
float PlanMergeRecost(const CoarseGraphOps& inner, const unsigned* nodes, int n);
// Whether a member joins: its biased route at full price within (1 + detourPercent / 100) of its own
// optimum; false for a negative real cost or an own cost at or below 0.
bool PlanMergeJoins(float realCost, float ownCost, int detourPercent);
// The detour in whole per cent, (realCost / ownCost - 1) * 100 rounded down; 0 when not above ownCost.
int  PlanMergeDetourPercent(float realCost, float ownCost);

// The join: walking the member's route from its start, its first node the set holds; that node's index
// on the anchor route, with *memberStep its index on the member's. -1 when the first shared node is the
// anchor's goal (its last node: meeting at the destination is no join) or there is none.
int PlanMergeJoinIndex(const unsigned* anchor, int anchorCount, const PlanMergeSet& set,
                       const unsigned* member, int memberCount, int* memberStep);
// The gather index: the largest of joins[0..n) (a member alone reads -1); 0 when none is above 0.
int PlanMergeGatherIndex(const int* joins, int n);
// At most max of nodes[0..n), spread over them with the first and the last kept; returns the count.
int PlanMergeSample(const unsigned* nodes, int n, unsigned* out, int max);

} // namespace planner
