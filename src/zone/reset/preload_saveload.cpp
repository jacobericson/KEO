#include "fixes/world/destroy_list_defer.h"
#include "zone/reset/preload_saveload.h"
#include "zone/preload/coverage_stats.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "navmesh/generation/nm_misspar.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "movement/islands.h"
#include "zone/camera_zone_hook.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/preload/preload_internal.h"
#include "zone/handoff/zone_handoff.h"
#include "zone/geometry/zone_geometry_epoch.h"
#include "zone/reset/zone_reset_fence.h"
#include "zone/reset/zone_reset_sequence.h"
#include "zone/reset/zone_reset_gate.h"
#if ZONEHAND_STEP >= 2
#include "zone/handoff/zone_prep_ledger.h"
#endif
#if ZONEHAND_STEP >= 2
#include "fixes/world/nest_validation.h"
#endif
#include <cstdio>     // _snprintf_s (hook_resetUnloadZones builds its line without CRT streams)
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX only; the function is resolved at runtime


void ClearPreloadZones()
{
	{
		// What the scans queued gets thrown away here. Count by furthest
		// lifecycle stage reached (mutually exclusive) plus both queues.
		// stalled = tracked with no stage flag (stall-timed-out, never
		// registered), the same class and order the "Save load reset:" line
		// uses, so pending= means the same on both lines. A slot the
		// registry guard dropped (zoneEntry NULL, awaiting compaction) is not
		// a zone and is not counted.
		int dPending = 0, dStalled = 0, dRegistered = 0, dGameOwned = 0;
		for (int i = 0; i < numPreloaded; ++i)
		{
			if (!preloadedZones[i].zoneEntry)           continue;
			if (preloadedZones[i].gameOwned)            dGameOwned++;
			else if (preloadedZones[i].registered)      dRegistered++;
			else if (preloadedZones[i].pending)         dPending++;
			else                                        dStalled++;
		}
		int qCam  = cameraQueueCount - cameraQueueNext;
		int qChar = charQueueCount - charQueueNext;
		if (dPending || dStalled || dRegistered || dGameOwned || qCam || qChar)
		{
			std::ostringstream ss;
			ss << "Preload clear: dropped pending=" << dPending
			   << " stalled=" << dStalled
			   << " registered=" << dRegistered
			   << " gameOwned=" << dGameOwned
			   << " queued=" << qCam << "/" << qChar;
			LogMsg(ss.str());
		}
	}

	for (int i = 0; i < MAX_PRELOADED; ++i)
	{
		preloadedZones[i].zoneEntry = NULL;
		preloadedZones[i].gridX = -1;
		preloadedZones[i].gridY = -1;
		preloadedZones[i].gameOwned = false;
		preloadedZones[i].pending = false;
		preloadedZones[i].registered = false;
		preloadedZones[i].registeredEmpty = false;
		preloadedZones[i].contentProcessed = false;
		preloadedZones[i].loadTimeSec = 0.0;
		preloadedZones[i].owner = OWNER_CHARACTER;
		preloadedZones[i].carried = false;
		preloadedZones[i].pipeLoadSec = -1.0;
		preloadedZones[i].pipeIsReadySeen = false;
		preloadedZones[i].pipe264Seen = false;
	}
	numPreloaded = 0;
	pendingCount = 0;
	predictedCenterX = -1;
	predictedCenterY = -1;
	cameraQueueCount = 0;
	cameraQueueNext = 0;
	charQueueCount = 0;
	charQueueNext = 0;
	lastCharScanTime = 0.0;
	lastCameraGX = -1;
	lastCameraGY = -1;
	ResetCameraFocusState();
	// Reset polling timers so scans fire immediately after transition end
	lastBaselineScan = 0.0;
	lastActivePoll = 0.0;
}

