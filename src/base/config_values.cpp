// config_values.cpp — the INI-loaded globals at their compiled-in defaults.

#include "base/config_values.h"


// =========================================================================
// Feature flags
// =========================================================================

bool deferralEnabled       = true;
bool priorityBoostEnabled  = true;
bool preloadEnabled        = true;
bool movementAwareEnabled  = true;
bool cachingEnabled        = true;
bool groupCohesionEnabled  = true;
bool pathfindDiagEnabled   = true;
bool islandFixEnabled      = true;   // islandFix=false keeps the island hooks passing through (A/B control)
int  cfg_islandFarSpan       = 2;      // islandFarSpan: same-island answers this many cells apart read false (0 = off)
bool islandEdgeRingEnabled   = false; // islandEdgeRing: see config_values.h; false computes and counts, never mutates the list
bool playerCharRegistryEnabled = true; // playerCharRegistry: watch the whole player faction (config_values.h)
bool reprioFastEnabled     = true;   // reprioFast: 1s ungated backstop + reprio on a move order (config_values.h)
bool routeTierEnabled      = true;   // routeTier: order-carrying mover's zone takes the top tier (config_values.h)
bool pathExtractGuardEnabled = true; // pathExtractGuard: SEH around path-result extraction (config_values.h)
bool sectionStampEnabled   = true;   // sectionStamp: count streaming-collection inserts (config_values.h)
bool navMeshUpdateGuardEnabled = true; // navMeshUpdateGuard: SEH classifier around NavMesh::update (config_values.h)
#ifdef ZONEOPT_DEBUG
bool destroyListDiagEnabled = true;  // destroyListDiag: inserter thread-id hook on in DEV,
#else
bool destroyListDiagEnabled = false; //   off in PROD (see config_values.h).
#endif
bool destroyListDeferEnabled = true; // destroyListDefer: destroyListOE mitigation, on everywhere (config_values.h).
bool saveLoadUnloadEnabled = true;   // saveLoadUnload: save-load crash fix, on everywhere (config_values.h).
bool escapePauseGuardEnabled = true; // escapePauseGuard: keep the escape menu paused through a loader unpause (config_values.h).
bool townGuardEnabled = true;        // townGuard: refuse a lease-less town coverage refresh (config_values.h).
bool zoneRetentionEnabled = true;    // zoneRetention: hold an adopted cell past its native expiry (config_values.h).
bool islandReadinessRuleEnabled = false; // islandReadinessRule: per-caller readiness A/B key (config_values.h).
bool readinessOverridesEnabled = true;   // readinessOverrides: isContentPending deferral override (config_values.h).
#ifdef ZONEOPT_DEBUG
bool npcWaitDiagEnabled = true;   // npcWaitDiag: NPC path-wait diagnostic, on in DEV,
#else
bool npcWaitDiagEnabled = false;  //   off in PROD (see config_values.h).
#endif
#ifdef ZONEOPT_DEBUG
bool gatePassDiagEnabled = true;  // gatePassDiag: per-pass gate-code timing diagnostic, on in DEV,
#else
bool gatePassDiagEnabled = false; //   off in PROD (see config_values.h).
#endif
bool pathCostLinesEnabled = true;        // pathCostLines: A* cost logging detail (config_values.h).
bool zoneLifeUnloadEnabled = true;       // zoneLifeUnload: idle zone unload (config_values.h).
bool islandDeletedReissueEnabled = true; // islandDeletedReissue: deleted-order re-issue (config_values.h).
int  cfg_k7PostDeathHold    = K7_HOLD_ON; // k7PostDeathHold: hold a post-death order swap (config_values.h).
bool k7DestReadyGateEnabled = true;       // k7DestReadyGate: wait for destination navmesh before re-issue (config_values.h).
bool k7ArrivalTriggerEnabled = true;      // k7ArrivalTrigger: re-issue at once on destination arrival (config_values.h).
bool navmeshVanillaPruningEnabled = true; // navmeshVanillaPruning: copy the game's pruning settings (config_values.h).
bool navmeshNeighbourSeedsEnabled = true; // navmeshNeighbourSeeds: neighbour-mesh seed capture (config_values.h).
bool navmeshBuildLockNarrowEnabled = true; // navmeshBuildLockNarrow (config_values.h).
bool navmeshStallThrottleEnabled = true;   // navmeshStallThrottle (config_values.h).
int  clusterGraphBypassMode = CGB_BYPASS;  // clusterGraphBypass: connectivity pre-check (config_values.h).
bool unstitchGuardEnabled = true;          // unstitchGuard: un-stitch bounds guard (config_values.h).
int  cfg_stitchSourceLines = 32;            // stitchSourceLines: per-drop line cap (config_values.h).
bool graphVisitorGuardEnabled = true;      // graphVisitorGuard: A* absent-instance guard (config_values.h).
bool graphExpandGuardEnabled = true;       // graphExpandGuard: A* expand-step absent-instance guard (config_values.h).
bool graphPositionGuardEnabled = true;     // graphPositionGuard: cluster-graph node-position absent-instance guard (config_values.h).
bool meshFaceGuardEnabled = true;          // meshFaceGuard: per-face bounds guard (config_values.h).
bool createInstanceGuardEnabled = true;    // createInstanceGuard: self-duplicate createInstance guard (config_values.h).
bool hullDoublePushGuardEnabled = true;    // hullDoublePushGuard: hull destroy-queue duplicate guard (config_values.h).
bool stitchByteGuardEnabled = true;        // stitchByteGuard: interior stitch +0x50 store guard (config_values.h).
bool navmeshAdjExclusionEnabled = true;    // navmeshAdjExclusion: defer adjacent navmesh jobs (config_values.h).
bool playerRepathTierEnabled = true;       // playerRepathTier: tier a player's mid-walk re-request (config_values.h).
bool navMeshLifeEnabled = true;            // navMeshLife: section lifecycle rows (config_values.h).
#ifdef ZONEOPT_DEBUG
bool unstitchProbeEnabled = false;         // unstitchProbe: DEV-only read-only probe (config_values.h).
bool sectionKeyProbeEnabled = true;        // sectionKeyProbe: DEV-only section-key capture (config_values.h).
#endif
bool settingsPanelEnabled = true;         // settingsPanel (config_values.h).
#ifdef ZONEOPT_DEBUG
bool navmeshMissHashEnabled = true;   // navmeshMissHash: per-MISS hash diagnostic, on in DEV,
#else
bool navmeshMissHashEnabled = false;  //   off in PROD (see config_values.h).
#endif
bool navmeshMissSplitEnabled = true;  // navmeshMissSplit (config_values.h).
bool navmeshMissSplitBgEnabled = true; // navmeshMissSplitBg (config_values.h).
bool physPurecallRecordEnabled = true; // physPurecallRecord: PhysX purecall forensic recorder (config_values.h).
bool physQueryGuardEnabled = true;    // physQueryGuard: PhysX query result-walk guard (config_values.h).
#ifdef ZONEOPT_DEBUG
bool zoneCycleStatsEnabled = true;    // zoneCycleStats: ZONEHAND_STEP 1 measurement, on in DEV,
#else
bool zoneCycleStatsEnabled = false;   //   off in PROD (see config_values.h).
#endif
bool zoneWedgeGuardEnabled = true;    // zoneWedgeGuard: on in DEV and PROD (config_values.h).
bool corpsePinEnabled = true;         // corpsePin: on in DEV and PROD (config_values.h).
bool nestValidationGuardEnabled = true; // nestValidationGuard: on in DEV and PROD (config_values.h).
ZoneGeometryMode zoneGeometryMode = ZONE_GEOMETRY_CONTENT_ONLY;  // zoneGeometryMode (config_values.h).


