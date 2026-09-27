// config_values.h — the globals the INI keys write, their enums and bounds.
// No game or Windows header, so the host suites link the config table over them.

#ifndef KENSHI_ZONE_OPT_CONFIG_VALUES_H
#define KENSHI_ZONE_OPT_CONFIG_VALUES_H

#include "zone/geometry/zone_geometry_cert.h"   // ZoneGeometryMode, for zoneGeometryMode below


// =========================================================================
// Feature flags (runtime, default true)
// =========================================================================

extern bool deferralEnabled;
extern bool priorityBoostEnabled;
extern bool preloadEnabled;
extern bool movementAwareEnabled;
extern bool cachingEnabled;
extern bool groupCohesionEnabled;
extern bool pathfindDiagEnabled;
extern bool islandFixEnabled;            // islandFix: overlay answers the island hooks
// islandFarSpan: the island hook answers false for a vanilla same-island pair
// whose cells are this many or more apart (Chebyshev), so setDestination takes
// its edge-route branch instead of one direct path. 0 = vanilla answer. Read
// at startup only; independent of islandFix.
extern int  cfg_islandFarSpan;

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
extern bool islandEdgeRingEnabled;

// playerCharRegistry: every player-faction character is watched, not only the
// ones carrying a move order. A watched character's zone ranks tier 2, so this
// moves navmesh queue order and, through preload_queue.cpp's use of
// ComputeZonePriority, registration order, as well as coverage. Raises the watched capacity to 64, downgrades an
// arrived mover to a baseline entry instead of dropping it, and lets a full
// registry overwrite its oldest order-less entry. false = only order-carrying
// characters, capacity 32, dropped on arrival (the A/B control).
extern bool playerCharRegistryEnabled;

// reprioFast: reprioritize the navmesh job queue on the reprioritizeInterval
// cadence whether or not anything is tracked, and once more immediately when
// the player issues a move order. false = a 3 s backstop that runs only while
// a mover or a queued preload exists.
extern bool reprioFastEnabled;

// routeTier: a zone a mover with a move order stands in takes the top navmesh
// tier. Any watched mover's zone already ranks tier 2, so this is a 2 -> 1
// lift for the zones the camera grid does not already cover. It also orders
// mod registration, because preload_queue.cpp ranks its candidates
// with the same ComputeZonePriority.
// Read at one site, in ComputeZonePriority.
extern bool routeTierEnabled;

// pathExtractGuard: wrap the path-result extraction loop so a fault inside it
// rolls the result back to "no new edges" instead of taking the process down.
// Every rescue is counted, the first is logged. false = hook not installed.
extern bool pathExtractGuardEnabled;

// sectionStamp: hook every streaming-collection insertion and count it, so a
// session log says whether the section table is taking inserts at all.
// false = hook not installed.
extern bool sectionStampEnabled;

// navMeshUpdateGuard: wrap the navmesh/path thread's NavMesh::update pass in
// SEH. A fault inside it kills the thread while it still holds the
// SectionManager's changeMutex exclusively, and every navigability query
// try-locks that mutex and gives up the instant the try fails -- so no
// character can be ordered to move again for the rest of the session. The
// guard records that fault and the lock state, and leaves the lock held: the
// fault means the navmesh data is already damaged, and the lock is what keeps
// the next reader out of it. RE_Kenshi's crash handler then offers an
// emergency save and ends the process either way. It installs no hook; false
// makes the existing wrapper a plain pass-through.
extern bool navMeshUpdateGuardEnabled;

// destroyListDiag: install the pass-through hook on GameWorld::destroyListOE's
// sole inserter (the destroyListOE diagnostic, destroy_list_defer.h). It only records the calling
// thread, but it is still a 5-byte patch into a hot engine function, so PROD
// leaves it off and DEV turns it on. The invariant probe itself is always on in
// every build and is not gated by this key.
extern bool destroyListDiagEnabled;

// destroyListDefer: the destroyListOE mitigation (destroy_list_defer.h). Off-main-thread inserts
// into GameWorld::destroyListOE are queued and replayed on the main thread, so
// the unsynchronised container has a single writer. On by default in every
// build; setting it false leaves the hook installed as the plain diagnostic,
// which is the A/B control. The hook is installed when either this or
// destroyListDiag is on.
extern bool destroyListDeferEnabled;