// Full mod-state reset. clearNavMeshCache=false keeps the L1 ring buffer:
// navmesh entries are keyed by zone geometry and building layout, so they stay
// valid across a save load of the same world, and dropping them would force
// every revisited tile back through an L2 read (31-46 ms) or a generation.
// Only a startup reset drops them.
static void ClearPreloadStateImpl(bool clearNavMeshCache)
{
	ClearPreloadZones();

	// Only on full reset, not transition end
	for (int i = 0; i < MAX_WATCHED; ++i)
	{
		watchedChars[i].character = 0;
		watchedChars[i].charMovement = 0;
		watchedChars[i].hasMoveOrder = false;
		watchedChars[i].destZoneX = -1;
		watchedChars[i].destZoneY = -1;
		watchedChars[i].currentZoneX = -1;
		watchedChars[i].currentZoneY = -1;
		watchedChars[i].addedTime = 0.0;
	}
	numWatched = 0;
	if (clearNavMeshCache)
		ClearNavMeshCache();
	ClearFormationGroups();
	IslandReset();
	ZoneHandoffOnWorldReset();
	// A world replacement removes every piece of geometry at once, so it is
	// a boundary in its own right; the epoch keeps counting across it, since
	// a certificate taken in the old world must never match in the new one.
	ZoneGeometryNoteBoundary();
	ZoneGeometryCertReset();
	H15Reset();
	// Registry guard: a new session logs its first refusal per zone again.
	// regSkipCount itself stays cumulative (the Transition line reports it).
	memset(g_regGuardLogged, 0, sizeof(g_regGuardLogged));
	// The per-cell record survives transition end, not a reset. Called
	// after the reset hook's unloads (and its classification), so every zone
	// the record names is gone or about to be destroyed with the old world.
	ZlClearAll();
}

void ClearPreloadState()
{
	ClearPreloadStateImpl(/*clearNavMeshCache=*/true);
}

void ClearPreloadStateForLoad()
{
	ClearPreloadStateImpl(/*clearNavMeshCache=*/false);
	// The world clear a load performs (sub_1407A82C0) frees every Ogre movable
	// and empties destroyListOE itself, so the deferred insert queue goes with
	// the rest of the state rather than being replayed onto freed objects.
	DestroyListDropDeferred();
}


// =========================================================================
// Save-load detection — main thread
// =========================================================================
//
// SaveManager::loadGame sets the ZoneManager loading byte (ZM+8) and the game
// clears it with state 5. Everything the mod tracks (preloaded ZoneMap*,
// watched characters, formation groups, island components) names objects that
// the load destroys, so the whole state is dropped on the rising edge of that
// byte, and again if the ZoneManager pointer itself changes. While the byte is
// set, preload processing is skipped: nothing we could preload survives.
static uintptr_t g_saveLoadZm      = 0;
static bool      g_saveLoadWasSet  = false;

// Set by hook_resetUnloadZones when it has already cleared the mod's state at
// the game's reset (main thread, saveLoadUnload on); consumed by the next
// PreloadCheckSaveLoad. The reset runs inside SaveManager::loadGame, which is
// called from GameWorld__mainLoop_GPUSensitiveStuff (+0x241) before
// updateCameraZone (+0x385) in the same frame, and loadGame sets ZM+8 (0x374002)
// after the reset, so the edge below is normally the very next check.
static bool      g_resetStateCleared = false;

// The save-load state clear, shared by the reset hook and the ZM+8 edge. Every
// part is idempotent: ClearPreloadZones logs its "Preload clear:" line only
// when something was tracked, and a second run finds nothing tracked.
static void ClearModStateForLoad()
{
	ClearPreloadStateForLoad();  // also clears formation groups, the island overlay
	                             // and the deferred destroy-list queue
	charZonesQueued = 0;
	// The coverage counters describe the loaded world's preload paths, so a
	// new world starts them over rather than carrying the old one's totals.
	CoverageResetSession();
}

