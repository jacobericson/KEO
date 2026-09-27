#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/handoff/zone_handoff.h"

#if ZONEHAND_STEP >= 2

#include "zone/handoff/zone_adoption_seam.h"
#include "zone/zone_ledger.h"
#include "base/core.h"
#include "fixes/world/nest_validation.h"
#include "game/game.h"
#include "zone/grid.h"
#include "zone/preload/preload.h"
#include "zone/preload/preload_internal.h"
#include "zone/transition.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/retention/zone_retention.h"
#include <string.h>
#include <sstream>
#include <iomanip>

// Everything below runs on the main thread. The two tracking sets are the
// game's own containers and an insert can rehash them, so a mod-side insert
// is legal only where a native insert would be: on this thread, and never
// while a native phase is walking the set it touches.

// The prep ledger itself is the shared instance in zone_prep_ledger.cpp.
static ZoneAdoptionLatency  g_latency;
static bool     g_init       = false;
static unsigned g_worldEpoch = 1;

// The largest cohort one loading cycle may carry. The cycle's fixed cost is
// per cycle, so a big cohort is cheaper than many small ones; the cap exists
// only to bound the work a single frame's insert loop does.
static const int  ZONE_ADOPT_COHORT_MAX = 32;
// One slot per cell, and the add is deduplicated, so the follow list cannot
// fill: a cell dropped here would never be walked, never retired, and its
// ledger slot would refuse every later preparation for the rest of the
// session. The counter below exists to prove that, not because a bound was
// chosen.
static const int  ZONE_ADOPT_NATIVE_MAX = ZONE_GRID_COUNT;
// A cell the game has let go is not re-admitted before this, so preparing,
// admitting and expiring cannot become a loading cycle per loop.
static const double ZONE_ADOPT_REENTRY_SEC = 20.0;

struct ZoneHandoffCellState
{
	double readyAt;         // entered ReadyForAdoption
	double admittedAt;      // inserted into the pending set
	double lastAdmittedAt;  // the re-entry brake's clock
	int    pendingType;     // activation type of a demand deferred while the loader walked the set
};
static ZoneHandoffCellState g_cells[ZONE_GRID_COUNT];

// Cell indices the ledger currently holds in a native state, so the tick
// follows them without walking the whole grid.
static int g_native[ZONE_ADOPT_NATIVE_MAX];
static int g_nativeCount = 0;

static long g_takeovers[ZONE_ACTIVATION_COUNT] = { 0, 0, 0 };
static long g_takeoverDeferred = 0;
static long g_townRefusals     = 0;
static long g_cohorts          = 0;
static long g_admitted         = 0;
static long g_retired          = 0;
static long g_ledgerRefused    = 0;   // Begin/SetState/Release calls the ledger turned down
static long g_insertDuplicate  = 0;   // the cell was already in the pending set
static long g_nativeFull       = 0;   // adopted cells the follow list could not take (unreachable by construction)
static long g_pendingAdopted   = 0;   // deferred demands answered once the loader stopped walking
static long g_cohortSaturated  = 0;   // AdmitCohort frames where more cells were ready than ZONE_ADOPT_COHORT_MAX could take
static double g_nextStatsLog   = 0.0;
static bool   g_adoptedThisFrame = false;
static bool   g_cycleFromAdoption = false;  // latched across a loading cycle; see ZoneHandoffAdoptionCycleInFlight

void ZoneHandoffInit()
{
	if (g_init)
		return;
	ZonePrepLedgerInit(&g_zonePrepLedger);
	ZoneAdoptionLatencyInit(&g_latency);
	memset(g_cells, 0, sizeof(g_cells));
	g_nativeCount = 0;
	g_init = true;
}

static void EnsureInit()
{
	ZoneHandoffInit();
}

// ZoneCellIndex clamps coordinates outside the grid onto a real cell, so
// every entry point checks the pair itself before it reaches the ledger.
static bool CellInGrid(int gx, int gy)
{
	return gx >= 0 && gx <= ZONE_GRID_MAX && gy >= 0 && gy <= ZONE_GRID_MAX;
}

static void NativeListAdd(int cell)
{
	for (int i = 0; i < g_nativeCount; ++i)
		if (g_native[i] == cell)
			return;
	if (g_nativeCount < ZONE_ADOPT_NATIVE_MAX)
		g_native[g_nativeCount++] = cell;
	else
		g_nativeFull++;
}