// saveLoadUnload: the save-load crash fix (preload_saveload.cpp,
// hook_resetUnloadZones). At the game's save-load reset, after it unloads the
// zones in Set A and Set B, unload every zone still holding a content (the
// mod's, which are in neither set) and clear the mod's state there. On by
// default in every build, PROD included. false = today's behaviour for
// bisection: nothing unloaded and the state clear left to the ZM+8 edge; the
// survivors are still counted and logged ("would unload").
extern bool saveLoadUnloadEnabled;

// escapePauseGuard: keep the escape menu's pause through a loader unpause
// (zone_pause.cpp). On by default; false restores the vanilla clobber where a
// finishing transition can resume the game behind an open escape menu.
extern bool escapePauseGuardEnabled;

// townGuard: refuse a town's coverage refresh that carries a nonpositive
// timer (zone/handoff/zone_lifecycle_hooks.cpp, ZONEHAND_STEP >= 2). Such a refresh means
// no cell in the coverage is leased, and the engine answers it with a fixed
// default that renews the whole coverage from nothing. On by default; false
// restores the vanilla behaviour for an A/B.
extern bool townGuardEnabled;

// zoneRetention: hold a cell the game took over from the mod past its native
// expiry, for as long as the retention policy says (zone_retention.cpp,
// ZONEHAND_STEP >= 3). On by default; false leaves every expiry exactly as
// the engine decides it, which is the A/B control and the behaviour of a
// build one step below.
extern bool zoneRetentionEnabled;

// islandReadinessRule: per-caller readiness rule in hook_isContentPending.
// Off by default; read by hook_isContentPending (readiness_hook.cpp).
extern bool islandReadinessRuleEnabled;

// readinessOverrides: false turns off the isContentPending deferral's
// override. On by default; read by readiness_hook.cpp.
extern bool readinessOverridesEnabled;

// npcWaitDiag: NPC path-wait diagnostic. DEV default on, PROD default off;
// read by path_pool.cpp.
extern bool npcWaitDiagEnabled;

// gatePassDiag: per-pass gate-code timing diagnostic. DEV default on, PROD
// default off; decides the Gates__updateCodes install (hook_manifest.cpp) and the
// GateRate: line (path_pool_report.cpp).
extern bool gatePassDiagEnabled;

// pathCostLines: the hook_findPathFull record and its histograms/cap
// counters accumulate whenever findPathFull is hooked
// (pathfindDiagEnabled); this key controls only how much of that gets
// printed on the heartbeat -- the compact AstarCap:/PathBusy: lines always
// print, this key adds the per-class AstarClass: detail lines. On by
// default (astar_cost.cpp).
extern bool pathCostLinesEnabled;

// zoneLifeUnload: the mod unloads the zones it loaded once they are idle and
// outside the retain radius. On by default.
extern bool zoneLifeUnloadEnabled;
// islandDeletedReissue: re-issue a player move order the engine deleted (not
// one the player cancelled). On by default.
extern bool islandDeletedReissueEnabled;

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
extern int cfg_k7PostDeathHold;

// k7DestReadyGate: before a deleted-order re-issue sends, wait for the
// destination cell's outdoor navmesh instance (ClassifyZoneReadiness,
// zone_readiness_classify.h) up to 15 s, so a re-issue is never sent into a
// navmesh gap and spends one of the shared MAX_REISSUES budget for nothing.
// On by default, and independent of k7PostDeathHold: it gates every deleted
// re-issue, held or not.
extern bool k7DestReadyGateEnabled;

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
extern bool k7ArrivalTriggerEnabled;

// navmeshVanillaPruning: fresh navmesh work buffers carry the game's
// region-pruning and extra-vertex settings copied from the real one. On by
// default; false = Havok's defaults on fresh work buffers, under their own L2
// settings hash, for the A/B. Read-only after LoadConfig
// (NmVanillaPruningActive, nm_quality.h).
extern bool navmeshVanillaPruningEnabled;

