#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/retention/zone_retention.h"

#if ZONEHAND_STEP >= 3

#include "zone/retention/zone_retention_policy.h"
#include "zone/retention/zone_retention_ledger.h"
#include "zone/retention/zone_expiry_guard.h"
#include "zone/handoff/zone_adoption_seam.h"
#include "zone/handoff/zone_handoff.h"
#include "zone/zone_life.h"
#include "base/core.h"
#include "game/game.h"
#include "zone/preload/preload_internal.h"
#include "movement/tracking.h"
#include <string.h>
#include <sstream>
#include <iomanip>

static ZoneRetentionLedger g_ledger;
static bool g_init = false;

// The frame's own state. g_zoneMgr doubles as "a frame has been opened": the
// prologue does nothing before the first BeginFrame, so the pacing rules
// always have a frame to be measured against.
static void*  g_zoneMgr         = NULL;
static double g_frameNow        = 0.0;
// An expensive decision was already made this frame. The two costly steps —
// walking every player character for a live anchor answer, and the queue-mutex
// acquisition inside the unload publication — both descend from the same
// branch, so one flag caps both at one per frame however that branch ends.
static bool   g_probedThisFrame = false;
static bool   g_pressure        = false;
static double g_lastReleaseAt   = -1.0;
static double g_nextMapRefresh  = 0.0;
static double g_nextStatsLog    = 0.0;

static const double ZONE_RETENTION_MAP_REFRESH_SEC = 1.0;
static const double ZONE_RETENTION_STATS_SEC       = 60.0;
// CollectMoverRetainZones writes a cell per watched mover and one per
// destination; this bounds what the tick reads back, not what it collects.
static const int    ZONE_RETENTION_MOVER_CELLS     = 64;
// A cell counts as held while it was held this recently. Wider than the
// refresh interval, so a cell held every frame is never missed by a sample
// that lands just before its next hold.
static const double ZONE_RETENTION_HELD_WINDOW_SEC = 2.0;

// When each cell was last held, or negative for never. The cap counts these:
// a per-frame fact like this one belongs here rather than in the ledger,
// whose every field is per-adoption lease state.
static double g_heldAt[ZONE_GRID_COUNT];

// The refusal backoff, per cell: how many times in a row this cell's fences
// have refused, and the time before which it is not asked again.
static int    g_deferStreak[ZONE_GRID_COUNT];
static double g_deferQuietUntil[ZONE_GRID_COUNT];

static long g_holds       = 0;   // frames a cell was kept past its native expiry
static long g_releases    = 0;
static long g_unfenced    = 0;   // released without the navmesh fences (they cannot exist here)
static long g_defers[ZONE_RETENTION_DEFER_COUNT] = { 0, 0 };
static long g_leases      = 0;   // prediction leases written
static long g_adopted     = 0;
// A tripwire, expected to stay 0 for as long as the prologue's arithmetic
// matches the original's: nothing between the two can change a countdown, the
// frame delta or the loading phase. It exists to notice a game build where
// that stops being true, and is printed only when it fires.
static long g_kept        = 0;
// Asks the backoff suppressed, and the longest refusal streak any cell has
// reached. Printed unconditionally: a field that appeared only once the
// backoff engaged could not tell a session that never deferred from a build
// whose backoff does not work.
static long g_deferQuiet     = 0;
static int  g_deferStreakMax = 0;

static void ClearBackoff()
{
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
	{
		g_deferStreak[c]     = 0;
		g_deferQuietUntil[c] = -1.0;
	}
}

void ZoneRetentionInit()
{
	if (g_init)
		return;
	ZoneRetentionLedgerInit(&g_ledger);
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		g_heldAt[c] = -1.0;
	ClearBackoff();
	ZoneExpiryGuardInit();
	g_init = true;
}

void ZoneRetentionOnWorldReset()
{
	ZoneRetentionInit();
	ZoneRetentionLedgerClearAll(&g_ledger);
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		g_heldAt[c] = -1.0;
	ClearBackoff();
	ZoneExpiryGuardOnWorldReset();
	g_zoneMgr         = NULL;
	g_frameNow        = 0.0;
	g_probedThisFrame = false;
	g_pressure        = false;
	g_lastReleaseAt   = -1.0;
	g_nextMapRefresh  = 0.0;
	g_nextStatsLog    = 0.0;
}

void ZoneRetentionBeginFrame(void* zoneMgr)
{
	ZoneRetentionInit();
	g_zoneMgr         = zoneMgr;
	g_frameNow        = ElapsedSec();
	g_probedThisFrame = false;
	ZoneExpiryGuardBeginFrame();
}

static bool CellInGrid(int gx, int gy)
{
	return gx >= 0 && gx <= ZONE_GRID_MAX && gy >= 0 && gy <= ZONE_GRID_MAX;
}

