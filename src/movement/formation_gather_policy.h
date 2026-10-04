// formation_gather_policy.h - The gathering group's decisions: whether the island tracker and K7 leave a
// member to its group, when a member's deleted gather order is sent again, how long the gather may take,
// and when the group timeout ends a group. Pure: no game header; main thread in the game.
#pragma once

// A member of a group that has not gathered is left to the group (no K7 deleted form, no arrival-wait
// arming, no evaluation of its own: it is walking to the leader), unless the route planner's merge
// left it alone (it walks the player's own order to the destination and is tracked on its own). A
// gathered group's member is never skipped here.
bool FormationSkipWhileGathering(bool groupGathered, bool memberAlone);

// Whether a gathering member's gather order is sent again: it was sent once (gatherSent), the engine
// has since deleted it (orderGone), the member stands outside both the gather radius and farSq
// (squared x-z distances to the gather point), and cooldownSec has passed since its last send. The far
// bound tells a deleted order short of the point from one that completed at the member's slot, which
// reads the same to the order state; a completed order stands within the engine's slot spread.
bool FormationGatherResendDue(bool gatherSent, float distSq, float gatherRadiusSq, float farSq, bool orderGone,
                              double sinceLastSendSec, double cooldownSec);

// The gather timeout in seconds: the farthest member's route distance to the gather point at the group's
// speed (game units per second), times FORMATION_GATHER_MARGIN, held to [floorSec, capSec]; floorSec when
// the distance or the speed is not positive.
const double FORMATION_GATHER_MARGIN = 1.5;
double FormationGatherTimeout(float routeDist, float speed, double floorSec, double capSec);

// Whether the group timeout ends a group of age ageSec: past limitSec it does, unless the order was merged
// and at least one member is still walking to the destination, which keeps it until ceilingSec.
bool FormationTimeoutCancels(double ageSec, double limitSec, double ceilingSec, bool merged, int walking);
