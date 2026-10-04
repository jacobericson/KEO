// formation_gather_policy.h - Whether the island tracker and K7 leave a formation member to its group
// while the group gathers. Pure: no game header; main thread in the game.
#pragma once

// A member of a group that has not gathered is left to the group (no K7 deleted form, no arrival-wait
// arming, no evaluation of its own: it is walking to the leader), unless the route planner's merge
// left it alone (it walks the player's own order to the destination and is tracked on its own). A
// gathered group's member is never skipped here.
bool FormationSkipWhileGathering(bool groupGathered, bool memberAlone);
