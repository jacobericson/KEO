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

// One click reaches the order capture as a burst of identical move orders within this many seconds;
// only the repeat test below reads it.
const double PLAN_ORDER_SETTLE = 1.0;

// Why the tick drops a plan, in this order: not a live player character, unconscious, within
// PLAN_POST_ARRIVAL of the destination, or a non-zero movement destination (moveDest) more than
// PLAN_DEST_MATCH in x-z from both the plan's destination (planDest) and the movement destination the
// character held when the plan was written, that the mod did not send for this plan
// (moveDestIsModSend) and that is not a halt onto the character's own position: a new order.
// PDW_ORDER (a non-move order, the stop key, or an order refused while the player list is over the
// planner's cap) and PDW_UNLOCATED (an order whose goal or start could not be located) are the order
// capture's drops, counted by reason; PlanDropDue never returns them.
enum PlanDropWhy { PDW_NONE = 0, PDW_NOT_PLAYER, PDW_KO, PDW_ARRIVED, PDW_NEW_DEST, PDW_ORDER, PDW_UNLOCATED, PDW_COUNT };
PlanDropWhy PlanDropDue(bool livePlayer, bool unconscious, float distToDest, const float moveDest[3],
                        const float planDest[3], const float destAtPlan[3], bool moveDestIsModSend,
                        bool halted);

// A mod detour (the formation's gather) keeps a plan for this long without steering it.
const double PLAN_HOLD_SECONDS = 20.0;
// Whether the movement destination is one the mod sent for this plan: within PLAN_DEST_MATCH in x-z of
// one of the first resendCount re-sends (at most PLAN_RESEND_POINTS), or within the gather's slot
// spread of the hold point while haveHold and holdAge (seconds of game time since the hold was sent,
// frozen while the game is paused) is under PLAN_HOLD_SECONDS.
bool PlanIsModSend(const float moveDest[3], const float resend[][3], int resendCount,
                   const float holdDest[3], int haveHold, double holdAge);

// One click reaches the order capture as a burst of identical move orders. A repeat: the plan is
// under PLAN_ORDER_SETTLE seconds old (planAge) and the new destination lies within one unit of
// the plan's in x-z.
bool PlanRepeatDue(const float planDest[3], const float newDest[3], double planAge);

// The memo key of one order's searches: characters whose start and goal nodes match share one.
unsigned __int64 PlanMemoKey(unsigned startNode, unsigned goalNode);
// Whether a memoised search answers another: the same node pair and the same water multiplier.
bool PlanMemoSame(unsigned __int64 keyA, float multA, unsigned __int64 keyB, float multB);

} // namespace planner

#endif