// =========================================================================
// Runtime-tunable parameters (defaults match original compile-time constants)
// =========================================================================

// Zone loading
float  cfg_preloadKeepAliveSeconds = 0.0f;

bool   cfg_camFocusEnabled       = true;
float  cfg_camFocusMaxDist       = 0.0f;   // <= 0 = one zone width (grid.cpp's zoneStepX)
float  cfg_camFocusHardMult      = 3.0f;
float  cfg_camFocusHysteresis    = 250.0f;  // 10% of PRELOAD_THRESHOLD (2500)

// NavMesh workers
int    cfg_navmeshWorkerCount    = 0;   // 0 = automatic; resolved by FinalizeConfig
int    g_navMeshWorkerCount      = 0;   // set for real in FinalizeConfig
int    cfg_navmeshGenConcurrency = 0;   // 0 = automatic; resolved by MissParInit

// NavMesh L2 disk cache
int    cfg_navmeshDiskCacheMaxMB = 512;
unsigned int g_modSetHash        = 0;

// Hook orchestration
double cfg_camLogInterval        = 10.0;
double cfg_reprioritizeInterval  = 1.0;  // flag-then-backstop cadence

// Zone lifecycle
int    cfg_zoneLifeRetainRadius  = 2;
double cfg_zoneLifeIdleSeconds   = 30.0;