static void NativeListRemoveAt(int i)
{
	g_nativeCount--;
	if (i < g_nativeCount)
		g_native[i] = g_native[g_nativeCount];
}


// =========================================================================
// Countdowns
// =========================================================================

// The engine writes activatedCountdown[type] before it notices the cell is
// already held, and a cell outside the active set never decrements. Left
// alone, those values read as a lease that cannot expire.
//
// Clearing them after a mod load also discards the keep-alive the load asked
// for, which is the same field: the preload keep-alive setting has no effect
// from this step on.
static void ClearCountdowns(void* zoneEntry, int keepType)
{
	if (!zoneEntry)
		return;
	float* c = (float*)(KLIB_MEMBER(2, (uintptr_t)zoneEntry, ZoneMap_activatedCountdown, OFF_ZONE_COUNTDOWNS));
	for (int t = 0; t < ZONE_ACTIVATION_COUNT; ++t)
		if (t != keepType)
			c[t] = 0.0f;
}


// =========================================================================
// Preparation
// =========================================================================

unsigned ZoneHandoffWorldEpoch()
{
	return g_worldEpoch;
}

void ZoneHandoffBeginFrame()
{
	g_adoptedThisFrame = false;
}

bool ZoneHandoffAdoptedThisFrame()
{
	return g_adoptedThisFrame;
}

bool ZoneHandoffAdoptionCycleInFlight()
{
	return g_cycleFromAdoption;
}

void ZoneHandoffOnWorldReset()
{
	EnsureInit();
	ZoneRetentionOnWorldReset();
	g_worldEpoch++;
	ZonePrepLedgerClearAll(&g_zonePrepLedger);
	memset(g_cells, 0, sizeof(g_cells));
	g_nativeCount = 0;
	g_cycleFromAdoption = false;
}

static void AdvancePrivate(int gx, int gy, int toState)
{
	if (!CellInGrid(gx, gy))
		return;
	EnsureInit();
	const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
	if (!e->inUse)
		return;
	if (!ZonePrepLedgerSetState(&g_zonePrepLedger, gx, gy, toState, ElapsedSec()))
		g_ledgerRefused++;
}

void ZoneHandoffNoteContentInitialized(int gx, int gy)
{
	AdvancePrivate(gx, gy, ZONE_STATE_PRIVATE_CONTENT);
}

void ZoneHandoffNoteRegistered(int gx, int gy)
{
	// Registration can happen without the mod having called the finalize
	// itself — a cell whose content the game's own tick finalized first
	// reaches the register step straight from the shell stage. The record
	// still has to pass through the content stage, which is true of it by
	// then, rather than skip one and be refused into a stage it can never
	// leave.
	if (CellInGrid(gx, gy)
	    && ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy)->state == ZONE_STATE_PRIVATE_SHELL)
		AdvancePrivate(gx, gy, ZONE_STATE_PRIVATE_CONTENT);
	AdvancePrivate(gx, gy, ZONE_STATE_PRIVATE_NAV);
}

// Retiring and releasing are two ledger calls, each of which can refuse; a
// refusal leaves the slot where it was, which would strand the cell, so both
// returns are checked and a failure is counted rather than assumed away.
static bool RetireAndRelease(int gx, int gy, double now)
{
	// First, and ahead of the two calls that can refuse: the retention entry
	// is independent of them, and a cell whose retirement they turn down is
	// one nothing else will come back to.
	ZoneRetentionNoteRetired(gx, gy);
	if (!ZonePrepLedgerSetState(&g_zonePrepLedger, gx, gy, ZONE_STATE_RETIRING, now))
	{
		g_ledgerRefused++;
		return false;
	}
	if (!ZonePrepLedgerRelease(&g_zonePrepLedger, gx, gy))
	{
		g_ledgerRefused++;
		return false;
	}
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, gx, gy, ZONE_CLASS_NONE);
	g_retired++;
	return true;
}

