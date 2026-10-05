// Host tests for the expiry guard's route and action tables and its hold arithmetic.
#include "zone/retention/zone_expiry_guard_policy.h"

#include "check.h"

static void TestRoute()
{
	Check(ZoneExpiryRouteFor(false, ZONE_RETENTION_ANSWER_NOT_MINE) == ZONE_EXPIRY_ROUTE_NONE &&
	      ZoneExpiryRouteFor(false, ZONE_RETENTION_ANSWER_HELD) == ZONE_EXPIRY_ROUTE_NONE &&
	      ZoneExpiryRouteFor(false, ZONE_RETENTION_ANSWER_RELEASE) == ZONE_EXPIRY_ROUTE_NONE,
	      "a cell that does not expire this frame goes nowhere");
	Check(ZoneExpiryRouteFor(true, ZONE_RETENTION_ANSWER_RELEASE) == ZONE_EXPIRY_ROUTE_RETENTION_FENCE,
	      "a tracked release takes the full fence");
	Check(ZoneExpiryRouteFor(true, ZONE_RETENTION_ANSWER_HELD) == ZONE_EXPIRY_ROUTE_NONE,
	      "a retention hold calls the original with its hold written");
	Check(ZoneExpiryRouteFor(true, ZONE_RETENTION_ANSWER_NOT_MINE) == ZONE_EXPIRY_ROUTE_CLAIMS_GUARD,
	      "an untracked expiring cell takes the claims guard");
	Check(ZONE_RETENTION_ANSWER_NOT_MINE == 0 &&
	      ZoneExpiryRouteFor(true, (ZoneRetentionAnswer)0) == ZONE_EXPIRY_ROUTE_CLAIMS_GUARD,
	      "retention off answers the zero value, which takes the claims guard");
}

static void TestOnBegin()
{
	static const ZoneExpiryAction kWant[NM_FENCE_IDLE + 1] =
	{
		ZONE_EXPIRY_HOLD,          // HELD: the full mode's
		ZONE_EXPIRY_HOLD,          // NO_LOCK: the full mode's
		ZONE_EXPIRY_HOLD,          // REFUSED_JOB: the full mode's
		ZONE_EXPIRY_HOLD,          // REFUSED_CLAIM
		ZONE_EXPIRY_HOLD,          // REFUSED
		ZONE_EXPIRY_HOLD,          // DEFER_PJ: the full mode's
		ZONE_EXPIRY_RUN_UNFENCED,  // UNAVAILABLE
		ZONE_EXPIRY_HOLD,          // RELEASED: never a begin's answer
		ZONE_EXPIRY_RUN_FENCED,    // CLAIMS_ONLY
		ZONE_EXPIRY_RUN_UNFENCED   // IDLE
	};
	bool ok = NM_FENCE_IDLE == 9;
	for (int v = 0; v <= NM_FENCE_IDLE; ++v)
		ok = ok && ZoneExpiryOnBegin((NmFenceResult)v) == kWant[v];
	Check(ok, "the action table over every fence result");
	Check(ZoneExpiryOnBegin(NM_FENCE_REFUSED) == ZONE_EXPIRY_HOLD,
	      "a refusal with the publication up holds, never runs unfenced");
	Check(ZoneExpiryOnBegin(NM_FENCE_REFUSED_CLAIM) == ZONE_EXPIRY_HOLD,
	      "a claim found after the publication holds");
	Check(ZoneExpiryOnBegin(NM_FENCE_CLAIMS_ONLY) == ZONE_EXPIRY_RUN_FENCED,
	      "a begun claims fence runs the original under the publication");
	Check(ZoneExpiryOnBegin(NM_FENCE_IDLE) == ZONE_EXPIRY_RUN_UNFENCED &&
	      ZoneExpiryOnBegin(NM_FENCE_UNAVAILABLE) == ZONE_EXPIRY_RUN_UNFENCED,
	      "no generator yet, or no fence possible, runs unfenced");
}

static void TestHoldEpisode()
{
	Check(ZoneExpiryHoldStartsEpisode(-1.0, 5.0), "a first hold starts an episode");
	Check(!ZoneExpiryHoldStartsEpisode(5.0, 5.0 + ZONE_EXPIRY_HOLD_GAP_SEC),
	      "a hold within the gap continues the episode");
	Check(ZoneExpiryHoldStartsEpisode(5.0, 5.0 + ZONE_EXPIRY_HOLD_GAP_SEC + 0.01),
	      "a hold past the gap starts another");
	Check(ZONE_EXPIRY_HOLD_GAP_SEC >= 10.0 * ZONE_RETENTION_HOLD_MARGIN,
	      "the gap outlasts a held cell's next expiry");
	Check(!ZoneExpiryHoldOverdue(ZONE_EXPIRY_HOLD_WARN_SEC - 0.01) && ZoneExpiryHoldOverdue(ZONE_EXPIRY_HOLD_WARN_SEC),
	      "the warning is due at the warning length");
}

int main()
{
	TestRoute();
	TestOnBegin();
	TestHoldEpisode();
	return CheckExit("zone_expiry_guard_units");
}
