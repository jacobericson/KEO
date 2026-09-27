// config.h — Feature flags, tuning parameters, shared structs (Layer 2)
// Depends on: core.h, game.h

#ifndef KENSHI_ZONE_OPT_CONFIG_H
#define KENSHI_ZONE_OPT_CONFIG_H

#include "base/core.h"
#include "game/game.h"
#include "base/ini_text.h"   // INI line rules, ParseBool, ParseFloat

// Zone handoff staging gate. 1 = the loading-cycle, Set B and private-lease
// measurement (ZoneCycle:, ZoneCycleSum:, ZonePriv: and the ZoneLeak: setBsz=
// token); it reads state and changes no behaviour. 2 = the handoff itself:
// preparation and cohort adoption (zone_handoff.cpp) publish a per-cell
// class (private/adopted) into the ledger, and hook_isContentPending's
// readiness contract (zone_readiness_contract.h) consults that class
// instead of islandReadinessRule -- private and adopted cells hear the
// original, every other cell keeps today's sections==0 bypass -- plus the
// global game-owned bypass (ZM+8) read fresh at the same call site. 3 is
// the retention hold that keeps an admitted cell from being evicted on its
// first native pass (zone_retention.h).
// build_opt_step4.bat and build_opt.bat set 3.
// Temporary gate — removed after validation.
#ifndef ZONEHAND_STEP
  #define ZONEHAND_STEP 0
#endif
// 2 also carries the nest-validation guard (SectionManager::finalizeZoneResources,
// src/fixes/world/nest_validation.h): it refuses the native nest destroy when the
// cell's mesh is not in yet.


#include "base/config_values.h"   // the INI-loaded globals


// =========================================================================
// NavMesh worker pool constants
// =========================================================================

const int WORKBUF_SIZE = 65536;  // workBuffer clone size (runtime probe: allocSize=65536)


// =========================================================================
// Compile-time hard limits (array sizing upper bounds)
// =========================================================================

const int MAX_FORMATION_MEMBERS_LIMIT = 64;  // hard cap for embedded FormationMember arrays




// =========================================================================
// Preloading enqueue order constants
// =========================================================================

const int OWNER_CAMERA    = 0;
const int OWNER_CHARACTER = 1;

// Preload order: center first, then cardinals, then diagonals
const int ORDER_DX[9] = { 0, -1, 1, 0, 0, -1, 1, -1, 1 };
const int ORDER_DY[9] = { 0, 0, 0, -1, 1, -1, -1, 1, 1 };

// 2x2 grid matching the pause mechanism's positive-offset pattern
const int SMALL_DX[4] = { 0, 1, 0, 1 };
const int SMALL_DY[4] = { 0, 0, 1, 1 };


// =========================================================================
// Shared struct definitions (used by multiple modules)
// =========================================================================

// PreloadedZone and QueuedZone structs moved to preload.h

// WatchedCharacter struct moved to tracking.h

// Formation structs (FormationMember, FormationGroup) moved to formation.h

// PathProbeEntry and PATH_PROBE_SIZE moved to pathfind_diag.h


// NavMesh cache
void ClearNavMeshCache();
void InitNavMeshCacheCS();
void LogNavMeshCacheStats(double now);


// =========================================================================
// Config loading
// =========================================================================

void LoadConfig(const std::string& dllDir);
void FinalizeConfig();


#endif // KENSHI_ZONE_OPT_CONFIG_H