void ZoneHandoffNoteLoaded(void* zoneEntry, int gx, int gy)
{
	EnsureInit();
	if (!CellInGrid(gx, gy))
		return;

	ClearCountdowns(zoneEntry, -1);

	double now = ElapsedSec();
	if (!ZonePrepLedgerBegin(&g_zonePrepLedger, gx, gy, g_worldEpoch, now))
	{
		// A record for an earlier attempt is still in the slot. If that
		// attempt was private, the preload that just took this cell has
		// superseded it and the record can be retired here; if it was
		// adopted, the cell is the game's and only the tick may retire it,
		// so the new preparation goes unrecorded rather than overwriting it.
		const ZonePrepEntry* stale = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
		bool privateStale = stale->inUse
		                 && stale->state != ZONE_STATE_NATIVE_A
		                 && stale->state != ZONE_STATE_NATIVE_B_LOADING
		                 && stale->state != ZONE_STATE_NATIVE_ACTIVE;
		if (!privateStale || !RetireAndRelease(gx, gy, now)
		    || !ZonePrepLedgerBegin(&g_zonePrepLedger, gx, gy, g_worldEpoch, now))
		{
			g_ledgerRefused++;
			return;
		}
	}
	int cell = ZoneCell(gx, gy);
	g_cells[cell].readyAt    = 0.0;
	g_cells[cell].admittedAt = 0.0;
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, gx, gy, ZONE_CLASS_PRIVATE);
}

void ZoneHandoffNoteDropped(int gx, int gy)
{
	EnsureInit();
	if (!CellInGrid(gx, gy))
		return;
	const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
	if (!e->inUse)
		return;
	// An adopted cell belongs to the game now; the tick retires it when the
	// game has finished with it, not the preload table's own bookkeeping.
	if (e->state == ZONE_STATE_NATIVE_A || e->state == ZONE_STATE_NATIVE_B_LOADING
	    || e->state == ZONE_STATE_NATIVE_ACTIVE || e->state == ZONE_STATE_RETIRING)
		return;
	RetireAndRelease(gx, gy, ElapsedSec());
}


// =========================================================================
// Insertion into the pending-activation set
// =========================================================================

// The preload table's slot for a cell, or -1. Short scan: the table holds a
// few dozen entries at most.
static int FindPreloadedSlot(void* zoneEntry)
{
	for (int i = 0; i < numPreloaded; ++i)
		if (preloadedZones[i].zoneEntry == zoneEntry)
			return i;
	return -1;
}

// True when the cell is in the pending set once this returns, whether this
// call put it there or found it already present. A cell already in the set
// is one the game is going to process either way, so both answers mean the
// same thing to the caller: it is no longer the mod's.
static bool InsertIntoPendingSet(void* zoneMgr, void* zoneEntry)
{
	if (!fn_addToTrackingSet || !zoneMgr || !zoneEntry)
		return false;
	void*  value  = zoneEntry;
	void** pValue = &value;
	unsigned char outPair[16];
	memset(outPair, 0, sizeof(outPair));
	fn_addToTrackingSet((void*)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_processingNewActiveZones, OFF_ZM_SET_A)),
	                    outPair, &value, &pValue);
	if (outPair[8] == 0)
		g_insertDuplicate++;
	return true;
}

// Everything both adoption routes share once the cell is in the set.
static void MarkAdopted(int gx, int gy, double now)
{
	int cell = ZoneCell(gx, gy);
	if (!ZonePrepLedgerSetState(&g_zonePrepLedger, gx, gy, ZONE_STATE_NATIVE_A, now))
		g_ledgerRefused++;
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, gx, gy, ZONE_CLASS_ADOPTED);
	if (cell >= 0)
	{
		g_cells[cell].admittedAt     = now;
		g_cells[cell].lastAdmittedAt = now;
		NativeListAdd(cell);
	}
	// From here the cell is the game's, and the game's own expiry is what
	// would take it away again; the retention lease is what stops that
	// happening the moment it lands.
	ZoneRetentionNoteAdopted(gx, gy, now);
	g_adoptedThisFrame = true;
	g_admitted++;
}


// =========================================================================
// Real-demand takeover
// =========================================================================

static volatile LONG g_offMainCalls = 0;

static void NoteOffMainCall()
{
	if (InterlockedIncrement(&g_offMainCalls) == 1)
		LogMsgDeferrable("Zone adoption: activateZoneMap reached from a thread that is not the main one");
}

