// navmesh_config.cpp - defaults and INI rows for the navmesh module.
#include "navmesh/navmesh_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

static const ConfigChoice kWorkerChoices[] =
{
	{ "0", 0, "Auto" }, { "1", 1, "1" }, { "2", 2, "2" }, { "3", 3, "3" }, { "4", 4, "4" }, { "5", 5, "5" }, { "6", 6, "6" }
};
static_assert(sizeof(kWorkerChoices) / sizeof(kWorkerChoices[0]) == NAVMESH_WORKER_COUNT + 1,
              "a worker choice per count up to NAVMESH_WORKER_COUNT, and Auto");
// The worker-count row's upper bound is written as a literal: cl 16 turns an
// explicit cast of the named constant into a run-time initializer for this
// table and places it in writable data.
static_assert(NAVMESH_WORKER_COUNT == 6, "the navmeshWorkerCount row's upper bound is 6.0f");

namespace navmesh {

const NavMeshConfig kNavMeshDefaults =
{
	true, // priorityBoostEnabled
	true, // cachingEnabled
	true, // reprioFastEnabled
	true, // routeTierEnabled
	true, // navmeshVanillaPruningEnabled
	true, // navmeshNeighbourSeedsEnabled
	true, // navmeshBuildLockNarrowEnabled
	true, // navmeshStallThrottleEnabled
	true, // navmeshAdjExclusionEnabled
#ifdef KEO_DEBUG
	true, // navmeshMissHashEnabled
#else
	false, // navmeshMissHashEnabled
#endif
	true, // navmeshMissSplitEnabled
	true, // navmeshMissSplitBgEnabled
	0, // cfg_navmeshWorkerCount
	0, // cfg_navmeshGenConcurrency
	512, // cfg_navmeshDiskCacheMaxMB
	1.0, // cfg_reprioritizeInterval
	0, // g_navMeshWorkerCount
	0, // g_modSetHash
};

NavMeshConfig g_navmeshCfg = kNavMeshDefaults;
} // namespace navmesh

namespace navmesh_config_detail {
union NavMeshConfigPodCheck { navmesh::NavMeshConfig s; };
} // namespace navmesh_config_detail
using namespace navmesh_config_detail;

namespace navmesh {
static_assert(__alignof(NavMeshConfig) >= 8, "NavMeshConfig must be 8-byte aligned");

const ConfigKey g_navmeshConfigKeys[] =
{
	CFG_OBOOL("priorityBoost", NavMeshConfig, priorityBoostEnabled,         NDOC, DEVROW,
	  "Navmesh thread priority boost",
	  "Raises the priority of the game's navmesh thread while a zone transition loads."),
	CFG_OBOOL("caching", NavMeshConfig, cachingEnabled,               NDOC, DEVROW,
	  "Navmesh cache",
	  "Keeps generated navmesh tiles in memory and on disk and reuses them instead of generating them"
	  " again."),
	CFG_OBOOL("reprioFast", NavMeshConfig, reprioFastEnabled,            DOC, DEVROW,
	  "Fast navmesh queue reordering",
	  "Reorders the navmesh job queue on the interval below and again the moment a move order is"
	  " issued, instead of a 3 second backstop."),
	CFG_OBOOL("routeTier", NavMeshConfig, routeTierEnabled,             DOC, DEVROW,
	  "Move order zone first",
	  "The zone a character with a move order stands in is generated before the zones around the"
	  " camera."),
	CFG_OBOOL("navmeshVanillaPruning", NavMeshConfig, navmeshVanillaPruningEnabled, NDOC, DEVROW,
	  "Game pruning on fresh navmesh buffers",
	  "Fresh navmesh work buffers carry the game's region pruning and extra-vertex settings. Off uses"
	  " Havok's defaults, under their own disk cache key."),
	CFG_OBOOL("navmeshNeighbourSeeds", NavMeshConfig, navmeshNeighbourSeedsEnabled, NDOC, DEVROW,
	  "Neighbour navmesh seeds",
	  "Records the seeds each neighbour's mesh gives, and adds stand-in seeds from a neighbour's"
	  " shipped tile where no neighbour mesh is usable."),
	CFG_OBOOL("navmeshBuildLockNarrow", NavMeshConfig, navmeshBuildLockNarrowEnabled, DOC, DEVROW,
	  "Narrow navmesh build lock",
	  "The navmesh collision builders lock only the parts that race, on the game's own lock. Off locks"
	  " each whole build on the mod's lock."),
	CFG_OBOOL("navmeshStallThrottle", NavMeshConfig, navmeshStallThrottleEnabled,  DOC, DEVROW,
	  "Navmesh build lock stall throttle",
	  "When the navmesh build lock has refused every attempt for five seconds, slows the engine's retry"
	  " to a few dozen attempts a second."),
	CFG_OBOOL("navmeshAdjExclusion", NavMeshConfig, navmeshAdjExclusionEnabled,   DOC, DEVROW,
	  "Neighbouring navmesh build exclusion",
	  "Makes a navmesh piece wait until its neighbours are built and handed to the game, so two"
	  " neighbours never stitch over each other. Off only counts the waits."),
	CFG_OBOOL("navmeshMissHash", NavMeshConfig, navmeshMissHashEnabled,       DOC, DEVROW,
	  "Navmesh hash log lines",
	  "Logs a hash of every generated navmesh."),
	CFG_OBOOL("navmeshMissSplit", NavMeshConfig, navmeshMissSplitEnabled,      DOC, DEVROW,
	  "Parallel navmesh generation",
	  "Navmesh generation on a worker runs outside the lock the other navmesh jobs wait on, so several"
	  " can generate at once."),
	CFG_OBOOL("navmeshMissSplitBg", NavMeshConfig, navmeshMissSplitBgEnabled,    DOC, DEVROW,
	  "Parallel background navmesh generation",
	  "The same for the generation the background navmesh thread runs itself. Needs parallel navmesh"
	  " generation."),
	CFG_OINT_CHOICES("navmeshWorkerCount", NavMeshConfig, cfg_navmeshWorkerCount, 0.0f, 6.0f, INT_MIN, DOC, SHOW,
	  "Background navmesh threads",
	  "Threads that generate and load navmesh tiles in the background."
	  " Auto uses half the logical CPUs.", kWorkerChoices),
	CFG_OINT("navmeshGenConcurrency", NavMeshConfig, cfg_navmeshGenConcurrency, 0.0f, 4.0f, INT_MIN, DOC, DEVROW,
	  "Parallel navmesh generations",
	  "How many navmesh generations may run at once while parallel generation is on. 0 is automatic:"
	  " the logical CPU count less three, at most four, one on CPUs with under five logical cores."),
	CFG_OINT("navmeshDiskCacheMaxMB", NavMeshConfig, cfg_navmeshDiskCacheMaxMB, 32.0f, 8192.0f, INT_MIN, NDOC, SHOW,
	  "Navmesh cache size (MB)",
	  "Size cap of the navmesh_cache folder; past it the oldest files are deleted down to 75% of the"
	  " cap."),
	CFG_ODOUBLE("reprioritizeInterval", NavMeshConfig, cfg_reprioritizeInterval, 1.0f, 30.0f, DOC, DEVROW,
	  "Navmesh queue reorder interval seconds",
	  "Seconds between navmesh queue reorders while fast reordering is on.", 1.0f, 1),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace navmesh