// navmeshNeighbourSeeds: the getSeedPointsFromAdjacentZone hook, which
// records the seeds each neighbour's mesh gave, and the shipped-tile stand-in
// seeds (where the game found no usable neighbour mesh, the seeds that
// neighbour's shipped tile gives are appended; the L2 settings hash folds
// "nbrseed1"). On by default; false = no hook, no prefetch and the L2
// settings hash without the marker, for the A/B. Read-only after LoadConfig
// (NmNbrSeedHookWanted / NmNbrSeedStandInActive, nm_cache_core.h).
extern bool navmeshNeighbourSeedsEnabled;

// navmeshBuildLockNarrow: the collision builders take the
// game's build mutex around the regions that race instead of buildCollisionCS
// around each whole builder. On by default; false = the wide cover with the
// same stats, for the A/B. Read once, when the builder hooks install.
extern bool navmeshBuildLockNarrowEnabled;

// navmeshStallThrottle: when a run of non-blocking build-lock acquisitions is
// refused for long enough that no live holder can explain it, slow the engine's
// unbounded retry down. On by default; false leaves the retry at full rate, for
// the A/B. Read on every refused acquisition.
extern bool navmeshStallThrottleEnabled;

// clusterGraphBypass: what checkFaceConnectivity
// answers. Four positions, mutually exclusive, so there is no combination to
// get wrong:
//
//   CGB_BYPASS (true, the default) -- answer 1 without consulting the cluster
//     graph. The graph is stale and rejects reachable pairs, and walking it
//     reads a navmesh section slot that can be replaced underneath it.
//   CGB_ORIGINAL (false) -- call the original and obey it, so a pair the graph
//     rejects never reaches the A* search.
//   CGB_MEASURE ("measure") -- call the original, count its answer, and wave
//     the pair through anyway. Routing behaves as under CGB_BYPASS while each
//     rejected pair's search is labelled and its outcome recorded, so what the
//     bypass lets through is read inside one run instead of from a pair of
//     runs. It pays CGB_ORIGINAL's read cost: diagnostic, not a default.
//   CGB_PLAYER ("player") -- call the original, wave a rejected pair through
//     only when the request being served is player-owned, and obey the
//     rejection otherwise. The requester is read from the request the
//     direct-path check stashed for this thread a few instructions earlier;
//     a check with no request behind it -- the AI task system's reachability
//     query, which never runs that check -- counts as unattributed and is
//     never waved, so the unknown case keeps vanilla behaviour.
//
// CGB_ORIGINAL, CGB_MEASURE and CGB_PLAYER all consult the cluster graph, and
// so all three walk the section slot that CGB_BYPASS exists to avoid reading.
// graphPositionGuard covers the absent-instance fault on that walk; the two
// A* absent-instance guards sit on different sites and do not.
//
// Read on every connectivity check, from the contentStream and AI back threads.
enum ClusterGraphMode
{
	CGB_BYPASS = 0,
	CGB_ORIGINAL,
	CGB_MEASURE,
	CGB_PLAYER
};
extern int clusterGraphBypassMode;

// unstitchGuard: the bounds test the cross-section un-stitch never makes
// (src/fixes/stitch/unstitch_guard.h). On by default and present in every build: the
// fault it prevents is a measured, twice-reproduced crash, and with it on a
// teardown whose records are all in bounds runs the game's own walk untouched.
// Read when the guard installs, and never again.
extern bool unstitchGuardEnabled;

// stitchSourceLines: how many dropped sets the stitch-source instrument
// (src/fixes/stitch/stitch_source.h) describes in the log, two lines each. Verbosity
// only: the write-side record, the classification counters and the heartbeat
// exist in every build whatever this says. 0 keeps the counters and prints no
// per-drop lines. Read at startup.
extern int cfg_stitchSourceLines;

// graphVisitorGuard: the absent-instance test the A* heuristic never makes
// (src/fixes/search/graph_visitor_guard.h). On by default and present in every build:
// the fault it prevents is a measured mid-session crash, and with it on a
// search whose sections all have their graph instances runs the game's own
// code untouched. Read when the guard installs, and never again.
extern bool graphVisitorGuardEnabled;