bool ZoneHandoffTownGuardRefuses(int activationType, float deactivationTimer)
{
	if (!IsMainThread())
	{
		NoteOffMainCall();
		return false;
	}
	if (!townGuardEnabled)
		return false;
	if (!ZoneTownGuardRefuses(activationType, deactivationTimer))
		return false;
	g_townRefusals++;
	return true;
}

bool ZoneHandoffTakeover(void* zoneMgr, void* zoneEntry, int activationType)
{
	// Every known caller of the detoured function is this thread. Insertion
	// can rehash a set the game may be walking elsewhere, so an unexpected
	// caller is refused and reported rather than served.
	if (!IsMainThread())
	{
		NoteOffMainCall();
		return false;
	}
	EnsureInit();
	if (!zoneMgr || !zoneEntry)
		return false;

	int gx = GetZoneGridX(zoneEntry);
	int gy = GetZoneGridY(zoneEntry);
	if (!CellInGrid(gx, gy))
		return false;

	// The entry is read in place, so the stage has to be copied out before
	// adoption rewrites it.
	const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
	int fromState = e->state;

	ZoneTakeoverInputs in;
	in.mainThread        = true;   // checked above
	in.modHoldsPrivately = e->inUse
	                    && e->identity.worldEpoch == g_worldEpoch
	                    && (e->state == ZONE_STATE_PRIVATE_SHELL
	                     || e->state == ZONE_STATE_PRIVATE_CONTENT
	                     || e->state == ZONE_STATE_PRIVATE_NAV
	                     || e->state == ZONE_STATE_READY_FOR_ADOPTION
	                     || e->state == ZONE_STATE_GEOMETRY_INVALIDATED);
	in.flagsPrivate      = IsZoneLoading(zoneEntry) && !IsZoneAccessible(zoneEntry);
	int phase = GetZoneState(zoneMgr);
	in.loaderWalkingSetA = (phase == 2 || phase == 3);

	ZoneTakeoverVerdict v = ZoneTakeoverDecide(in);
	if (v == ZONE_TAKEOVER_NO)
		return false;
	if (v == ZONE_TAKEOVER_DEFER)
	{
		ZonePrepLedgerSetPendingAdoption(&g_zonePrepLedger, gx, gy, true);
		// Always set explicitly, not only in the valid-type case: otherwise an
		// out-of-range type here would reuse whatever an earlier deferral left
		// behind, as keepType at the consuming end (:585).
		g_cells[ZoneCell(gx, gy)].pendingType =
			(activationType >= 0 && activationType < ZONE_ACTIVATION_COUNT) ? activationType : -1;
		g_takeoverDeferred++;
		return false;
	}

	if (!InsertIntoPendingSet(zoneMgr, zoneEntry))
		return false;

	double now = ElapsedSec();
	// The activation that got here has just written this cell's countdown for
	// its own type; that one is a live lease and stays. The other two were
	// written while the cell was private and have not ticked since.
	if (activationType >= 0 && activationType < ZONE_ACTIVATION_COUNT)
		ClearCountdowns(zoneEntry, activationType);
	else
		ClearCountdowns(zoneEntry, -1);

	MarkAdopted(gx, gy, now);
	if (activationType >= 0 && activationType < ZONE_ACTIVATION_COUNT)
		g_takeovers[activationType]++;

	// The preload table stops reasoning about a cell the game owns.
	int slot = FindPreloadedSlot(zoneEntry);
	if (slot >= 0)
		DropTrackedZone(slot);

	{
		std::ostringstream ss;
		ss << "Zone takeover (" << gx << "," << gy << "): type=" << activationType
		   << " from " << ZoneLifecycleStateName(fromState);
		LogDebug(ss.str());
	}
	return true;
}


// =========================================================================
// Cohort admission
// =========================================================================

// The four main-side physics queue counts. The flag the engine keeps beside
// them is the back side's answer and lags a frame; the counts are what phase
// 3 tests, and an activation clears the flag anyway.
bool ZoneHandoffPhysicsCountsClear()
{
	uintptr_t physics = *(uintptr_t*)((uintptr_t)GameAddr(RVA_PAUSESTATE_PHYSICS));
	if (!physics)
		return false;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToMake_mainThreadData_count, 432)) > 0) return false;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_actorsToDestroy_mainThreadData_count, 680)) > 0) return false;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_terrainToLoad_mainThreadData_count, 760)) > 0) return false;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToDestroy_mainThreadData_count, 512)) > 0) return false;
	return true;
}