bool PreloadCheckSaveLoad(void* zoneMgr)
{
	uintptr_t zm = (uintptr_t)zoneMgr;
	if (!zm)
		return false;

	bool loading = *(unsigned char*)(KLIB_MEMBER(2, zm, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;

	// Consumed on every check, edge or not: it only speaks for the reset that
	// immediately precedes this frame's check.
	bool resetCleared = g_resetStateCleared;
	g_resetStateCleared = false;

	if (zm != g_saveLoadZm || (loading && !g_saveLoadWasSet))
	{
		// Rising edge of ZM+8, or a brand-new ZoneManager: drop everything.
		// Keeps the navmesh caches: they are world-keyed and survive the load.
		// After a reset hook that already cleared, this is the backstop: it
		// runs again (harmless, see ClearModStateForLoad) and catches anything
		// queued since, e.g. off-main destroy-list inserts.
		ClearModStateForLoad();
		// The first bind of a session is a pointer change with the byte clear;
		// it gets its own line so it is not read as a mid-session load. A load
		// whose reset already cleared says so, so the two lines of one load are
		// not read as two loads.
		if (!loading)
			LogMsg("Zone manager bound: preload state cleared");
		else if (resetCleared)
			LogMsg("Save load detected: preload state already cleared at the reset "
			       "(backstop clear re-run)");
		else
			LogMsg("Save load detected: preload state cleared");
		g_saveLoadZm = zm;
	}
	g_saveLoadWasSet = loading;

	// PreloadCheckSaveLoad is the only preload split-unit function camera_zone_hook.cpp
	// calls on every main-thread frame regardless of preload/zone state, so
	// PreloadStep1Tick's periodic print and the dismissal edge detector
	// are ticked from here instead of a new call site in camera_zone_hook.cpp. Skipped
	// while a save load is in progress: the world (and the camera focus zone)
	// is being torn down or rebuilt.
	if (!loading)
		PreloadStep1Tick(zoneMgr);

	return loading;
}


// =========================================================================
// Save-load reset: unload the zones the reset leaves alive
// =========================================================================
//
// The game's "Reset game" step (sub_14036CA40) calls sub_14036C1E0 to unload
// every zone in Set A and Set B, runs the world clear, and then wipes the
// ZoneMap handle registry. The mod's zones are in neither set (it loads them
// with loadSingleZone directly), so without this hook they would survive the
// reset with a live content and no registration, and the mod would later adopt
// and process one: a crash in Building::createPhysical. game.h has the
// addresses.
//
// After the original has unloaded its own zones, every ZoneMap that still has
// a content is outside both sets by construction (the original also empties
// Set A and erases each zone from Set B), so each one is unloaded here through
// the original's own per-zone call, with its arguments (zm, zone, 0).
//
// What is intact when this runs: the ZoneMap handle registry and the world's
// object set. The world clear (0x36CF2B) and the registry memset (0x36CF60)
// both come after this function returns. What has already run:
// SaveFileSystem::newGame (0x36CB0E), GameWorld__populateMapArea_nonPermanent
// (0x36CB6A), GameWorld__destroyDeathParade (0x36CB96),
// FactionManager__clearAndDestroy (0x36CBBA) and the biome/weather reset
// (0x36CBD7); nothing on the navmesh side. The game's own Set A/B unloads run
// in exactly that state, so the mod's zones are unloaded in the state vanilla
// unloads its own.
//
// unloadSingleZone frees zone+0xB8 (terrain collision) and the content, which
// a MISS inside processJobAlt reads (0x3CC99C). So the whole unload, the
// original's included, runs inside the fence (zone_reset_sequence.h), taken
// before the original: the original frees every zone in both tracking sets, a
// cell the mod handed to the game among them, through the same per-zone call
// the survivor pass uses. The fence is taken at every reset, whatever the
// survivor-unload key says, because the game's own unloads happen either way.
//
// Deadlock: the only acquisitions of processJobCS are the NavMesh background
// thread's and the workers' (through nm_workers_internal.h helpers). Each takes
// it holding no other lock — so nothing waits for processJobCS while holding a lock the
// unloads need, and the order here (processJobCS, then whatever the native
// unload takes) is the order those threads use too. Nothing on the
// content-stream thread takes it, so the NavMesh change region the unloads
// enter cannot close a cycle through it.
static const DWORD RESET_PJ_LOCK_TIMEOUT_MS = 10000;

// Furthest lifecycle stage the mod's tracking records for a zone, in the same
// order and with the same meaning as ClearPreloadZones' "Preload clear:" line:
// gameOwned > registered > pending, and "stalled" for a tracked zone with
// none of those flags (a stall-timed-out, never-registered load, or one
// waiting for the zombie handler).
enum ResetClass
{
	RESET_PENDING = 0,
	RESET_STALLED,
	RESET_REGISTERED,
	RESET_GAME_OWNED,
	RESET_UNTRACKED,
	RESET_RETAINED,     // no table entry, held by the zone-lifecycle record
	RESET_UNVERIFIED,   // a retirement an earlier reset began and could not carry out
	RESET_CLASS_COUNT
};

static const char* const kResetClassName[RESET_CLASS_COUNT] =
	{ "pending", "stalled", "registered", "gameOwned", "untracked", "retained", "unverified" };

static int ClassifyTrackedZone(int i)
{
	if (preloadedZones[i].gameOwned)       return RESET_GAME_OWNED;
	if (preloadedZones[i].registered)      return RESET_REGISTERED;
	if (preloadedZones[i].pending)         return RESET_PENDING;
	return RESET_STALLED;
}

// Main thread only.
static int ClassifyResetSurvivor(void* zoneEntry)
{
	for (int i = 0; i < numPreloaded; ++i)
	{
		if (preloadedZones[i].zoneEntry == zoneEntry)
			return ClassifyTrackedZone(i);
	}
	// A zone the working table let go of (transition-end
	// compaction, the 30 s gameOwned eviction) is still the mod's while its
	// record lives, so it is "retained", not "untracked". untracked= should
	// then read 0: a zone with a content outside Set A/B that the mod never
	// recorded (the ZoneLeak: line's unexplained=).
	if (ZlFlagsOf(zoneEntry) != 0)
		return RESET_RETAINED;
	// A cell an earlier reset left loaded because a generation was reading it:
	// the mod's tracking of it went with that world, so nothing above can name
	// it, but it is not unexplained. untracked= is the leak signal and this
	// cause does not belong in it.
	if (ZoneResetFenceHolds(GetZoneGridX(zoneEntry), GetZoneGridY(zoneEntry),
	                        ZoneResetFenceGeneration()))
		return RESET_UNVERIFIED;
	return RESET_UNTRACKED;
}

// The cells that hold content when the reset is entered, before the original
// has freed any of them. This is the only point at which a cell the mod
// handed to the game is still visible to the mod as a loaded cell, so it is
// where the reset's account of what it was about to destroy comes from.
// Main thread only: `tracked` and `adopted` are main-thread state.
struct ResetCensus
{
	int content;    // cells holding content
	int tracked;    // of those, cells in the preload table
	int adopted;    // of those, cells the preparation ledger has in a native state
	int claimed;    // of those, cells a navmesh job is generating right now
};

static void TakeResetCensus(void* zoneMgr, ResetCensus* out)
{
	out->content = 0;
	out->tracked = 0;
	out->adopted = 0;
	out->claimed = 0;
	for (int i = 0; i < ZONE_GRID_COUNT; ++i)
	{
		int gx = i / 64, gy = i % 64;
		void* ze = GetZoneEntry(zoneMgr, gx, gy);
		if (!ze || *(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT)) == NULL)
			continue;
		out->content++;
		for (int k = 0; k < numPreloaded; ++k)
		{
			if (preloadedZones[k].zoneEntry == ze)
			{
				out->tracked++;
				break;
			}
		}
#if ZONEHAND_STEP >= 2
		{
			const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
			if (e->inUse && e->identity.worldEpoch == ZoneHandoffWorldEpoch()
			    && (e->state == ZONE_STATE_NATIVE_A || e->state == ZONE_STATE_NATIVE_B_LOADING
			        || e->state == ZONE_STATE_NATIVE_ACTIVE))
				out->adopted++;
		}
#endif
		if (NavMeshZoneClaimed(ze))
			out->claimed++;
	}
}

namespace preload_saveload_detail
{
	// The hook's operations for the fence sequence. Main thread; the mod's
	// tracking is read and written only when onMain.
	struct ResetFenceCtx
	{
		void*               zoneMgr;
		bool                onMain;
		unsigned            outgoingGen;
		void**              survivors;      // ZONE_GRID_COUNT slots
		int*                counts;         // RESET_CLASS_COUNT slots
#ifdef ZONEOPT_DEBUG
		unsigned char*      survivorClass;
		unsigned char*      survivorFlags;
#endif
		int                 survivorCount;
		NavMeshPjLockResult pj;
		LARGE_INTEGER       holdStart;
		double              pjHoldMs;
		double              origMs;
		int                 unverified;
	};

	ResetFenceCtx* FenceCtx(void* ctx)
	{
		return (ResetFenceCtx*)ctx;
	}

	bool ResetDrainBegin(void*, unsigned timeoutMs, unsigned* waitedMs)
	{
		DWORD waited = 0;
		bool drained = MissParDrainBegin(timeoutMs, &waited);
		*waitedMs = waited;
		return drained;
	}

	void ResetDrainEnd(void*)
	{
		MissParDrainEnd();
	}

	ZoneResetLock ResetLockProcessJob(void* ctx, unsigned timeoutMs, unsigned* waitedMs)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		DWORD waited = 0;
		c->pj = NavMeshTryLockProcessJobFor(timeoutMs, &waited);
		*waitedMs = waited;
		if (c->pj == NM_PJLOCK_HELD)
		{
			QueryPerformanceCounter(&c->holdStart);
			return ZONE_RESET_LOCK_HELD;
		}
		return (c->pj == NM_PJLOCK_TIMEOUT) ? ZONE_RESET_LOCK_TIMEOUT : ZONE_RESET_LOCK_NONE;
	}

	// Right after the last unload, before any logging or state clear.
	void ResetUnlockProcessJob(void* ctx)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		NavMeshUnlockProcessJob();
		LARGE_INTEGER holdEnd;
		QueryPerformanceCounter(&holdEnd);
		c->pjHoldMs = QPCToMs(c->holdStart, holdEnd);
	}

	// The game unloads its own Set A/B zones, inside the fence.
	void ResetNativeUnload(void* ctx)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		LARGE_INTEGER origStart, origEnd;
		QueryPerformanceCounter(&origStart);
		orig_resetUnloadZones(c->zoneMgr);
		QueryPerformanceCounter(&origEnd);
		c->origMs = QPCToMs(origStart, origEnd);
	}

	// What still holds content is outside both sets by construction (the
	// original empties Set A and erases each zone from Set B). Classified here
	// and logged after the release, so nothing is written while processJobCS
	// is held.
	int ResetCollectSurvivors(void* ctx)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		for (int i = 0; i < ZONE_GRID_COUNT; ++i)
		{
			// ZoneManager+200, 4096 ZoneMaps of 360 bytes (ctor sub_140A0BE00's
			// vector constructor; Set A starts right after, at +1474760). Index
			// order is x*64 + y (ZoneManager__lookupZone 0xA07C10); it does not
			// matter here, since every entry is visited.
			void* ze = GetZoneEntry(c->zoneMgr, i / 64, i % 64);
			if (*(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT)) == NULL)
				continue;

