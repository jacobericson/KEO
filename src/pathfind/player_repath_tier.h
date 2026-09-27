#ifndef KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_H
#define KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_H

// The game's mid-walk re-request for a player character (HavokCharacter
// ::update 0x1480D7, ::moveAlongPath 0x148B86) always calls requestPath with
// priority 0 -- the same value an NPC's own request carries -- so
// hook_requestPath cannot tell the two apart from the priority argument
// alone. This publishes the set of player-owned HavokCharacter* once a frame
// (main thread) and lets hook_requestPath recognize its own characters'
// re-requests without a priority tag.
//
// Main thread publishes; hook_requestPath reads from the main thread or the
// AI back thread (CharMovement::update, and so its requestPath calls, runs
// there whenever character multithreading is on). The reader takes no lock
// and allocates nothing, so it is safe on either thread even though the two
// never run it at once today.

#include <sstream>
#include "base/config.h"


// Rebuilds the published set from the playerCharacters lektor. Main thread
// only, once a frame; cheap (a handful of characters, capped at 256).
void PlayerRepathTierPublish(double now);

// Publishes an empty set. Main thread only; called while a save is loading,
// so a character pointer from the session before the load cannot be matched
// against a HavokCharacter* the allocator has since reused.
void PlayerRepathTierClear();

// Lock-free reader: true if havokChar is in the last published set. Counts
// every call in playerRepathSeen and every match in playerRepathWouldTier,
// whether or not playerRepathTierEnabled arms the tier.
bool PlayerRepathTierIsPlayerOwned(uintptr_t havokChar);

// Notes that a match was (or, with the key off, would have been) turned into
// a tier write, for the heartbeat.
void PlayerRepathTierNoteTiered();

// Folds this module's counters into an existing heartbeat line.
void PlayerRepathTierAppendStats(std::ostringstream& ss);

// True once any of this module's counters has moved, for a heartbeat's own
// "say nothing this session" gate.
bool PlayerRepathTierAnyStats();


#endif // KENSHI_ZONE_OPT_PLAYER_REPATH_TIER_H