// The engine's own per-zone readiness answer, asked of the original rather
// than of the mod's detour: the cohort must not admit a cell whose mesh is
// out, because the game publishes accessibility for the whole active set at
// once and accessibility decides character height as well as activation.
static bool MeshReportedIn(int gx, int gy)
{
	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!sectionMgr || !orig_isContentPending)
		return false;
	int coords[2] = { gx, gy };
	return orig_isContentPending((void*)sectionMgr, (void*)coords) != 0;
}

static bool ContentInitialized(void* zoneEntry)
{
	void* content = *(void**)zoneEntry;
	if (!content)
		return false;
	int firstTime = 0, loaded = 0, activation = 0;
	if (!ReadContentLifeFlags(content, &firstTime, &loaded, &activation))
		return false;
	return loaded != 0;
}

// Prepared cells that pass the gate become ready; ready cells are what the
// cohort collects.
static void AdvanceReady(void* zoneMgr, double now)
{
	for (int i = 0; i < numPreloaded; ++i)
	{
		void* ze = preloadedZones[i].zoneEntry;
		if (!ze || !preloadedZones[i].pending)
			continue;
		int gx = preloadedZones[i].gridX;
		int gy = preloadedZones[i].gridY;
		if (!CellInGrid(gx, gy))
			continue;

		const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
		if (!e->inUse || e->identity.worldEpoch != g_worldEpoch)
			continue;
		if (e->state != ZONE_STATE_PRIVATE_NAV)
			continue;

		ZoneAdmissionInputs in;
		in.preparedAndReady   = true;
		in.contentInitialized = ContentInitialized(ze);
		in.flagsPrivate       = IsZoneLoading(ze) && !IsZoneAccessible(ze);
		in.meshReportedIn     = in.contentInitialized && in.flagsPrivate && MeshReportedIn(gx, gy);
		in.reAdmissionAllowed = ZoneReAdmissionAllowed(now, g_cells[ZoneCell(gx, gy)].lastAdmittedAt,
		                                               ZONE_ADOPT_REENTRY_SEC);
		if (ZoneAdmissionDecide(in) == ZONE_ADMIT_NO)
			continue;

		if (ZonePrepLedgerSetState(&g_zonePrepLedger, gx, gy, ZONE_STATE_READY_FOR_ADOPTION, now))
			g_cells[ZoneCell(gx, gy)].readyAt = now;
		else
			g_ledgerRefused++;
	}
	(void)zoneMgr;
}

static void AdmitCohort(void* zoneMgr, double now)
{
	ZoneIdentity ready[ZONE_ADOPT_COHORT_MAX];
	int n = ZonePrepLedgerCollectReady(&g_zonePrepLedger, ready, ZONE_ADOPT_COHORT_MAX);
	if (ZonePrepLedgerCountInState(&g_zonePrepLedger, ZONE_STATE_READY_FOR_ADOPTION) > n)
		g_cohortSaturated++;
	if (n <= 0)
		return;

	int inserted = 0;
	for (int k = 0; k < n; ++k)
	{
		int gx = ready[k].cellX;
		int gy = ready[k].cellY;
		if (ready[k].worldEpoch != g_worldEpoch)
			continue;

		void* ze = GetZoneEntry(zoneMgr, gx, gy);
		if (!ze)
			continue;
		// The gate ran on an earlier frame for some of these; re-read the
		// flags rather than trust them.
		if (!IsZoneLoading(ze) || IsZoneAccessible(ze))
			continue;
		if (!InsertIntoPendingSet(zoneMgr, ze))
			continue;

		ClearCountdowns(ze, -1);
		MarkAdopted(gx, gy, now);
		int slot = FindPreloadedSlot(ze);
		if (slot >= 0)
			DropTrackedZone(slot);
		inserted++;
	}

	if (inserted > 0)
	{
		g_cohorts++;
		std::ostringstream ss;
		ss << "Zone cohort admitted: " << inserted << " cell(s), setB="
		   << ZoneCycleSetBSize(zoneMgr);
		LogMsg(ss.str());
	}
}


