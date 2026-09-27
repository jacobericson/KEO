#include <cstdio>
#include "pathfind/player_repath_tier_policy.h"

#include "check.h"

int main()
{
	// A player's own order (priority >= 2) always gets the order tier,
	// whatever the O6 key says and whether or not it happens to also match
	// the published set (it never would -- the order runs before the set
	// exists for that character's own request -- but the rule must not care).
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(2, false, true);
		Check(d.tier == 45, "priority>=2 tiers unconditionally");
		Check(d.source == PRT_SOURCE_ORDER, "priority>=2 is the order source");
	}
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(2, false, false);
		Check(d.tier == 45, "the order path ignores the O6 key");
		Check(d.source == PRT_SOURCE_ORDER, "still the order source with the key off");
	}

	// A genuine NPC request (priority < 2, not in the published set) never
	// gets a tier, matching the game's own priority.
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(0, false, true);
		Check(d.tier == 0, "an unmatched NPC request is left alone");
		Check(d.source == PRT_SOURCE_NONE, "no source for an NPC request");
	}

	// O6's case: a state-6 mid-walk re-request (priority 0) for a character
	// matched against the published player set. Tiered with the key on.
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(0, true, true);
		Check(d.tier == 45, "a matched state-6 re-request is tiered when the key is on");
		Check(d.source == PRT_SOURCE_STATE6, "the state-6 source is reported");
	}

	// Same match, key off (observe mode): source still reports the match (so
	// the caller counts a would-tier), but the returned tier is 0 -- nothing
	// is written to the request.
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(0, true, false);
		Check(d.tier == 0, "observe mode never writes the tier");
		Check(d.source == PRT_SOURCE_STATE6, "observe mode still classifies the match");
	}

	// priority == 1 is not a value the game sends today, but the rule reads
	// as "< 2", so it follows the state-6 path rather than the order path.
	{
		PlayerRepathTierDecision d = PlayerRepathTierDecide(1, true, true);
		Check(d.source == PRT_SOURCE_STATE6, "priority 1 with a match takes the state-6 path");
	}

	return CheckExit("player_repath_tier_units");
}
