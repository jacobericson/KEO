// navmesh_config.h - the navmesh module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"

// Capacity, not the live count: it sizes the worker handle arrays and the L2
// in-flight table, and bounds what navmeshWorkerCount may be set to. The number
// of workers actually created is g_navMeshWorkerCount (default: half the
// logical CPUs, via FinalizeConfig).
const int NAVMESH_WORKER_COUNT = 6;

namespace navmesh {

// Starts as a copy of kNavMeshDefaults, then written on the main thread by
// LoadConfig and FinalizeConfig before any hook installs. Two main-thread
// writers later only clear flags: CheckBuildGate (plugin_entry.cpp), when
// the build gate fails and no hook installs, clears priorityBoostEnabled and
// cachingEnabled; InstallHooks (hook_manifest.cpp), with earlier hooks
// already live, clears cachingEnabled when the dispatchJob hook fails. Read
// on any thread; a hook running during that clear reads the old or the new
// value of one bool.
struct NavMeshConfig
{
	bool priorityBoostEnabled;

	bool cachingEnabled;

	// reprioFast: reprioritize the navmesh job queue on the reprioritizeInterval
	// cadence whether or not anything is tracked, and once more immediately when
	// the player issues a move order. false = a 3 s backstop that runs only while
	// a mover or a queued preload exists.
	bool reprioFastEnabled;

	// routeTier: a zone a mover with a move order stands in takes the top navmesh
	// tier. Any watched mover's zone already ranks tier 2, so this is a 2 -> 1
	// lift for the zones the camera grid does not already cover. It also orders
	// mod registration, because preload_queue.cpp ranks its candidates
	// with the same ComputeZonePriority.
	// Read at one site, in ComputeZonePriority.
	bool routeTierEnabled;

	// navmeshVanillaPruning: fresh navmesh work buffers carry the game's
	// region-pruning and extra-vertex settings copied from the real one. On by
	// default; false = Havok's defaults on fresh work buffers, under their own L2
	// settings hash, for the A/B. Read-only after LoadConfig
	// (NmVanillaPruningActive, nm_quality.h).
	bool navmeshVanillaPruningEnabled;

	// navmeshNeighbourSeeds: the getSeedPointsFromAdjacentZone hook, which
	// records the seeds each neighbour's mesh gave, and the shipped-tile stand-in
	// seeds (where the game found no usable neighbour mesh, the seeds that
	// neighbour's shipped tile gives are appended; the L2 settings hash folds
	// "nbrseed1"). On by default; false = no hook, no prefetch and the L2
	// settings hash without the marker, for the A/B. Read-only after LoadConfig
	// (NmNbrSeedHookWanted / NmNbrSeedStandInActive, nm_cache_core.h).
	bool navmeshNeighbourSeedsEnabled;

	// navmeshBuildLockNarrow: the collision builders take the
	// game's build mutex around the regions that race instead of buildCollisionCS
	// around each whole builder. On by default; false = the wide cover with the
	// same stats, for the A/B. Read once, when the builder hooks install.
	bool navmeshBuildLockNarrowEnabled;

	// navmeshStallThrottle: when a run of non-blocking build-lock acquisitions is
	// refused for long enough that no live holder can explain it, slow the engine's
	// unbounded retry down. On by default; false leaves the retry at full rate, for
	// the A/B. Read on every refused acquisition.
	bool navmeshStallThrottleEnabled;

	// navmeshAdjExclusion: a navmesh job is not claimed while
	// a job whose stitches touch the same objects is in flight or published but
	// not yet drained (nm_adjacency.h). The registry, the drain observer and the
	// checker run whatever this key says; false only counts (adjWould=). Default
	// true. Read when the observer installs, and never again.
	bool navmeshAdjExclusionEnabled;

	// navmeshMissHash: log a MissHash: line for every generated MISS.
	// DEV default on, PROD default off; read by
	// nm_misspar.cpp.
	bool navmeshMissHashEnabled;

	// navmeshMissSplit: a worker clone's MISS releases
	// processJobCS around realGenerate. On by default; false = every populate
	// runs under the lock, with the same counters, for the A/B. Read by the
	// populate hook on every call.
	bool navmeshMissSplitEnabled;

	// navmeshMissSplitBg: the same release for a MISS that
	// runs on the real generator with a fresh work buffer swapped in (the
	// background thread's own, and a worker whose clone failed). Needs
	// navmeshMissSplit as well. False = that path stays under the lock, for the
	// A/B.
	bool navmeshMissSplitBgEnabled;

	// NavMesh workers
	int    cfg_navmeshWorkerCount;    // INI value; 0 = automatic (half the logical CPUs)

	// How many realGenerate calls may run at once with processJobCS released.
	// 0 = automatic (two, fewer on small CPUs), 1 = one at a
	// time, clamped to 0..4. Read once at startup.
	int    cfg_navmeshGenConcurrency;

	// NavMesh L2 disk cache
	int    cfg_navmeshDiskCacheMaxMB; // navmesh_cache\ size cap in MB; past it the
	                                  // oldest files are deleted down to 75% of the cap

	double cfg_reprioritizeInterval;  // seconds between navmesh queue reprioritization

	// Derived by LoadConfig and FinalizeConfig; no key.

	// The clamped worker count, i.e. how many threads CreateNavMeshWorkers starts
	// and how many the priority boost/restore loops walk. Set once from
	// cfg_navmeshWorkerCount after clamping.
	int    g_navMeshWorkerCount;

	// FNV-1a over the game's active mod list (mods.cfg), read once on the main
	// thread in LoadConfig. Part of every L2 disk cache filename: a changed mod set
	// can move terrain and buildings, so its meshes must not be reused.
	// 0 means "mod list unavailable".
	unsigned int g_modSetHash;
};

extern NavMeshConfig g_navmeshCfg;
extern const NavMeshConfig kNavMeshDefaults;
extern const ConfigKey g_navmeshConfigKeys[];

} // namespace navmesh