#ifdef ZONEOPT_DEBUG
			c->survivorClass[c->survivorCount] = RESET_UNTRACKED;
			c->survivorFlags[c->survivorCount] = (unsigned char)((IsZoneLoading(ze) ? 1 : 0)
			                                                   | (IsZoneAccessible(ze) ? 2 : 0));
#endif
			if (c->onMain)
			{
				int cls = ClassifyResetSurvivor(ze);
#ifdef ZONEOPT_DEBUG
				c->survivorClass[c->survivorCount] = (unsigned char)cls;
#endif
				c->counts[cls]++;
			}
			c->survivors[c->survivorCount++] = ze;
		}
		return c->survivorCount;
	}

	bool ResetSurvivorClaimed(void* ctx, int i)
	{
		return NavMeshZoneClaimed(FenceCtx(ctx)->survivors[i]);
	}

	void ResetUnloadSurvivor(void* ctx, int i)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		void* ze = c->survivors[i];
		int gx = GetZoneGridX(ze);
		int gy = GetZoneGridY(ze);
		fn_unloadZoneFromReset(c->zoneMgr, ze, 0);
		// The retirement completed: an earlier reset's record of this cell, if
		// there was one, is now answered.
		if (c->onMain)
			ZoneResetFenceClear(gx, gy);
	}

	// Left loaded with its private flag still set, while every trace of it in
	// the mod's tracking goes with this world. Recorded so the cell is still
	// attributable in the next one, where it blocks both the mod's preload and
	// the game's activation lease.
	void ResetKeepSurvivor(void* ctx, int i)
	{
		ResetFenceCtx* c = FenceCtx(ctx);
		int gx = GetZoneGridX(c->survivors[i]);
		int gy = GetZoneGridY(c->survivors[i]);
		if (c->onMain && ZoneResetFenceNoteUnverified(gx, gy, c->outgoingGen))
			++c->unverified;
	}
}
using namespace preload_saveload_detail;