// =========================================================================
// Deferred demands
// =========================================================================

// A demand that arrived while the loader was walking the set was recorded
// rather than served. Answer it once the loader is idle: the caller that
// asked may not poll, and until the cell is handed over it is one the game
// cannot reach at all.
//
// Idle, not merely "not walking". This runs from a call that is a sibling of
// the loader's, never nested inside it, so no walk can be live here whatever
// the phase — but a cell inserted after the pending set has been drained
// would sit there unscheduled, because the only thing that starts a cycle is
// the activation tail's phase 0 -> 1 write, which a direct insert bypasses.
// Waiting for an idle loader costs a few frames and puts both adoption
// routes on one rule.
static void ConsumePendingAdoptions(void* zoneMgr, double now)
{
	if (GetZoneState(zoneMgr) != 0)
		return;

	for (int i = 0; i < numPreloaded; ++i)
	{
		void* ze = preloadedZones[i].zoneEntry;
		if (!ze)
			continue;
		int gx = preloadedZones[i].gridX;
		int gy = preloadedZones[i].gridY;
		if (!CellInGrid(gx, gy))
			continue;

		const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
		if (!e->inUse || !e->pendingAdoption || e->identity.worldEpoch != g_worldEpoch)
			continue;

		if (!IsZoneLoading(ze) || IsZoneAccessible(ze))
		{
			// Somebody else settled the cell while the demand waited.
			ZonePrepLedgerSetPendingAdoption(&g_zonePrepLedger, gx, gy, false);
			continue;
		}
		if (!InsertIntoPendingSet(zoneMgr, ze))
			continue;

		// Same rule as an immediate takeover: the deferred demand's own
		// activation already wrote its slot, and the other two are frozen.
		ClearCountdowns(ze, g_cells[ZoneCell(gx, gy)].pendingType);
		MarkAdopted(gx, gy, now);   // clears pendingAdoption with the state change
		int slot = FindPreloadedSlot(ze);
		if (slot >= 0)
			DropTrackedZone(slot);
		g_pendingAdopted++;
	}
}


// =========================================================================
// Following the game's own bookkeeping
// =========================================================================

static void FollowNative(void* zoneMgr, double now)
{
	for (int i = g_nativeCount - 1; i >= 0; --i)
	{
		int cell = g_native[i];
		int gx = cell / (ZONE_GRID_MAX + 1);
		int gy = cell % (ZONE_GRID_MAX + 1);
		const ZonePrepEntry* e = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy);
		if (!e->inUse || e->identity.worldEpoch != g_worldEpoch)
		{
			NativeListRemoveAt(i);
			continue;
		}

		void* ze = GetZoneEntry(zoneMgr, gx, gy);
		if (!ze)
		{
			NativeListRemoveAt(i);
			continue;
		}

		bool f176 = IsZoneLoading(ze);
		bool f177 = IsZoneAccessible(ze);
		// A published, resident cell has nothing left to observe and cannot
		// be retiring, and the membership test walks the whole set, so the
		// common case skips it.
		if (e->state == ZONE_STATE_NATIVE_ACTIVE && f177)
			continue;
		bool inSetB = ZoneInSetB(zoneMgr, ze);
		int  before = e->state;
		ZonePrepLedgerObserveCell(&g_zonePrepLedger, gx, gy, f176, f177, inSetB, now);
		int  after  = ZonePrepLedgerGetConst(&g_zonePrepLedger, gx, gy)->state;

		if (before != ZONE_STATE_NATIVE_ACTIVE && after == ZONE_STATE_NATIVE_ACTIVE
		    && g_cells[cell].readyAt > 0.0 && g_cells[cell].admittedAt > 0.0)
			ZoneAdoptionLatencyRecord(&g_latency, g_cells[cell].readyAt,
			                          g_cells[cell].admittedAt, now);

		// The game has let the cell go: no flags, not in either set. Nothing
		// records a journey that ended here — no convention exists for the
		// latency of a cell that never became active, and none is invented.
		if (!f176 && !f177 && !inSetB && !ZoneInSetA(zoneMgr, ze))
		{
			if (RetireAndRelease(gx, gy, now))
			{
				g_cells[cell].readyAt    = 0.0;
				g_cells[cell].admittedAt = 0.0;
				NativeListRemoveAt(i);
			}
		}
	}
}


