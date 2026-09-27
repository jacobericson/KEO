#include "pathfind/player_repath_tier_policy.h"

const int PLAYER_REPATH_TIER = 45;

PlayerRepathTierDecision PlayerRepathTierDecide(int priority, bool matchedPlayerSet, bool enabledKey)
{
	PlayerRepathTierDecision d;
	d.tier = 0;
	d.source = PRT_SOURCE_NONE;

	if (priority >= 2)
	{
		d.tier = PLAYER_REPATH_TIER;
		d.source = PRT_SOURCE_ORDER;
	}
	else if (matchedPlayerSet)
	{
		d.source = PRT_SOURCE_STATE6;
		if (enabledKey)
			d.tier = PLAYER_REPATH_TIER;
	}

	return d;
}