void ZoneRetentionNoteAdopted(int gx, int gy, double now)
{
	ZoneRetentionInit();
	if (!CellInGrid(gx, gy))
		return;
	// The revisit ladder is read before the reset and applied after it: the
	// reset clears the three per-adoption deadlines, the ladder is not one of
	// them.
	double grace = ZoneRetentionLedgerNoteRevisit(&g_ledger, gx, gy, now);
	ZoneIdentity id;
	id.worldEpoch         = ZoneHandoffWorldEpoch();
	id.cellX              = gx;
	id.cellY              = gy;
	id.contentIncarnation = 0;
	ZoneRetentionLedgerReset(&g_ledger, gx, gy, id, now);
	ZoneRetentionLedgerSetMinResidence(&g_ledger, gx, gy, now + ZONE_RETENTION_MIN_RESIDENCE_SEC);
	ZoneRetentionLedgerSetGrace(&g_ledger, gx, gy, now + grace);
	{
		// A fresh lease: whatever refused the last time this cell was held
		// says nothing about this one.
		int c = ZoneCell(gx, gy);
		if (c >= 0)
		{
			g_deferStreak[c]     = 0;
			g_deferQuietUntil[c] = -1.0;
		}
	}
	g_adopted++;
}

void ZoneRetentionNoteRetired(int gx, int gy)
{
	ZoneRetentionInit();
	if (CellInGrid(gx, gy))
		ZoneRetentionLedgerRetire(&g_ledger, gx, gy);
}


// =========================================================================
// The prologue's decision
// =========================================================================

static float* Countdowns(void* zoneEntry)
{
	return (float*)(KLIB_MEMBER(2, (uintptr_t)zoneEntry, ZoneMap_activatedCountdown, OFF_ZONE_COUNTDOWNS));
}

// Leaves the town countdown at a value the native decrement turns into the
// hold margin. Chosen because nothing reads that slot as a lease:
// getDeactivationCountdown, which is what a town's coverage refresh passes
// on, is the larger of the camera and player slots only.
void ZoneRetentionWriteTownHold(void* zoneEntry)
{
	float frameDelta = *(const float*)((uintptr_t)GameAddr(RVA_GLOBAL_FRAME_TIME));
	Countdowns(zoneEntry)[ZONE_ACTIVATION_TOWN] = ZoneRetentionHoldValue(frameDelta);
}

// A retention hold: the write, the stamp the cap counts, and the count.
static void WriteHold(void* zoneEntry, int cell)
{
	ZoneRetentionWriteTownHold(zoneEntry);
	if (cell >= 0)
		g_heldAt[cell] = g_frameNow;
	g_holds++;
}

// Cells the policy is keeping alive by itself right now.
static int HeldCount(double now)
{
	int n = 0;
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		if (g_heldAt[c] >= 0.0 && now - g_heldAt[c] <= ZONE_RETENTION_HELD_WINDOW_SEC)
			n++;
	return n;
}

bool ZoneExpiresThisFrame(void* zoneEntry)
{
	if (!zoneEntry || !g_zoneMgr)
		return false;

	// Past phase 1 the original keeps every cell without touching a
	// countdown, so there is nothing to decide and nothing a write would do
	// but sit there undecremented.
	if (GetZoneState(g_zoneMgr) > 1)
		return false;

	const float* cd = Countdowns(zoneEntry);
	float frameDelta = *(const float*)((uintptr_t)GameAddr(RVA_GLOBAL_FRAME_TIME));
	return ZoneRetentionNativeWouldExpireThisFrame(cd[ZONE_ACTIVATION_CAMERA], cd[ZONE_ACTIVATION_PLAYER],
	                                               cd[ZONE_ACTIVATION_TOWN], frameDelta);
}