// =========================================================================
// The tick
// =========================================================================

void ZoneHandoffNoteWorldState(void* zoneMgr)
{
	EnsureInit();
	if (!zoneMgr)
		return;
	bool justLoaded = *(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;
	ZonePrepLedgerSetGameOwnedBypass(&g_zonePrepLedger, justLoaded);
}

void ZoneHandoffTick(void* zoneMgr, double now)
{
	EnsureInit();
	if (!zoneMgr)
		return;

	bool justLoaded = ZonePrepLedgerGameOwnedBypass(&g_zonePrepLedger);

	FollowNative(zoneMgr, now);
	ConsumePendingAdoptions(zoneMgr, now);

	ZoneCohortWindow w;
	w.mainThread         = IsMainThread();
	w.loaderIdle         = GetZoneState(zoneMgr) == 0;
	w.transitionQuiet    = !isTransitionActive
	                    && InterlockedCompareExchange(&transitionEndPending, 0, 0) == 0;
	w.worldQuiet         = !justLoaded;
	w.centralZoneValid   = *(void**)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_centralZone, OFF_ZM_CURRENT_ZONE)) != NULL;
	w.physicsQueuesClear = ZoneHandoffPhysicsCountsClear();

	// The cycle-in-flight latch: idle at the top of this tick means whatever
	// cycle it was tracking has finished, so it starts clear; an admission
	// below (cohort or takeover, either sets g_adoptedThisFrame) then raises
	// it fresh if this tick is the one that starts a new cycle.
	if (w.loaderIdle)
		g_cycleFromAdoption = false;

	// The stats block below prints unconditionally on its own 60 s timer,
	// not gated on the cohort window: nest skips and the phase-2 wedge
	// scenario this counter exists to catch both happen precisely while the
	// window is closed, so gating the line on it would hide the nest
	// counters in exactly the case they matter most. loaderIdle/physClear/
	// transQuiet are the window's own predicates, read as they stand, so a
	// silent cohort is explained rather than merely absent.
	if (ZoneCohortWindowOpen(w))
	{
		AdvanceReady(zoneMgr, now);
		AdmitCohort(zoneMgr, now);
	}

	if (g_adoptedThisFrame)
		g_cycleFromAdoption = true;

	if (now >= g_nextStatsLog)
	{
		g_nextStatsLog = now + 60.0;
		std::ostringstream ss;
		ss << "ZoneAdopt: cohorts=" << g_cohorts << " admitted=" << g_admitted
		   << " takeover=" << g_takeovers[ZONE_ACTIVATION_CAMERA]
		   << "/" << g_takeovers[ZONE_ACTIVATION_PLAYER]
		   << "/" << g_takeovers[ZONE_ACTIVATION_TOWN]
		   << " defer=" << g_takeoverDeferred << "/" << g_pendingAdopted
		   << " townRefuse=" << g_townRefusals
		   << " retired=" << g_retired
		   << " dup=" << g_insertDuplicate
		   << " natFull=" << g_nativeFull
		   << " cohortFull=" << g_cohortSaturated
		   << " refused=" << g_ledgerRefused
		   << " nest=" << NestValidationSkippedCount() << "/" << NestValidationRevalidatedCount()
		   << "/" << NestValidationDestroyedCount()
		   << " loaderIdle=" << (w.loaderIdle ? 1 : 0)
		   << " physClear=" << (w.physicsQueuesClear ? 1 : 0)
		   << " transQuiet=" << (w.transitionQuiet ? 1 : 0)
		   << " adoptLatency n=" << ZoneAdoptionLatencySampleCount(&g_latency)
		   << std::fixed << std::setprecision(0)
		   << " p99ready2admit=" << ZoneAdoptionLatencyP99ReadyToAdmitted(&g_latency)
		   << "ms p99admit2active=" << ZoneAdoptionLatencyP99AdmittedToActive(&g_latency)
		   << "ms p99ready2active=" << ZoneAdoptionLatencyP99ReadyToActive(&g_latency) << "ms";
		LogMsg(ss.str());
	}
}

#endif // ZONEHAND_STEP >= 2
