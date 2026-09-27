// config_keys.cpp — the core module's rows (the keys LoadConfig reads, in its
// order), the retired keys, the custom parsers and the module list.

#include "base/config_table.h"
#include "base/config_values.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "bench/bench_lever_ab.h"
#include "bench/bench_sweep.h"
#include <limits.h>
#include <sstream>
#include <string.h>

// ---- Custom parsers ------------------------------------------------------

static bool ParseIslandEdgeRing(const std::string& val, ConfigLogFn log)
{
	bool b;
	bool isObserve = _stricmp(val.c_str(), "observe") == 0;
	bool isOff = _stricmp(val.c_str(), "off") == 0;
	bool isOn = _stricmp(val.c_str(), "on") == 0;
	if (isObserve || isOff || isOn) {
		b = isOn;
		log("Config: islandEdgeRing=" + val + " is a retired value; read as " + (b ? "true" : "false"));
		islandEdgeRingEnabled = b;
		return true;
	}
	if (ParseBool(val, &b)) { islandEdgeRingEnabled = b; return true; }
	return false;
}

static bool ParseK7PostDeathHold(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (ParseBoolOr(val, "observe", &b, &third)) {
		cfg_k7PostDeathHold = third ? K7_HOLD_OBSERVE : (b ? K7_HOLD_ON : K7_HOLD_OFF);
		return true;
	}
	return false;
}

static bool ParseClusterGraphBypass(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b = true, third = false;
	if (ParseBoolOr(val, "player", &b, &third) && third) {
		clusterGraphBypassMode = CGB_PLAYER;
		return true;
	}
	if (ParseBoolOr(val, "measure", &b, &third)) {
		clusterGraphBypassMode = third ? CGB_MEASURE : (b ? CGB_BYPASS : CGB_ORIGINAL);
		return true;
	}
	return false;
}

// A refused value is answered, not reported as an unknown key.
static bool ParseZoneGeometryMode(const std::string& val, ConfigLogFn log)
{
	ZoneGeometryMode m;
	if (ZoneGeometryModeFromName(val.c_str(), &m))
		zoneGeometryMode = m;
	else
		log("Config: zoneGeometryMode '" + val + "' refused; only contentOnly is available");
	return true;
}

static bool ParseBenchLevers(const std::string& val, ConfigLogFn log)
{
	std::vector<std::string> unknownLevers;
	bool leversFallback = false;
	ParseBenchLeversKey("bench.levers", val, &unknownLevers, &leversFallback);
	for (size_t i = 0; i < unknownLevers.size(); ++i)
		log("Bench: unknown lever '" + unknownLevers[i] + "' in bench.levers, ignored");
	if (leversFallback)
		log("Bench: bench.levers named no known lever, using every lever");
	return true;
}

static bool ParseBenchSweep(const std::string& val, ConfigLogFn log)
{
	std::vector<std::string> badLegs, extraLegs;
	bool sweepDefault = false;
	ParseBenchSweepKey("bench.sweep", val, &badLegs, &extraLegs, &sweepDefault);
	for (size_t i = 0; i < badLegs.size(); ++i)
		log("Bench: bench.sweep entry '" + badLegs[i] + "' ignored (a slot:1 or slot:20 leg is expected)");
	for (size_t i = 0; i < extraLegs.size(); ++i)
	{
		std::ostringstream ss;
		ss << "Bench: bench.sweep entry '" << extraLegs[i] << "' ignored (past the " << BENCH_SWEEP_MAX_LEGS
		   << "-leg limit)";
		log(ss.str());
	}
	if (sweepDefault)
		log("Bench: bench.sweep named no valid leg, using the default " + BenchSweepListText());
	return true;
}

// ---- The core table ------------------------------------------------------
//
// Every macro sets every column: minInt's "none" is INT_MIN, not zero. A row
// with lo > hi has no clamp. The defaults are the initialisers' values as the
// INI writes them, DEV first.

#define CFG_ROW(n, kind, size, lo, hi, tgt, minI, posOnly, doc, dev, prod, fn, label, tip, diag, sLo, sExp, ch, chN) \
	{ n, kind, 0, size, lo, hi, false, label, tip, diag, sLo, sExp, \
	  tgt, minI, posOnly, doc, false, dev, prod, fn, ch, chN }