ZoneRetentionAnswer ZoneRetentionAnswerFor(void* zoneEntry)
{
	if (!zone::g_zoneCfg.zoneRetentionEnabled)
		return ZONE_RETENTION_ANSWER_NOT_MINE;
	int gx = GetZoneGridX(zoneEntry);
	int gy = GetZoneGridY(zoneEntry);
	if (!CellInGrid(gx, gy))
		return ZONE_RETENTION_ANSWER_NOT_MINE;

	int cell = ZoneCell(gx, gy);
	const ZoneRetentionEntry* e = ZoneRetentionLedgerGetConst(&g_ledger, gx, gy);
	double now = g_frameNow;

	ZoneRetentionCellInputs in;
	in.tracked           = e->inUse && e->identity.worldEpoch == ZoneHandoffWorldEpoch();
	in.anchorsUnknown    = !ZlRetentionReadable();
	in.readerPinned      = e->readerPinCount > 0;
	in.minResidenceLive  = e->minResidenceDeadline > now;
	in.discretionaryLive = e->graceDeadline > now || e->predictionLeaseDeadline > now;
	// The lifecycle pass's own map: the camera's and the players' cells, the
	// watched movers' current and next cells and everything in the mod's
	// working tables, each stamped at its configured radius, up to a second
	// old. Outside pressure a cell the map marks holds without the live read;
	// a cell it misses is decided by the live check below, at the same radii,
	// which is what decides a release.
	in.mapRetained       = ZlRetentionNear(cell);
	in.underPressure     = g_pressure;
	in.pacingAllows      = ZoneRetentionPacingAllows(g_probedThisFrame, ZoneHandoffAdoptedThisFrame(),
	                                                 g_lastReleaseAt, now, g_pressure);

	ZoneRetentionPrecheck pre = ZoneRetentionPrecheckCell(in);
	if (pre == ZONE_RETENTION_PRE_PASS)
		return ZONE_RETENTION_ANSWER_NOT_MINE;
	if (pre == ZONE_RETENTION_PRE_HOLD)
	{
		WriteHold(zoneEntry, cell);
		return ZONE_RETENTION_ANSWER_HELD;
	}

	// A cell whose fences have just refused stands down before it is asked
	// again. Checked before the frame's probe budget is spent, so a cell in
	// its window costs one comparison and leaves the budget for another cell.
	if (cell >= 0 && g_deferQuietUntil[cell] > now)
	{
		g_deferQuiet++;
		WriteHold(zoneEntry, cell);
		return ZONE_RETENTION_ANSWER_HELD;
	}

	// Everything past here is expensive, whichever way it ends: the anchor
	// walk below reads every player character, and a release goes on to take
	// the unload publication's queue mutex, which blocks. Stamping the frame
	// here rather than on a completed release is what keeps both to one per
	// frame -- a cell that holds, or one whose fences refuse, has spent the
	// frame's budget just as surely as one that went.
	g_probedThisFrame = true;

	// An anchor it cannot read answers "near", which is a hold.
	bool anchorInRange = ZlAnchorsNearCell(g_zoneMgr, gx, gy,
	                                       ZoneRetentionLiveRadius(g_pressure, zone::g_zoneCfg.cfg_zoneLifeRetainRadius),
	                                       ZoneRetentionLiveRadius(g_pressure, zone::g_zoneCfg.cfg_zoneLifeSquadRadius));
	if (ZoneRetentionFinalVerdict(anchorInRange) == ZONE_RETENTION_HOLD)
	{
		// Proximity renews the grace, so a cell that has just been beside the
		// player does not release the moment it stops being.
		ZoneRetentionLedgerSetGrace(&g_ledger, gx, gy,
		                            now + ZoneRetentionGraceSecondsForLevel(e->revisitBackoffLevel));
		WriteHold(zoneEntry, cell);
		return ZONE_RETENTION_ANSWER_HELD;
	}
	return ZONE_RETENTION_ANSWER_RELEASE;
}

void ZoneRetentionHoldInstead(void* zoneEntry, ZoneRetentionDefer reason)
{
	if (!zoneEntry)
		return;
	g_defers[reason]++;
	int cell = ZoneCell(GetZoneGridX(zoneEntry), GetZoneGridY(zoneEntry));
	if (cell >= 0)
	{
		int streak = ++g_deferStreak[cell];
		if (streak > g_deferStreakMax)
			g_deferStreakMax = streak;
		g_deferQuietUntil[cell] = g_frameNow + ZoneRetentionDeferBackoffSeconds(streak);
	}
	WriteHold(zoneEntry, cell);
}

