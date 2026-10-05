#include "zone/retention/zone_expiry_guard_policy.h"

ZoneExpiryRoute ZoneExpiryRouteFor(bool expires, ZoneRetentionAnswer answer)
{
	if (!expires)
		return ZONE_EXPIRY_ROUTE_NONE;
	if (answer == ZONE_RETENTION_ANSWER_RELEASE)
		return ZONE_EXPIRY_ROUTE_RETENTION_FENCE;
	if (answer == ZONE_RETENTION_ANSWER_HELD)
		return ZONE_EXPIRY_ROUTE_NONE;
	return ZONE_EXPIRY_ROUTE_CLAIMS_GUARD;
}

ZoneExpiryAction ZoneExpiryOnBegin(NmFenceResult begin)
{
	switch (begin)
	{
	case NM_FENCE_CLAIMS_ONLY:
		return ZONE_EXPIRY_RUN_FENCED;
	case NM_FENCE_IDLE:
	case NM_FENCE_UNAVAILABLE:
		return ZONE_EXPIRY_RUN_UNFENCED;
	default:
		return ZONE_EXPIRY_HOLD;
	}
}

bool ZoneExpiryHoldStartsEpisode(double lastHeldAt, double now)
{
	return lastHeldAt < 0.0 || now - lastHeldAt > ZONE_EXPIRY_HOLD_GAP_SEC;
}

bool ZoneExpiryHoldOverdue(double heldSeconds)
{
	return heldSeconds >= ZONE_EXPIRY_HOLD_WARN_SEC;
}