#define CFG_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define CFG_BOOL(n, g, doc, dev, prod, diag, label, tip) \
	CFG_ROW(n, CK_BOOL, 0, 1.0f, 0.0f, &g, INT_MIN, false, doc, dev, prod, NULL, label, tip, diag, 0.0f, 0, NULL, 0)
// An integer row with a range drags in whole steps from its clamp minimum.
#define CFG_INT(n, g, lo, hi, minI, doc, dev, prod, diag, label, tip) \
	CFG_ROW(n, CK_INT, 0, lo, hi, &g, minI, false, doc, dev, prod, NULL, label, tip, diag, lo, 0, NULL, 0)
#define CFG_INT_CHOICES(n, g, lo, hi, minI, doc, dev, prod, diag, label, tip, ch) \
	CFG_ROW(n, CK_INT, 0, lo, hi, &g, minI, false, doc, dev, prod, NULL, label, tip, diag, lo, 0, ch, CFG_COUNT(ch))
#define CFG_FLOAT(n, g, lo, hi, posOnly, doc, dev, prod, diag, label, tip, sLo, sExp) \
	CFG_ROW(n, CK_FLOAT, 0, lo, hi, &g, INT_MIN, posOnly, doc, dev, prod, NULL, label, tip, diag, sLo, sExp, NULL, 0)
#define CFG_DOUBLE(n, g, lo, hi, doc, dev, prod, diag, label, tip, sLo, sExp) \
	CFG_ROW(n, CK_DOUBLE, 0, lo, hi, &g, INT_MIN, false, doc, dev, prod, NULL, label, tip, diag, sLo, sExp, NULL, 0)
// A custom row without choices stays INI-only.
#define CFG_CUSTOM(n, tgt, width, fn, doc, dev, prod) \
	CFG_ROW(n, CK_CUSTOM, width, 1.0f, 0.0f, tgt, INT_MIN, false, doc, dev, prod, fn, NULL, NULL, false, 0.0f, 0, NULL, 0)
#define CFG_CUSTOM_CHOICES(n, tgt, width, fn, doc, dev, prod, label, tip, ch) \
	CFG_ROW(n, CK_CUSTOM, width, 1.0f, 0.0f, tgt, INT_MIN, false, doc, dev, prod, fn, label, tip, false, 0.0f, 0, ch, CFG_COUNT(ch))
#define CFG_RETIRED(n) \
	{ n, CK_CUSTOM, 0, 0, 1.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, \
	  NULL, INT_MIN, false, false, true, NULL, NULL, NULL, NULL, 0 }

#define DOC  true
#define NDOC false

// devOnly: DIAG rows only read, count, record or log, and show in DEV builds only.
#define SHOW false
#define DIAG true

// The drop boxes' values: the INI text, the value it parses to, the name shown.
static const ConfigChoice kWorkerChoices[] =
{
	{ "0", 0, "Auto" }, { "1", 1, "1" }, { "2", 2, "2" }, { "3", 3, "3" }, { "4", 4, "4" }, { "5", 5, "5" }, { "6", 6, "6" }
};
static_assert(sizeof(kWorkerChoices) / sizeof(kWorkerChoices[0]) == NAVMESH_WORKER_COUNT + 1,
              "a worker choice per count up to NAVMESH_WORKER_COUNT, and Auto");

static const ConfigChoice kIslandEdgeRingChoices[] =
{
	{ "false", 0, "false" }, { "true", 1, "true" }
};

static const ConfigChoice kK7HoldChoices[] =
{
	{ "false", K7_HOLD_OFF, "off" }, { "observe", K7_HOLD_OBSERVE, "observe" }, { "true", K7_HOLD_ON, "on" }
};

static const ConfigChoice kClusterGraphChoices[] =
{
	{ "false", CGB_ORIGINAL, "false" }, { "true", CGB_BYPASS, "true" }, { "player", CGB_PLAYER, "player" },
	{ "measure", CGB_MEASURE, "measure" }
};