// graphExpandGuard: the same absent-instance test one cache slot over, on the
// A* step that expands a popped node (src/fixes/search/graph_expand_guard.h). On by
// default and present in every build, for the reason above: the node-cost
// guard keeps such a node in the search, so the expansion is where it arrives
// next. Read when the guard installs, and never again.
extern bool graphExpandGuardEnabled;

// graphPositionGuard: the same absent-instance test on the cluster-graph
// search's own node-position helper (src/fixes/search/graph_position_guard.h) --
// reached only when clusterGraphBypass actually consults the graph (measure,
// player or off), a call path neither guard above sits on. The detour
// installs in every build regardless of this key; the key chooses only
// whether a call that meets an absent instance is substituted (true, the
// default) or merely classified and counted before the original runs
// unchanged (false, an observe mode that keeps the fault's own base rate
// visible in a control). Read when the guard installs, and never again.
extern bool graphPositionGuardEnabled;

// meshFaceGuard: the face-index and edge-run tests the per-face AABB step
// never makes (src/fixes/streaming/mesh_face_guard.h). On by default and present in
// every build: the fault it prevents is a measured crash recorded three times
// at one instruction, and with it on a face whose edge run lands inside its
// instance runs the game's own walk untouched. Read when the guard installs,
// and never again.
extern bool meshFaceGuardEnabled;

// navMeshLife: the section lifecycle rows at streaming-collection insert and
// removal (src/fixes/streaming/navmesh_life.h). On by default in DEV and PROD alike, and
// deliberately independent of every preload and pathfinding key: its counts
// are only worth anything when the same build can be run with those keys on
// and off and the two logs compared. Read at both sites.
extern bool navMeshLifeEnabled;

// createInstanceGuard: skips a NavMesh::createInstance call whose NavInstance
// is already queued in addList by pointer with a live instance
// (src/fixes/streaming/create_instance_guard.h); vanilla frees that object and queues
// the freed pointer. The detour installs in every build regardless of this
// key; the key chooses only whether such a call is skipped (true, the
// default) or counted and passed to the original unchanged (false, observe).
// Read when the guard installs, and never again.
extern bool createInstanceGuardEnabled;

// hullDoublePushGuard: drops a second queueing of one object on
// PhysicsInterface::hullsToDestroy before PhysicsActual::updateUT flushes it
// to the physics thread (src/fixes/physx/hull_queue_guard.h); vanilla deletes such
// an object twice, the second time through freed memory. The detour installs
// in every build regardless of this key; the key chooses only whether a
// duplicate is dropped (true, the default) or counted and left in place
// (false, observe). Read when the guard installs, and never again.
extern bool hullDoublePushGuardEnabled;

// stitchByteGuard: skips the one-byte `+0x50` store NavMeshGenerator::update
// makes on the output of an interior stitch task, which lands one byte past
// the 0x48-byte NavInstance on the next heap block
// (src/fixes/stitch/stitch_byte_guard.h). The patch installs in every build whenever
// its bytes verify and no path thread exists yet; the key chooses only
// whether that store is skipped (true, the default) or kept and recorded
// (false, observe). Read when the guard installs, and never again.
extern bool stitchByteGuardEnabled;

// navmeshAdjExclusion: a navmesh job is not claimed while
// a job whose stitches touch the same objects is in flight or published but
// not yet drained (nm_adjacency.h). The registry, the drain observer and the
// checker run whatever this key says; false only counts (adjWould=). Default
// true. Read when the observer installs, and never again.
extern bool navmeshAdjExclusionEnabled;

// playerRepathTier: the game's own mid-walk re-request for a player character
// calls requestPath at priority 0, the same as an NPC's, so it queues at the
// NPC tier (10) instead of the mod's player tier (45) that its original order
// got. The publish-and-match in player_repath_tier.h runs and counts either
// way; this key chooses only whether a match's tier is actually written
// (true, the default) or just counted (false, observe; playerRepath= in the
// Phase12 heartbeat carries seen/would/tiered/set every time). Read on every
// match.
extern bool playerRepathTierEnabled;

#ifdef ZONEOPT_DEBUG
// unstitchProbe: the read-only detour on NavMesh::deleteInstance
// (src/fixes/stitch/unstitch_probe.h). Off by default and DEV only -- the flag, its
// INI key and the detour are all compiled out of a PROD build, so a PROD
// session cannot install it. Read when the probe installs and on every
// teardown; it decides only whether the probe looks, never what the game does.
extern bool unstitchProbeEnabled;