void ZoneRetentionNoteReleased(void* zoneEntry, bool expired, bool fenced)
{
	if (!zoneEntry)
		return;
	// The fences let this cell through, whichever way the original went, so
	// the refusal streak that built the backoff window is over.
	{
		int c = ZoneCell(GetZoneGridX(zoneEntry), GetZoneGridY(zoneEntry));
		if (c >= 0)
		{
			g_deferStreak[c]     = 0;
			g_deferQuietUntil[c] = -1.0;
		}
	}
	if (!expired)
	{
		// The policy predicted the expiry branch and the original kept the
		// cell anyway. Nothing to undo — the cell stays tracked and the next
		// frame decides again — but the prediction and the branch have
		// diverged and that is worth a number.
		g_kept++;
		return;
	}
	int gx = GetZoneGridX(zoneEntry);
	int gy = GetZoneGridY(zoneEntry);
	if (CellInGrid(gx, gy))
	{
		ZoneRetentionLedgerNoteEvicted(&g_ledger, gx, gy, g_frameNow);
		ZoneRetentionLedgerRetire(&g_ledger, gx, gy);
		int cell = ZoneCell(gx, gy);
		if (cell >= 0)
			g_heldAt[cell] = -1.0;
	}
#ifdef KEO_DEBUG
	// One line per actual release: the pacing rules (spacing, adoption-frame
	// exclusion) apply across frames, so only a per-release stamp -- not the
	// 60 s aggregate below -- can show whether they held. `adopt` is a
	// tripwire, not a live sample: ZoneRetentionPacingAllows already refuses
	// a release on any frame where an admission set the flag, so this line
	// can only be reached with it clear. Expected to read 0 always; a 1 here
	// means that guard broke.
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3);
		ss << "ZoneRelease: (" << gx << "," << gy << ") t=" << g_frameNow << " dt=";
		if (g_lastReleaseAt >= 0.0)
			ss << (g_frameNow - g_lastReleaseAt);
		else
			ss << "-";
		ss << " adopt=" << (ZoneHandoffAdoptedThisFrame() ? 1 : 0);
		LogDebug(ss.str());
	}
#endif
	g_lastReleaseAt = g_frameNow;
	g_releases++;
	if (!fenced)
		g_unfenced++;
}


// =========================================================================
// The policy tick
// =========================================================================

bool ZoneRetentionUnderPressure()
{
	return g_pressure;
}

// Every watched character's stored cells buy a lease, renewed while the
// entry stands and left to run out when it goes. At a squad radius of 0 only
// a mover's do: a stationary entry's stored cell can be one its character
// has left, and the squads keep no ring to lease.
static void RenewPredictionLeases(double now)
{
	int gx[ZONE_RETENTION_MOVER_CELLS];
	int gy[ZONE_RETENTION_MOVER_CELLS];
	bool moving[ZONE_RETENTION_MOVER_CELLS];
	int n = CollectMoverRetainZones(gx, gy, moving, ZONE_RETENTION_MOVER_CELLS);
	bool squadRing = zone::g_zoneCfg.cfg_zoneLifeSquadRadius >= 1;
	for (int i = 0; i < n; ++i)
	{
		if (!CellInGrid(gx[i], gy[i]))
			continue;
		if (!moving[i] && !squadRing)
			continue;
		if (!ZoneRetentionLedgerGetConst(&g_ledger, gx[i], gy[i])->inUse)
			continue;
		ZoneRetentionLedgerSetPredictionLease(&g_ledger, gx[i], gy[i],
		                                      now + ZONE_RETENTION_PREDICTION_LEASE_SEC);
		g_leases++;
	}
}

void ZoneRetentionTick(void* zoneMgr, double now)
{
	ZoneRetentionInit();
	if (!zoneMgr || !IsMainThread())
		return;

	if (now >= g_nextMapRefresh)
	{
		g_nextMapRefresh = now + ZONE_RETENTION_MAP_REFRESH_SEC;
		ZlRetentionRefresh(zoneMgr);
		RenewPredictionLeases(now);
		int cap = zone::g_zoneCfg.cfg_zoneRetentionMaxHeld;
		g_pressure = ZoneRetentionPressureNext(g_pressure, HeldCount(now), cap, ZoneRetentionLowWater(cap));
	}

	if (now >= g_nextStatsLog)
	{
		g_nextStatsLog = now + ZONE_RETENTION_STATS_SEC;
		std::ostringstream ss;
		ss << "ZoneRetain: tracked=" << ZoneRetentionLedgerCountInUse(&g_ledger)
		   << " held=" << HeldCount(now)
		   << " adopted=" << g_adopted
		   << " holds=" << g_holds
		   << " release=" << g_releases
		   << " unfenced=" << g_unfenced
		   << " defer=" << g_defers[ZONE_RETENTION_DEFER_NAV] << "/" << g_defers[ZONE_RETENTION_DEFER_PJ]
		   << " defQuiet=" << g_deferQuiet << "/" << g_deferStreakMax
		   << " lease=" << g_leases
		   << " pressure=" << (g_pressure ? 1 : 0)
		   << " cap=" << zone::g_zoneCfg.cfg_zoneRetentionMaxHeld
		   << " r=" << zone::g_zoneCfg.cfg_zoneLifeRetainRadius << "/" << zone::g_zoneCfg.cfg_zoneLifeSquadRadius
		   << " fzh=" << (GameFastZoneHopping() ? "on" : "off")
		   << " anchors=" << (ZlRetentionReadable() ? 1 : 0);
		ss << ZoneExpiryGuardStatsFragment();
		if (g_kept)
			ss << " kept=" << g_kept;
		LogMsg(ss.str());
	}
}

#endif // ZONEHAND_STEP >= 3