static const ConfigKey kCoreKeys[] =
{
	CFG_BOOL("deferral",              deferralEnabled,              NDOC, "true", "true", SHOW,
	  "Navmesh readiness deferral",
	  "Lets a zone count as ready while its navmesh work is still pending, once its content sections"
	  " have drained."),
	CFG_BOOL("priorityBoost",         priorityBoostEnabled,         NDOC, "true", "true", SHOW,
	  "Navmesh thread priority boost",
	  "Raises the priority of the game's navmesh thread while a zone transition loads."),
	CFG_BOOL("preload",               preloadEnabled,               NDOC, "true", "true", SHOW,
	  "Zone preloading",
	  "Loads the zones around the camera and the player's characters before they are needed."),
	CFG_BOOL("movementAware",         movementAwareEnabled,         NDOC, "true", "true", SHOW,
	  "Movement-aware preloading",
	  "Watches player move orders and preloads the zones toward each destination."),
	CFG_BOOL("caching",               cachingEnabled,               NDOC, "true", "true", SHOW,
	  "Navmesh cache",
	  "Keeps generated navmesh tiles in memory and on disk and reuses them instead of generating them"
	  " again."),
	CFG_BOOL("groupCohesion",         groupCohesionEnabled,         NDOC, "true", "true", SHOW,
	  "Squad cohesion",
	  "Keeps a squad given one move order walking together instead of scattering."),
	CFG_BOOL("pathfindDiag",          pathfindDiagEnabled,          NDOC, "true", "true", SHOW,
	  "Pathfinding hooks",
	  "Installs the mod's pathfinding hooks. The cluster graph bypass, the path request priority tiers,"
	  " the longer A* search for player orders, the path extraction guard and the path log lines all run"
	  " through them; off turns every one of them off."),
	CFG_BOOL("islandFix",             islandFixEnabled,             NDOC, "true", "true", SHOW,
	  "Island routing overlay",
	  "Lets the mod's island overlay answer the engine's island checks."),
	CFG_INT("islandFarSpan",          cfg_islandFarSpan, 0.0f, 8.0f, INT_MIN, DOC, "2", "2", SHOW,
	  "Far order island span in cells",
	  "Treats two cells this many or more apart as different islands, so a far move order walks leg by"
	  " leg instead of one path that can end short. 0 keeps the game's answer."),
	CFG_CUSTOM_CHOICES("islandEdgeRing",      &islandEdgeRingEnabled, sizeof(bool), ParseIslandEdgeRing, DOC, "false", "false",
	  "Edge route leg ring",
	  "Narrows the island list the engine's edge-route leg finder sees to the cells around the"
	  " character, so a leg is at most about two cells. false only counts what it would remove.", kIslandEdgeRingChoices),
	CFG_BOOL("playerCharRegistry",    playerCharRegistryEnabled,    DOC,  "true", "true", SHOW,
	  "Watch every player character",
	  "Watches every player-faction character, not only those with a move order, so the zone each"
	  " stands in keeps its place in the navmesh queue and in what loads first."),
	CFG_BOOL("reprioFast",            reprioFastEnabled,            DOC,  "true", "true", SHOW,
	  "Fast navmesh queue reordering",
	  "Reorders the navmesh job queue on the interval below and again the moment a move order is"
	  " issued, instead of a 3 second backstop."),
	CFG_BOOL("routeTier",             routeTierEnabled,             DOC,  "true", "true", SHOW,
	  "Move order zone first",
	  "The zone a character with a move order stands in is generated before the zones around the"
	  " camera."),
	CFG_BOOL("pathExtractGuard",      pathExtractGuardEnabled,      DOC,  "true", "true", SHOW,
	  "Path extraction guard",
	  "A fault while reading a path result rolls it back to no new edges instead of crashing; the"
	  " character re-paths on the next tick."),
	CFG_BOOL("sectionStamp",          sectionStampEnabled,          DOC,  "true", "true", DIAG,
	  "Navmesh section insert counter",
	  "Counts every instance the navmesh streaming collection takes, on the PathGuard: log line."),
	CFG_BOOL("navMeshUpdateGuard",    navMeshUpdateGuardEnabled,    DOC,  "true", "true", SHOW,
	  "Navmesh update fault recorder",
	  "Records a fault in the navmesh thread's update pass to navmesh_guard.txt and leaves the navmesh"
	  " lock held, so the damaged data is never read."),
	CFG_BOOL("destroyListDiag",       destroyListDiagEnabled,       NDOC, "true", "false", DIAG,
	  "Destroy list thread recorder",
	  "Records the calling thread of each insert into the game's destroy list. Changes nothing the game"
	  " does."),
	CFG_BOOL("destroyListDefer",      destroyListDeferEnabled,      NDOC, "true", "true", SHOW,
	  "Destroy list deferral",
	  "Queues destroy list inserts made off the main thread and replays them on the main thread, so the"
	  " list has a single writer."),
	CFG_BOOL("saveLoadUnload",        saveLoadUnloadEnabled,        NDOC, "true", "true", SHOW,
	  "Unload mod zones at save load",
	  "At a save load's reset, unloads every zone the mod still holds and clears its state. Off only"
	  " counts them."),
	CFG_BOOL("escapePauseGuard",      escapePauseGuardEnabled,      DOC,  "true", "true", SHOW,
	  "Keep the escape menu's pause",
	  "Keeps the escape menu's pause when a zone load finishes behind it. Off also turns off the zone"
	  " cycle measurement and the wedge report."),
	CFG_BOOL("townGuard",             townGuardEnabled,             DOC,  "true", "true", SHOW,
	  "Town coverage guard",
	  "Refuses a town's coverage refresh that carries a nonpositive timer, the engine's own signal that"
	  " nothing in that coverage is leased."),
	CFG_BOOL("zoneRetention",         zoneRetentionEnabled,         DOC,  "true", "true", SHOW,
	  "Zone retention",
	  "Holds a cell the game has taken over from the mod past its countdowns, for as long as the"
	  " retention policy says."),
	CFG_BOOL("islandReadinessRule",   islandReadinessRuleEnabled,   NDOC, "false", "false", SHOW,
	  "Per-caller readiness rule",
	  "Answers the content readiness check by the caller's class instead of one answer for every"
	  " caller."),
	CFG_BOOL("readinessOverrides",    readinessOverridesEnabled,    NDOC, "true", "true", SHOW,
	  "Readiness deferral override",
	  "Lets the readiness deferral override the game's answer. Off leaves the game's own answer."),
	CFG_BOOL("npcWaitDiag",           npcWaitDiagEnabled,           NDOC, "true", "false", DIAG,
	  "NPC path wait diagnostic",
	  "Measures how long NPC path requests wait, for the log."),
	CFG_BOOL("gatePassDiag",          gatePassDiagEnabled,          NDOC, "true", "false", DIAG,
	  "Gate code pass timing",
	  "Times each gate-code pass for the GateRate: log line."),
	CFG_BOOL("pathCostLines",         pathCostLinesEnabled,         DOC,  "true", "true", DIAG,
	  "Path search class lines",
	  "Prints the heartbeat's per-class AstarClass: detail lines; the compact AstarCap: and PathBusy:"
	  " lines print either way."),
	CFG_BOOL("zoneLifeUnload",        zoneLifeUnloadEnabled,        NDOC, "true", "true", SHOW,
	  "Unload idle mod zones",
	  "Unloads the zones the mod loaded once they are idle and outside the retain radius."),
	CFG_BOOL("islandDeletedReissue",  islandDeletedReissueEnabled,  NDOC, "true", "true", SHOW,
	  "Re-issue deleted move orders",
	  "Re-issues a player move order the engine deleted, never one the player cancelled."),
	CFG_CUSTOM_CHOICES("k7PostDeathHold",     &cfg_k7PostDeathHold, sizeof(int), ParseK7PostDeathHold, DOC, "true", "true",
	  "Hold move orders through combat",
	  "Holds a player move order that ended as combat took over, and re-issues it once the fight ends,"
	  " up to 60 seconds. observe only logs what would be held.", kK7HoldChoices),
	CFG_BOOL("k7DestReadyGate",       k7DestReadyGateEnabled,       DOC,  "true", "true", SHOW,
	  "Wait for the destination's navmesh",
	  "Before re-issuing a deleted order, waits up to 15 seconds for the destination cell's navmesh to"
	  " be in the world."),
	CFG_BOOL("k7ArrivalTrigger",      k7ArrivalTriggerEnabled,      DOC,  "true", "true", SHOW,
	  "Re-issue on navmesh arrival",
	  "When an order stops far short while its destination's navmesh is missing, re-issues it as soon"
	  " as that navmesh arrives. Off only logs when it would have sent."),
	CFG_BOOL("navmeshVanillaPruning", navmeshVanillaPruningEnabled, NDOC, "true", "true", SHOW,
	  "Game pruning on fresh navmesh buffers",
	  "Fresh navmesh work buffers carry the game's region pruning and extra-vertex settings. Off uses"
	  " Havok's defaults, under their own disk cache key."),
	CFG_BOOL("navmeshNeighbourSeeds", navmeshNeighbourSeedsEnabled, NDOC, "true", "true", SHOW,
	  "Neighbour navmesh seeds",
	  "Records the seeds each neighbour's mesh gives, and adds stand-in seeds from a neighbour's"
	  " shipped tile where no neighbour mesh is usable."),
	CFG_BOOL("navmeshBuildLockNarrow", navmeshBuildLockNarrowEnabled, DOC, "true", "true", SHOW,
	  "Narrow navmesh build lock",
	  "The navmesh collision builders lock only the parts that race, on the game's own lock. Off locks"
	  " each whole build on the mod's lock."),
	CFG_BOOL("navmeshStallThrottle",  navmeshStallThrottleEnabled,  DOC,  "true", "true", SHOW,
	  "Navmesh build lock stall throttle",
	  "When the navmesh build lock has refused every attempt for five seconds, slows the engine's retry"
	  " to a few dozen attempts a second."),
	CFG_CUSTOM_CHOICES("clusterGraphBypass",  &clusterGraphBypassMode, sizeof(int), ParseClusterGraphBypass, DOC, "true", "true",
	  "Cluster graph pre-check",
	  "true answers the engine's connectivity pre-check as connected without reading the stale cluster"
	  " graph; false hands the check back to the game. measure and player ask the graph, then wave every"
	  " pair, or a player's pairs, through.", kClusterGraphChoices),
	CFG_BOOL("unstitchGuard",         unstitchGuardEnabled,         DOC,  "true", "true", SHOW,
	  "Un-stitch bounds guard",
	  "Drops a stale navmesh connection record that names a node its neighbour no longer has, instead"
	  " of reading past the node map and crashing."),
	CFG_INT("stitchSourceLines",      cfg_stitchSourceLines, 1.0f, 0.0f, 0, DOC, "32", "32", DIAG,
	  "Stitch source log sets",
	  "How many sets that lost records to the un-stitch guard are described in the log, two lines each."
	  " 0 prints none."),
	CFG_BOOL("graphVisitorGuard",     graphVisitorGuardEnabled,     DOC,  "true", "true", SHOW,
	  "Search estimate instance guard",
	  "Keeps a path search node whose section has no graph instance in the search without a distance"
	  " estimate, instead of crashing."),
	CFG_BOOL("graphExpandGuard",      graphExpandGuardEnabled,      DOC,  "true", "true", SHOW,
	  "Search expansion instance guard",
	  "Drops a path search node whose section has no graph instance, instead of crashing, and carries"
	  " on with the rest."),
	CFG_BOOL("graphPositionGuard",    graphPositionGuardEnabled,    NDOC, "true", "true", SHOW,
	  "Cluster graph position guard",
	  "When the cluster graph is consulted, substitutes a node whose section has no graph instance"
	  " instead of crashing. Off only classifies and counts it."),
	CFG_BOOL("meshFaceGuard",         meshFaceGuardEnabled,         DOC,  "true", "true", SHOW,
	  "Navmesh face bounds guard",
	  "Leaves a navmesh face whose edge record is no longer valid out of its section's bounding box,"
	  " instead of walking outside the section and crashing."),
	CFG_BOOL("createInstanceGuard",   createInstanceGuardEnabled,   DOC,  "true", "true", SHOW,
	  "Queued navmesh piece guard",
	  "Stops the game freeing a navmesh piece that is already queued to be added. Off only counts it."),
	CFG_BOOL("hullDoublePushGuard",   hullDoublePushGuardEnabled,   DOC,  "true", "true", SHOW,
	  "Physics double delete guard",
	  "Drops a second destroy entry for one physics object before the physics thread deletes it twice."
	  " Off only counts duplicates."),
	CFG_BOOL("stitchByteGuard",       stitchByteGuardEnabled,       DOC,  "true", "true", SHOW,
	  "Interior stitch byte guard",
	  "Skips the flag byte the game writes past an interior's navmesh when its stitch finishes. Off"
	  " only records what the write hit."),
	CFG_BOOL("navmeshAdjExclusion",   navmeshAdjExclusionEnabled,   DOC,  "true", "true", SHOW,
	  "Neighbouring navmesh build exclusion",
	  "Makes a navmesh piece wait until its neighbours are built and handed to the game, so two"
	  " neighbours never stitch over each other. Off only counts the waits."),
	CFG_BOOL("playerRepathTier",      playerRepathTierEnabled,      DOC,  "true", "true", SHOW,
	  "Player path re-request priority",
	  "A player character's mid-walk path re-request keeps the priority its move order got instead of"
	  " queueing behind every NPC. Off only counts."),
	CFG_BOOL("navMeshLife",           navMeshLifeEnabled,           DOC,  "true", "true", DIAG,
	  "Navmesh section lifecycle lines",
	  "Logs one line each time a navmesh section joins or leaves the streaming collection. Read-only."),
#ifdef ZONEOPT_DEBUG
	CFG_BOOL("unstitchProbe",         unstitchProbeEnabled,         DOC,  "false", "false", DIAG,
	  "Un-stitch probe",
	  "Before each navmesh instance teardown, logs any connection record whose node index would land"
	  " outside its neighbour's node map. Changes nothing."),
	CFG_BOOL("sectionKeyProbe",       sectionKeyProbeEnabled,       DOC,  "true", "true", DIAG,
	  "Section key probe",
	  "Records the last few section-table lookups the world step makes from a packed key, so a crash"
	  " record says whether the index was in range. Changes nothing."),
#endif
	CFG_BOOL("settingsPanel",         settingsPanelEnabled,         DOC,  "true", "true", SHOW,
	  "ZoneOpt settings tab",
	  "This tab in the game's Options window."),
	CFG_BOOL("navmeshMissHash",       navmeshMissHashEnabled,       DOC,  "true", "false", DIAG,
	  "Navmesh hash log lines",
	  "Logs a hash of every generated navmesh."),
	CFG_BOOL("navmeshMissSplit",      navmeshMissSplitEnabled,      DOC,  "true", "true", SHOW,
	  "Parallel navmesh generation",
	  "Navmesh generation on a worker runs outside the lock the other navmesh jobs wait on, so several"
	  " can generate at once."),
	CFG_BOOL("navmeshMissSplitBg",    navmeshMissSplitBgEnabled,    DOC,  "true", "true", SHOW,
	  "Parallel background navmesh generation",
	  "The same for the generation the background navmesh thread runs itself. Needs parallel navmesh"
	  " generation."),
	CFG_BOOL("zoneCycleStats",        zoneCycleStatsEnabled,        DOC,  "true", "false", DIAG,
	  "Zone cycle measurement",
	  "Measures each zone manager loading cycle, the player characters in cells the mod holds and Set"
	  " B's size, for the log. Reading only."),
	CFG_BOOL("zoneWedgeGuard",        zoneWedgeGuardEnabled,        DOC,  "true", "true", DIAG,
	  "Zone cycle wedge report",
	  "Reports once if a loading cycle stays in one phase for more than 10 seconds. Reading only; never"
	  " forces the phase forward."),
	CFG_BOOL("physPurecallRecord",    physPurecallRecordEnabled,    DOC,  "true", "true", DIAG,
	  "PhysX pure call recorder",
	  "Captures the faulting thread and call site into purecall_dump.txt when PhysX aborts on a pure"
	  " virtual call. Forensic only."),
	CFG_BOOL("physQueryGuard",        physQueryGuardEnabled,        DOC,  "true", "true", SHOW,
	  "PhysX box query guard",
	  "Checks each result of the game's box query before it is used, and drops a collision shape that"
	  " is already destroyed."),
	CFG_BOOL("corpsePin",             corpsePinEnabled,             DOC,  "true", "true", SHOW,
	  "Carried corpse follows its carrier",
	  "Moves the squad of a carried NPC corpse to its carrier instead of leaving it pinned at the"
	  " pickup spot."),
	CFG_BOOL("nestValidationGuard",   nestValidationGuardEnabled,   DOC,  "true", "true", SHOW,
	  "Nest validation guard",
	  "Skips the game's nest validation for a cell whose navmesh is not in yet, so no nest is destroyed"
	  " against a missing mesh; the cell is checked again next loading cycle."),
	CFG_CUSTOM("zoneGeometryMode",    &zoneGeometryMode, sizeof(ZoneGeometryMode), ParseZoneGeometryMode, DOC, "contentOnly", "contentOnly"),
	CFG_BOOL("camFocus",              cfg_camFocusEnabled,          DOC,  "true", "true", SHOW,
	  "Camera focus prediction",
	  "Predicts the camera's zone from where it points instead of where it sits, so a zoomed-out camera"
	  " still preloads the zone the squad walks into."),

	CFG_FLOAT("preloadKeepAliveSeconds", cfg_preloadKeepAliveSeconds, 0.0f, 86400.0f, false, NDOC, "0", "0", SHOW,
	  "Preloaded zone keep-alive seconds",
	  "How long a zone the mod preloads stays loaded before it may unload. 0 takes the game's own"
	  " default.", 0.0f, 0),
	CFG_INT_CHOICES("navmeshWorkerCount",     cfg_navmeshWorkerCount, 0.0f, (float)NAVMESH_WORKER_COUNT, INT_MIN, DOC, "0", "0", SHOW,
	  "Navmesh worker threads",
	  "Threads that generate and load navmesh tiles in the background."
	  " Auto uses half the logical CPUs.", kWorkerChoices),
	CFG_INT("navmeshGenConcurrency",  cfg_navmeshGenConcurrency, 0.0f, 4.0f, INT_MIN, DOC, "0", "0", SHOW,
	  "Parallel navmesh generations",
	  "How many navmesh generations may run at once while parallel generation is on. 0 is automatic:"
	  " two, fewer on CPUs with under five logical cores."),
	CFG_INT("navmeshDiskCacheMaxMB",  cfg_navmeshDiskCacheMaxMB, 32.0f, 8192.0f, INT_MIN, NDOC, "512", "512", SHOW,
	  "Navmesh disk cache size in MB",
	  "Size cap of the navmesh_cache folder; past it the oldest files are deleted down to 75% of the"
	  " cap."),
	CFG_DOUBLE("camLogInterval",      cfg_camLogInterval, 1.0f, 300.0f, NDOC, "10.0", "10.0", DIAG,
	  "Camera log interval seconds",
	  "Seconds between debug camera log lines.", 1.0f, 0),
	// 0 stays "auto" (one zone width); a positive override is still bounded.
	CFG_FLOAT("camFocusMaxDist",      cfg_camFocusMaxDist, 500.0f, 50000.0f, true, DOC, "0", "0", SHOW,
	  "Camera focus distance cap",
	  "World units from the nearest squad member past which the focus point is pulled back toward them."
	  " 0 is one zone width.", 0.0f, 0),
	CFG_FLOAT("camFocusHardMult",     cfg_camFocusHardMult, 1.0f, 10.0f, false, DOC, "3.0", "3.0", SHOW,
	  "Camera focus hard cutoff multiple",
	  "Multiple of the distance cap past which the focus point is ignored outright.", 1.0f, 1),
	// Capped well under PRELOAD_THRESHOLD (2500) so threshold-margin stays positive.
	CFG_FLOAT("camFocusHysteresis",   cfg_camFocusHysteresis, 0.0f, 2000.0f, false, DOC, "250", "250", SHOW,
	  "Camera focus hysteresis",
	  "World units past the preload threshold a neighbouring zone's prediction must move to take over"
	  " or be released, so a point near a border does not thrash.", 0.0f, 0),
	CFG_DOUBLE("reprioritizeInterval", cfg_reprioritizeInterval, 1.0f, 30.0f, DOC, "1.0", "1.0", SHOW,
	  "Navmesh queue reorder interval seconds",
	  "Seconds between navmesh queue reorders while fast reordering is on.", 1.0f, 1),
	CFG_INT("zoneLifeRetainRadius",   cfg_zoneLifeRetainRadius, 1.0f, 4.0f, INT_MIN, NDOC, "2", "2", SHOW,
	  "Zone retain radius in cells",
	  "Radius, in cells around the camera and the player's characters, inside which a zone the mod"
	  " loaded is never unloaded."),
	CFG_DOUBLE("zoneLifeIdleSeconds", cfg_zoneLifeIdleSeconds, 5.0f, 600.0f, NDOC, "30.0", "30.0", SHOW,
	  "Idle zone unload delay seconds",
	  "Seconds a zone the mod loaded must sit outside the retain radius before it is unloaded.", 5.0f, 0),

	CFG_CUSTOM("bench.levers",        NULL, 0, ParseBenchLevers, DOC, "", ""),
	CFG_CUSTOM("bench.sweep",         NULL, 0, ParseBenchSweep, DOC,
	           "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20", "swamp:1,swamp:20,city:1,city:20,sand:1,sand:20"),

	// Keys a feature used to read. An old INI that still sets one gets a single
	// "Config: retired key '<key>' ignored" line instead of the generic
	// unknown-key line, so the log says the key is gone on purpose.
	//   squadPathCache: the squad path cache and its injection are deleted. The
	//                   injection reported a path whose edges were not a connected
	//                   chain and crashed the post-processor.
	//   stuckRetry:     the PLAYER STUCK retry (replacement move orders toward
	//                   the zone exit) was deleted; it was off by default.
	//                   PLAYER STUCK itself stays a diagnostic.
	//   instanceCullSingleThread: the single-thread instance culling lever read
	//                   render-queue slot 0, which Kenshi never uses, so every
	//                   InstanceBatchHW stopped drawing. Removed.
	//   camFocusDwellSec: the camera-focus zone-cell dwell was replaced by real
	//                   hysteresis on the prediction axes (camFocusHysteresis);
	//                   the dwell state was never read by anything behavioural.
	//   exitFaceProbe and the 17 tuning/capacity keys below: each parsed,
	//                   clamped and (cameraReserved/formationTimeout) cross-clamped
	//                   against another dead key, but nothing outside the parser
	//                   read the global it wrote. Retiring them stops them being
	//                   counted as applied overrides.
	CFG_RETIRED("squadPathCache"),
	CFG_RETIRED("stuckRetry"),
	CFG_RETIRED("instanceCullSingleThread"),
	CFG_RETIRED("camFocusDwellSec"),
	CFG_RETIRED("exitFaceProbe"),
	CFG_RETIRED("activePollInterval"),
	CFG_RETIRED("baselineScanInterval"),
	CFG_RETIRED("cameraReserved"),
	CFG_RETIRED("charScanInterval"),
	CFG_RETIRED("edgeThreshold"),
	CFG_RETIRED("evictInterval"),
	CFG_RETIRED("formationTimeout"),
	CFG_RETIRED("gatherRadiusSq"),
	CFG_RETIRED("gatherTimeout"),
	CFG_RETIRED("maxCharZones"),
	CFG_RETIRED("maxFormationGroups"),
	CFG_RETIRED("maxFormationMembers"),
	CFG_RETIRED("maxPendingOrder"),
	CFG_RETIRED("maxPreloaded"),
	CFG_RETIRED("maxWatched"),
	CFG_RETIRED("preloadThreshold"),
	CFG_RETIRED("scatterApproachDistSq"),

	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0,
	  NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

// The settings page's order.
extern const ConfigModule kConfigModules[] =
{
	{ "render", "Render and particles", g_renderKeys, &g_renderCfg, &RenderConfigDefaults(), sizeof(RenderConfig) },
	{ "core", "Zones, navmesh and pathfinding", kCoreKeys, NULL, NULL, 0 },
};
extern const int kConfigModuleCount = (int)(sizeof(kConfigModules) / sizeof(kConfigModules[0]));
