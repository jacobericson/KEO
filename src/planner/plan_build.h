// plan_build.h - The route planner's plan-building rules, pure: the legs from a coarse route, the
// footprint pick for an unloaded point, the drop predicate and the per-order search memo key.
#ifndef KENSHI_ZONE_OPT_PLANNER_PLAN_BUILD_H
#define KENSHI_ZONE_OPT_PLANNER_PLAN_BUILD_H

#include "planner/plan_policy.h"

namespace planner {

// One step of a coarse route: the node's directory index, and, when the next step is in another
// section, the resolved cross arc's portal and edge.
struct PlanRouteStep { int dirIndex; int node; int crossesNext; float portal[3]; float edgeA[3]; float edgeB[3]; };

// One leg per section change along steps[0..n), in route order, then the destination (dest, its
// cell, destSection) with isDestination = 1. At most PLAN_MAX_PORTAL_LEGS portal legs: a longer
// route keeps its first PLAN_MAX_PORTAL_LEGS and sets *truncated. Returns the leg count (1 for a
// route that never changes section).
int PlanBuildLegs(const PlanRouteStep* steps, int n, const float dest[3], int destSection,
                  PlanLeg* out, int* truncated);

// A candidate node for the footprint pick: its box and centre, world units.
struct PlanNodeBox { float boxMin[3], boxMax[3], centre[3]; };
// Among the boxes widened by 5 units in x and z and 30 in y that hold p, the nearest centre in 3D;
// with none and allowFallback, the nearest centre within 200 units in x-z; else -1.
int PlanPickFootprint(const PlanNodeBox* nodes, int n, const float p[3], bool allowFallback);

// The engine applies a move order asynchronously, so for this long after an order the character's
// movement destination can still be the previous one; the mod's re-issue path waits the same second
// before reading an order's result.
const double PLAN_ORDER_SETTLE = 1.0;

// Why the tick drops a plan, in this order: not a live player character, unconscious, within
// PLAN_POST_ARRIVAL of the destination, or, once the plan is PLAN_ORDER_SETTLE seconds old
// (planAge, seconds since the plan was written), a non-zero movement destination more than
// PLAN_DEST_MATCH from the plan's (a new order or the stop key's halt).
enum PlanDropWhy { PDW_NONE = 0, PDW_NOT_PLAYER, PDW_KO, PDW_ARRIVED, PDW_NEW_DEST };
PlanDropWhy PlanDropDue(bool livePlayer, bool unconscious, float distToDest,
                        const float moveDest[3], const float planDest[3], double planAge);

// One click reaches the order capture as a burst of identical move orders. A repeat: the plan is
// under PLAN_ORDER_SETTLE seconds old (planAge) and the new destination lies within one unit of
// the plan's in x-z.
bool PlanRepeatDue(const float planDest[3], const float newDest[3], double planAge);

// The memo key of one order's searches: characters whose start and goal nodes match share one.
unsigned __int64 PlanMemoKey(unsigned startNode, unsigned goalNode);

} // namespace planner

#endif