// sectionKeyProbe: the read-only capture of the section-table lookups the
// world step makes from a packed key (src/fixes/streaming/section_key_probe.h). On by
// default and DEV only -- the flag, its INI key and both detours are compiled
// out of a PROD build. On rather than off because what it records is only
// worth anything if it is already running when a fault arrives, and a session
// nobody remembered to arm records nothing. Read when the probe installs, and
// never again.
extern bool sectionKeyProbeEnabled;
#endif

// settingsPanel: the ZoneOpt tab in the game's Options window (src/gui/).
// On by default; read at startup only.
extern bool settingsPanelEnabled;

// navmeshMissHash: log a MissHash: line for every generated MISS.
// DEV default on, PROD default off; read by
// nm_misspar.cpp.
extern bool navmeshMissHashEnabled;

// zoneCycleStats: at ZONEHAND_STEP >= 1, the per-loading-cycle measurement
// (ZoneCycle: / ZoneCycleSum:), the private-lease sample (ZonePriv:) and the
// ZoneLeak: setBsz= token. DEV default on, PROD default off; false makes the
// measurement a no-op. Read on every sample.
extern bool zoneCycleStatsEnabled;

// zoneWedgeGuard: at ZONEHAND_STEP >= 1, the one-shot ZoneWedge: report when
// a loading cycle dwells in one loadingPhase past ZC_PHASE_WEDGE_THRESHOLD_MS
// (a permanent hang, not a slow load). On by default in DEV and PROD alike,
// independent of zoneCycleStats: it fires only in an already-broken state and
// a beta user's report is worthless if the build that hit it had it off.
// Read on every sample.
extern bool zoneWedgeGuardEnabled;

// zoneGeometryMode: which geometry contract a prepared cell is adopted
// under. `contentOnly` is the only accepted value: it asserts nothing about
// any mesh, and the certificate machinery at the L1 store point counts its
// verdicts without acting on them. The other mode needs verified coverage of
// the engine's own geometry producers and is fenced at compile time as well
// as refused here (zone_geometry_cert.h). Read at the store point and on the
// periodic report.
extern ZoneGeometryMode zoneGeometryMode;

// navmeshMissSplit: a worker clone's MISS releases
// processJobCS around realGenerate. On by default; false = every populate
// runs under the lock, with the same counters, for the A/B. Read by the
// populate hook on every call.
extern bool navmeshMissSplitEnabled;

// navmeshMissSplitBg: the same release for a MISS that
// runs on the real generator with a fresh work buffer swapped in (the
// background thread's own, and a worker whose clone failed). Needs
// navmeshMissSplit as well. False = that path stays under the lock, for the
// A/B.
extern bool navmeshMissSplitBgEnabled;

// physPurecallRecord: a forensic-only recorder on PhysXCore64.dll's own
// pure-virtual-call handler slot (src/fixes/physx/purecall_record.h). A pure
// virtual call there is a CRT abort, not an SEH exception -- the mod's crash
// handler cannot see it -- so this instead captures the faulting thread, the
// real call site and a timestamp into purecall_dump.txt just before abort()
// runs. On by default in every build; it writes nothing (data write, not a
// hook) unless PhysXCore64.dll's own bytes match this build's signature.
extern bool physPurecallRecordEnabled;

// physQueryGuard: the point-of-use guard on the result walk inside
// GameWorld::getObjectsWithinBox (src/fixes/physx/physx_query_guard.h). The scene's
// overlap query can hand back a shape whose block is already free; calling
// getActor() through it lands in PhysX's _purecall, which aborts the process
// with no record. The guard validates each entry immediately before that call
// and skips a bad one. On by default in every build; false skips the install
// entirely and leaves vanilla behaviour. Read once, at startup.
extern bool physQueryGuardEnabled;

// corpsePin: ActivePlatoon::calculateCurrentPos detour (src/fixes/world/corpse_pin.h).
// A squad whose only member is a carried NPC corpse otherwise stays pinned at
// the pickup spot forever; this repositions
// it to the carrier instead. On by default; false skips the install
// entirely, a true no-op (the build gate still verifies the site's row at
// startup either way, like every other row). Read once, at hook install.
extern bool corpsePinEnabled;