void __fastcall hook_resetUnloadZones(void* zoneMgr)
{
	// The admission gate is up from here to the hook's exit, an unwind included.
	ZoneResetGateScope admission(&g_zoneResetGate, &NavMeshRaiseResetGateLocked, &NavMeshWakeWorkersIfQueued,
		&NavMeshLowerResetGateLocked);
	DWORD tid     = GetCurrentThreadId();
	bool  onMain  = IsMainThread();
	bool  keyOn   = zone::g_zoneCfg.saveLoadUnloadEnabled;
	bool  fnBound = (fn_unloadZoneFromReset != NULL);
	bool  unload  = keyOn && fnBound;

#if ZONEHAND_STEP >= 2
	// A stale skip flag from the previous world would make the first
	// legitimate proceed on the same cell index, in the new world, look like
	// a spurious revalidation. Main thread only, like the ledger it clears.
	if (onMain)
		NestValidationClearOnReset();
#endif

	// NavMesh jobs in flight as the reset is entered, before anything has been
	// unloaded: workerBusyCount is the busy bridge, raised by the worker path
	// at claim time and by the bg thread at the unlink, so it counts
	// MISSes and HITs on both.
	long nmBusy = InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0);

	// What the reset is about to destroy, taken before the original frees any
	// of it. The survivor pass below runs after the original and by
	// construction can no longer see a cell the game owned.
	ResetCensus census;
	census.content = census.tracked = census.adopted = census.claimed = 0;
	if (onMain)
		TakeResetCensus(zoneMgr, &census);

	// The generation these records belong to: the reset that is running now.
	// Read once here rather than at the skip below, so the order of the
	// advance at the end of the function cannot change what is recorded.
	unsigned outgoingGen = ZoneResetFenceGeneration();

	// The mod's tracking (preloadedZones etc.) is main-thread state; off the
	// main thread it is neither read nor cleared here.
	int counts[RESET_CLASS_COUNT] = { 0 };
	static void* s_survivors[ZONE_GRID_COUNT];
