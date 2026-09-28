// movement_config.h - the movement module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"

// k7PostDeathHold: a deleted-order "x" swap (the current task leaves
// ORDER_TYPE_MOVE with an empty deque) whose order died first (an end
// signature preceded the swap) is HELD instead of dropped, so the
// deleted-order re-issue can still send it once the swap's task -- a combat
// task on the whitelist (k7_swap_policy.h) -- ends. K7_HOLD_ON is the
// default: on. K7_HOLD_OBSERVE classifies and logs "K7 hold (observe)" but
// drops exactly as today; K7_HOLD_OFF is today's behaviour with no
// classification. The would-hold and refusal counters count in every mode
// (island_reissue_diag.cpp IslandReissueAppendDiag).
enum K7HoldMode { K7_HOLD_OFF = 0, K7_HOLD_OBSERVE = 1, K7_HOLD_ON = 2 };

namespace movement {

// Starts as a copy of kMovementDefaults, then written by LoadConfig on the
// main thread before any hook installs. Two main-thread writers later only
// clear: CheckBuildGate (plugin_entry.cpp), when the build gate fails and no
// hook installs, clears islandFixEnabled and groupCohesionEnabled and sets
// cfg_islandFarSpan to 0; InstallHooks (hook_manifest.cpp), with earlier
// hooks already live, clears groupCohesionEnabled when the scatter patch
// fails or the addOrderSelectedCharacters hook is missing. Read on any
// thread; a hook running during that clear reads the old or the new value
// of one field.
struct MovementConfig
{
	bool groupCohesionEnabled;

	bool islandFixEnabled;            // islandFix: overlay answers the island hooks

// islandFarSpan: the island hook answers false for a vanilla same-island pair
// whose cells are this many or more apart (Chebyshev), so setDestination takes
// its edge-route branch instead of one direct path. 0 = vanilla answer. Read
// at startup only; independent of islandFix.
	int  cfg_islandFarSpan;

// islandEdgeRing: NavMesh::getZoneEdge rays from the destination toward the
// character and stops at the first island face it crosses. For a same-island
// destination that face sits on the destination cell's own near boundary, so
// a far order still asks the path thread for a leg as long as the whole
// island, which exhausts the search's node cap. The getIsland hook narrows
// the list it hands back to that one caller (identified by its return
// address) to the cells within Chebyshev 1 of the character's own cell, so
// the leg is at most about 2 cells -- but only when that whole neighbourhood
// is present in the list; an incomplete one is left alone (partial= on
// IslandSpan:) rather than filtered into a park. The ring is armed, and its
// counters (ra=/filt/rm/partial/skip/big) run, whenever its hooks installed,
// independent of this key. false leaves the list alone (computes and counts
// only); true removes it. Applies to every character, NPCs included. Read at
// startup only. See ring= on IslandSpan:.
	bool islandEdgeRingEnabled;

// playerCharRegistry: every player-faction character is watched, not only the
// ones carrying a move order. A watched character's zone ranks tier 2, so this
// moves navmesh queue order and, through preload_queue.cpp's use of
// ComputeZonePriority, registration order, as well as coverage. Raises the watched capacity to 64, downgrades an
// arrived mover to a baseline entry instead of dropping it, and lets a full
// registry overwrite its oldest order-less entry. false = only order-carrying
// characters, capacity 32, dropped on arrival (the A/B control).
	bool playerCharRegistryEnabled;

// islandDeletedReissue: re-issue a player move order the engine deleted (not
// one the player cancelled). On by default.
	bool islandDeletedReissueEnabled;

	int cfg_k7PostDeathHold;

// k7DestReadyGate: before a deleted-order re-issue sends, wait for the
// destination cell's outdoor navmesh instance (ClassifyZoneReadiness,
// zone_readiness_classify.h) up to 15 s, so a re-issue is never sent into a
// navmesh gap and spends one of the shared MAX_REISSUES budget for nothing.
// On by default, and independent of k7PostDeathHold: it gates every deleted
// re-issue, held or not.
	bool k7DestReadyGateEnabled;

// k7ArrivalTrigger: when a tracked order's end signature fires far
// (>1,000 units) from its destination while that destination cell's outdoor
// navmesh instance is not yet in the world (or was, within the last few
// seconds), arm a wait and re-issue at once -- bypassing K7TryDeletedReissue's
// own STOPPED_HYSTERESIS wait -- the first poll the character is confirmed
// still stopped and the instance is in the world, up to 15 s. Every other K7
// gate (cooldown, budget, nudge, character-state, destination match, zone
// accessibility, k7PostDeathHold, k7DestReadyGate) still applies through the
// same ReissueOrder path. A plain boolean like every other key here: true
// (default) sends. false still arms and tracks the would-send time and still
// logs "K7 arrival (observe)" against the real K7 send when it lands -- it
// only sends nothing itself, so the instrument is never gated behind the
// lever (k7Arrive=w/x count regardless of this key; only k7Arrive=s does not).
	bool k7ArrivalTriggerEnabled;
};

extern MovementConfig g_movementCfg;
extern const MovementConfig kMovementDefaults;
extern const ConfigKey g_movementConfigKeys[];

} // namespace movement