// nestValidationGuard: at ZONEHAND_STEP >= 2, the finalizeZoneResources
// detour (src/fixes/world/nest_validation.h). On by default; false is a true
// no-op -- the original runs exactly as before the guard existed. Read on
// every call.
extern bool nestValidationGuardEnabled;


// Capacity, not the live count: it sizes the worker handle arrays and the L2
// in-flight table, and bounds what navmeshWorkerCount may be set to. The number
// of workers actually created is g_navMeshWorkerCount (default: half the
// logical CPUs, via FinalizeConfig).
const int NAVMESH_WORKER_COUNT = 6;


// =========================================================================
// Runtime-tunable parameters (cfg_ prefix, loaded from INI)
// =========================================================================

// Zone loading
// loadSingleZone's 4th argument (xmm3) for the zones the mod preloads. The game
// writes it to zoneEntry + 4*(timerIndex + 48) and it decides when that zone is
// unloaded again. 0 takes the game's own per-timer default, which is what
// processState2 passes; a positive value overrides it. Exists so this can be
// A/B'd (0 vs 3600) without a rebuild.
extern float  cfg_preloadKeepAliveSeconds;

// Camera focus (camera_focus.h/camera_zone_hook.cpp): use the camera's orbit/follow
// anchor instead of its own position for zone prediction, so a zoomed-out,
// rearward-pitched camera still preloads the zone the squad is entering.
extern bool   cfg_camFocusEnabled;       // false reverts to the raw camera position
extern float  cfg_camFocusMaxDist;       // soft cap from the nearest squad member; <= 0 = one zone width
extern float  cfg_camFocusHardMult;      // hard reject beyond maxDist * this (no clamp, just fallback)
// Hysteresis band (world units) for the prediction axis check in camera_zone_hook.cpp:
// once a neighbour is predicted on an axis, that axis must fall back below
// PRELOAD_THRESHOLD - this before releasing it; a new neighbour needs
// PRELOAD_THRESHOLD + this to commit. Default 250 = 10% of the 2500-unit
// threshold: enough to absorb ordinary jitter at the border without
// meaningfully delaying a real crossing (zones are ~8192 units wide).
extern float  cfg_camFocusHysteresis;

// NavMesh workers
extern int    cfg_navmeshWorkerCount;    // INI value; 0 = automatic (half the logical CPUs)
// The clamped worker count, i.e. how many threads CreateNavMeshWorkers starts
// and how many the priority boost/restore loops walk. Set once from
// cfg_navmeshWorkerCount after clamping.
extern int    g_navMeshWorkerCount;
// How many realGenerate calls may run at once with processJobCS released.
// 0 = automatic (two, fewer on small CPUs), 1 = one at a
// time, clamped to 0..4. Read once at startup.
extern int    cfg_navmeshGenConcurrency;

// NavMesh L2 disk cache
extern int    cfg_navmeshDiskCacheMaxMB; // navmesh_cache\ size cap in MB; past it the
                                         // oldest files are deleted down to 75% of the cap

// FNV-1a over the game's active mod list (mods.cfg), read once on the main
// thread in LoadConfig. Part of every L2 disk cache filename: a changed mod set
// can move terrain and buildings, so its meshes must not be reused.
// 0 means "mod list unavailable".
extern unsigned int g_modSetHash;

// Hook orchestration
extern double cfg_camLogInterval;        // debug camera log interval
extern double cfg_reprioritizeInterval;  // seconds between navmesh queue reprioritization

// Zone lifecycle
extern int    cfg_zoneLifeRetainRadius;  // Chebyshev radius (zones) around camera/player chars
                                         // inside which a mod-loaded zone is never unloaded (1-4).
                                         // It also sets the radius of the shared proximity map,
                                         // which retention reads as its cheap filter, so raising
                                         // it makes adopted cells stickier as well.
extern double cfg_zoneLifeIdleSeconds;   // seconds a mod-loaded zone must sit outside the
                                         // retain radius before it is unloaded (5-600)


#endif // KENSHI_ZONE_OPT_CONFIG_VALUES_H