#ifdef ZONEOPT_DEBUG
	// The per-zone lines are written after the fence is released, so what they
	// report has to be kept: the class, and +176/+177 as they were before the
	// unload.
	static unsigned char s_survivorClass[ZONE_GRID_COUNT];
	static unsigned char s_survivorFlags[ZONE_GRID_COUNT];
#endif

	ResetFenceCtx fc;
	fc.zoneMgr       = zoneMgr;
	fc.onMain        = onMain;
	fc.outgoingGen   = outgoingGen;
	fc.survivors     = s_survivors;
	fc.counts        = counts;
#ifdef ZONEOPT_DEBUG
	fc.survivorClass = s_survivorClass;
	fc.survivorFlags = s_survivorFlags;
#endif
	fc.survivorCount = 0;
	fc.pj            = NM_PJLOCK_NONE;
	fc.holdStart.QuadPart = 0;
	fc.pjHoldMs      = 0.0;
	fc.origMs        = 0.0;
	fc.unverified    = 0;

	ZoneResetFenceOps ops;
	ops.ctx              = &fc;
	ops.drainBegin       = &ResetDrainBegin;
	ops.drainEnd         = &ResetDrainEnd;
	ops.lock             = &ResetLockProcessJob;
	ops.unlock           = &ResetUnlockProcessJob;
	ops.nativeUnload     = &ResetNativeUnload;
	ops.collectSurvivors = &ResetCollectSurvivors;
	ops.survivorClaimed  = &ResetSurvivorClaimed;
	ops.unloadSurvivor   = &ResetUnloadSurvivor;
	ops.keepSurvivor     = &ResetKeepSurvivor;

	ZoneResetFenceOutcome fence;
	ZoneResetRunFence(&ops, RESET_PJ_LOCK_TIMEOUT_MS, 500, unload, &fence);

	NavMeshPjLockResult pj = fc.pj;
	DWORD  pjWaitMs   = fence.lockWaitMs;
	double pjHoldMs   = fc.pjHoldMs;
	double origMs     = fc.origMs;
	DWORD  drainMs    = fence.drainMs;
	int    drainSkip  = fence.kept;
	int    unverified = fc.unverified;
	int    survivors  = fence.survivors;

#ifdef ZONEOPT_DEBUG
	// The per-zone lines, outside the fence.
	if (onMain)
	{
		for (int k = 0; k < survivors; ++k)
		{
			std::ostringstream ss;
			ss << "Save load reset: zone (" << GetZoneGridX(s_survivors[k]) << ","
			   << GetZoneGridY(s_survivors[k]) << ") "
			   << kResetClassName[s_survivorClass[k]]
			   << " load=" << ((s_survivorFlags[k] & 1) ? 1 : 0)
			   << " access=" << ((s_survivorFlags[k] & 2) ? 1 : 0)
			   << (unload ? " unloading" : " left loaded");
			LogDebug(ss.str());
		}
	}
