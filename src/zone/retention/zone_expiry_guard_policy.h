#ifndef KEO_ZONE_EXPIRY_GUARD_POLICY_H
#define KEO_ZONE_EXPIRY_GUARD_POLICY_H

#include "zone/retention/zone_retention_policy.h"
#include "navmesh/jobs/nm_unload_fence_policy.h"

// Which fence an expiring Set B cell takes in the ZoneMap::update prologue,
// and what the expiry guard does with its fence's answer. No game types and no
// game state.

enum ZoneExpiryRoute
{
	ZONE_EXPIRY_ROUTE_NONE = 0,         // the original runs as it would anyway
	ZONE_EXPIRY_ROUTE_RETENTION_FENCE,  // retention wants the cell gone: its full fence
	ZONE_EXPIRY_ROUTE_CLAIMS_GUARD      // not retention's: the expiry guard
};

// `answer` is read only when `expires`.
ZoneExpiryRoute ZoneExpiryRouteFor(bool expires, ZoneRetentionAnswer answer);

enum ZoneExpiryAction
{
	ZONE_EXPIRY_HOLD = 0,        // write the hold and let the original keep the cell
	ZONE_EXPIRY_RUN_FENCED,      // the original runs with the publication up
	ZONE_EXPIRY_RUN_UNFENCED     // no claim can exist, or no fence ever can
};

// The claims-mode begin's result. Anything that is not a begun publication or
// a proof that no claim can exist holds: a refusal never runs unfenced.
ZoneExpiryAction ZoneExpiryOnBegin(NmFenceResult begin);

// A hold leaves the town countdown a margin of a few frames, so a held cell
// expires again within a fraction of a second. A hold more than the gap after
// the cell's last one starts a new episode; an episode that reaches the
// warning length is said once.
const double ZONE_EXPIRY_HOLD_GAP_SEC  = 1.0;
const double ZONE_EXPIRY_HOLD_WARN_SEC = 30.0;
bool ZoneExpiryHoldStartsEpisode(double lastHeldAt, double now);
bool ZoneExpiryHoldOverdue(double heldSeconds);

#endif // KEO_ZONE_EXPIRY_GUARD_POLICY_H
