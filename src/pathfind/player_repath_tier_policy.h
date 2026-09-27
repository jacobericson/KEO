#ifndef KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_POLICY_H
#define KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_POLICY_H

// The tiering decision hook_requestPath makes, pulled out of the hook so it
// is host-testable and cannot drift from what player_repath_tier.cpp does.
// See player_repath_tier.h for the published-set side.

enum PlayerRepathTierSource
{
	PRT_SOURCE_NONE,    // leave the game's own priority alone
	PRT_SOURCE_ORDER,   // priority >= 2: the order itself
	PRT_SOURCE_STATE6   // priority < 2, matched the published player set
};

struct PlayerRepathTierDecision
{
	int                   tier;    // 0 = don't write; otherwise the tier to write
	PlayerRepathTierSource source; // which rule produced it, for the split counter
};

// priority is requestPath's own argument. matchedPlayerSet is whether the
// character is in this frame's published player-owned set (only meaningful,
// and only checked, when priority < 2). enabledKey is playerRepathTierEnabled:
// with it false, a state-6 match is still classified PRT_SOURCE_STATE6 (so the
// caller counts a would-tier) but the returned tier is 0 (observe mode never
// writes it). The order path (priority >= 2) is unconditional; this key does
// not gate it.
PlayerRepathTierDecision PlayerRepathTierDecide(int priority, bool matchedPlayerSet, bool enabledKey);

#endif // KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_POLICY_H