#endif

	// The mod's state is cleared below, after the line, only on the main thread
	// and only when the zones were unloaded; otherwise the ZM+8 edge in
	// PreloadCheckSaveLoad does it, as before this hook existed.
	bool cleared = unload && onMain;

	// One PROD line per reset, built without CRT streams so it is safe on any
	// thread the reset might run on. It comes before the clear so that the
	// clear's own "Preload clear: dropped ..." line reads as its consequence.
	// Sized for every field at once plus the explanatory tail, which is last
	// in the format and would be the first thing a truncation dropped.
	char buf[1024];
	char classes[224];
	if (onMain)
		_snprintf_s(classes, sizeof(classes), _TRUNCATE,
			"pending=%d stalled=%d registered=%d gameOwned=%d retained=%d unverified=%d untracked=%d",
			counts[RESET_PENDING], counts[RESET_STALLED], counts[RESET_REGISTERED],
			counts[RESET_GAME_OWNED], counts[RESET_RETAINED],
			counts[RESET_UNVERIFIED], counts[RESET_UNTRACKED]);
	else
		_snprintf_s(classes, sizeof(classes), _TRUNCATE,
			"pending=- stalled=- registered=- gameOwned=- retained=- unverified=- untracked=-");

	// pj=none: this build has no processJobCS. The lock is attempted at every
	// reset, whatever the survivor count, because what it covers is the
	// original's own unloads.
	const char* pjTok = (pj == NM_PJLOCK_HELD)    ? "held"
	                  : (pj == NM_PJLOCK_TIMEOUT) ? "timeout"
	                  :                             "none";

	const char* tail = "";
	if (!keyOn)
		tail = " (saveLoadUnload=off: survivors left loaded)";
	else if (!fnBound)
		tail = " (unload function not bound: survivors left loaded)";
	else if (!cleared)
		tail = " (off main thread: mod state not read or cleared here; left to the ZM+8 edge)";

	// unverified=<this reset>/<held now>/<refused, table full>. The third
	// field is how many skipped cells the table could not take: with one, the
	// other two stop accounting for every hold-over.
	//
	// What held content when the reset was entered, against what survived the
	// original. adopted= is the cells the mod had handed to the game: they are
	// inside both tracking sets and the original frees every one of them, so
	// they can never appear in the survivor counts. claimed= is generations
	// running at that instant; with pj=held and drainMs bounded, none of them
	// is still reading a zone by the time anything is freed.
	// A count a build cannot take reads "-", so a zero always means the reset
	// looked and found none.
#if ZONEHAND_STEP >= 2
	char adoptedTok[16];
	_snprintf_s(adoptedTok, sizeof(adoptedTok), _TRUNCATE, "%d", census.adopted);
#else
	const char* adoptedTok = "-";
#endif
	char claimedTok[16];
	_snprintf_s(claimedTok, sizeof(claimedTok), _TRUNCATE, "%d", census.claimed);
	char preTok[128];
	if (onMain)
		_snprintf_s(preTok, sizeof(preTok), _TRUNCATE,
			" pre=%d tracked=%d adopted=%s claimed=%s",
			census.content, census.tracked, adoptedTok, claimedTok);
	else
		_snprintf_s(preTok, sizeof(preTok), _TRUNCATE,
			" pre=- tracked=- adopted=- claimed=-");

	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"Save load reset: %s %d zone(s) outside Set A/B (%s) tid=%lu main=%d"
		"%s gen=%u nmBusy=%ld pj=%s pjWaitMs=%lu pjHoldMs=%.1f origMs=%.1f"
		" drainMs=%lu drainSkip=%d admitDeferred=%ld unverified=%d/%d/%ld%s",
		unload ? "unloaded" : "would unload", survivors, classes,
		(unsigned long)tid, onMain ? 1 : 0, preTok, outgoingGen,
		nmBusy, pjTok, (unsigned long)pjWaitMs, pjHoldMs, origMs,
		(unsigned long)drainMs, drainSkip, ZoneResetGateDeferredTotal(&g_zoneResetGate),
		unverified, ZoneResetFenceCount(), ZoneResetFenceOverflow(), tail);
	LogMsg(buf);

	// The reset is over: records made during it are hold-overs from here on.
	if (onMain)
		ZoneResetFenceAdvanceGeneration();

	// Drop the mod's state at the same moment the game drops its own, so no
	// pointer into a zone unloaded above is used again. With the key off
	// nothing was unloaded, so the clear stays where it is today (the edge).
	if (cleared)
	{
		ClearModStateForLoad();
		g_resetStateCleared = true;
	}
}


// Transition-end compaction (replaces ClearPreloadZones there).
//   kept:    pending and registered entries (their registration resumes at
//            state 0), game-owned entries, which still leave the table
//            through EvictStaleZones' 30 s rule, and the stalled
//            never-registered entries too: the zombie handler then unloads
//            them through UnloadModZone (save = false). Dropped, such a +176
//            zone inside the retention radius would stay loaded for good,
//            block the game's own load of it and could trip the pause near an
//            inaccessible zone;
//   dropped: registry-guard slots and both queues -- the same queue reset as
//            ClearPreloadZones.
// The per-cell record is untouched: every zone dropped here stays recorded.
// Like ClearPreloadZones' "Preload clear:" line, this line says what the
// transition end threw away, plus what it kept.
void CompactPreloadZones(double transitionSec)
{
	int kPending = 0, kRegistered = 0, kGameOwned = 0, kStalled = 0;
	int dStalled = 0;
	int qCam  = cameraQueueCount - cameraQueueNext;
	int qChar = charQueueCount - charQueueNext;
	double now = ElapsedSec();
	if (transitionSec < 0.0)
		transitionSec = 0.0;

	int n = 0;
	int pend = 0;
	for (int i = 0; i < numPreloaded; ++i)
	{
		if (!preloadedZones[i].zoneEntry)
			continue;                              // registry-guard slot
		if (preloadedZones[i].gameOwned)       kGameOwned++;
		else if (preloadedZones[i].registered) kRegistered++;
		else if (preloadedZones[i].pending)    kPending++;
		else
		{
			// Stall-timed-out, never registered: kept for the zombie handler's
			// unload.
			kStalled++;
		}
		if (preloadedZones[i].pending)
		{
			pend++;
			// The stall timer does not run while a transition is open
			// (EvictStaleZones), so give the pending entry back the time the
			// loading screen took: its registration resumes at state 0 with
			// the budget it had left. Capped at now (an entry loaded during
			// the bracket).
			double t = preloadedZones[i].loadTimeSec + transitionSec;
			preloadedZones[i].loadTimeSec = (t < now) ? t : now;
		}
		// The one-zone-in-flight gate skips entries carried across the
		// transition, so the new location's queue starts at once.
		preloadedZones[i].carried = true;
		if (n != i)
			preloadedZones[n] = preloadedZones[i];
		n++;
	}
	for (int i = n; i < MAX_PRELOADED; ++i)
	{
		preloadedZones[i].zoneEntry = NULL;
		preloadedZones[i].gridX = -1;
		preloadedZones[i].gridY = -1;
		preloadedZones[i].gameOwned = false;
		preloadedZones[i].pending = false;
		preloadedZones[i].registered = false;
		preloadedZones[i].registeredEmpty = false;
		preloadedZones[i].contentProcessed = false;
		preloadedZones[i].loadTimeSec = 0.0;
		preloadedZones[i].owner = OWNER_CHARACTER;
		preloadedZones[i].carried = false;
		preloadedZones[i].pipeLoadSec = -1.0;
		preloadedZones[i].pipeIsReadySeen = false;
		preloadedZones[i].pipe264Seen = false;
	}
	numPreloaded = n;
	pendingCount = pend;

	// dropped stalled= and kept stalled= are both always printed, so a reader
	// compares like with like.
	if (dStalled || qCam || qChar || kPending || kRegistered || kGameOwned || kStalled)
	{
		std::ostringstream ss;
		ss << "Preload compact: dropped stalled=" << dStalled
		   << " queued=" << qCam << "/" << qChar
		   << " kept pending=" << kPending
		   << " registered=" << kRegistered
		   << " gameOwned=" << kGameOwned;
		ss << " stalled=" << kStalled;
		ss << " records=" << g_zlLive;
		LogMsg(ss.str());
	}

	// The same resets ClearPreloadZones makes, table aside.
	predictedCenterX = -1;
	predictedCenterY = -1;
	cameraQueueCount = 0;
	cameraQueueNext = 0;
	charQueueCount = 0;
	charQueueNext = 0;
	lastCharScanTime = 0.0;
	lastCameraGX = -1;
	lastCameraGY = -1;
	ResetCameraFocusState();
	lastBaselineScan = 0.0;
	lastActivePoll = 0.0;
}
