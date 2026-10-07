#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/retention/zone_expiry_guard.h"

#if ZONEHAND_STEP >= 3

#include "zone/retention/zone_expiry_guard_policy.h"
#include "zone/retention/zone_retention.h"
#include "navmesh/nm_workers.h"
#include "base/core.h"
#include "game/game.h"
#include "zone/preload/preload_internal.h"
#include <sstream>
#include <iomanip>

// When each cell's current hold began and when it was last renewed, negative
// for none.
static double g_holdStart[ZONE_GRID_COUNT];
static double g_holdLast[ZONE_GRID_COUNT];
static bool   g_init = false;

static long   g_holds             = 0;   // expiries held because a claim named the cell
static long   g_fenced            = 0;   // expiries run under the publication
static long   g_idle              = 0;   // run before any dispatch, or with caching off
static long   g_unfenced          = 0;   // run where no fence can ever exist
static double g_maxHoldS          = 0.0;
static int    g_fencedThisFrame   = 0;
static int    g_maxFencedPerFrame = 0;
static bool   g_warned            = false;

static void ClearStamps()
{
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
	{
		g_holdStart[c] = -1.0;
		g_holdLast[c]  = -1.0;
	}
}

void ZoneExpiryGuardInit()
{
	if (g_init)
		return;
	ClearStamps();
	g_init = true;
}

void ZoneExpiryGuardOnWorldReset()
{
	ZoneExpiryGuardInit();
	ClearStamps();
	g_fencedThisFrame = 0;
}

void ZoneExpiryGuardBeginFrame()
{
	g_fencedThisFrame = 0;
}

static int CellOf(void* zoneEntry, int* gx, int* gy)
{
	*gx = GetZoneGridX(zoneEntry);
	*gy = GetZoneGridY(zoneEntry);
	return ZoneCell(*gx, *gy);
}

static void Hold(void* zoneEntry)
{
	ZoneRetentionWriteTownHold(zoneEntry);
	g_holds++;
	int gx = 0, gy = 0;
	int cell = CellOf(zoneEntry, &gx, &gy);
	if (cell < 0)
		return;
	double now = ElapsedSec();
	if (ZoneExpiryHoldStartsEpisode(g_holdLast[cell], now))
		g_holdStart[cell] = now;
	g_holdLast[cell] = now;
	double held = now - g_holdStart[cell];
	if (held > g_maxHoldS)
		g_maxHoldS = held;
	if (g_warned || !ZoneExpiryHoldOverdue(held))
		return;
	g_warned = true;
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1)
	   << "Zone expiry guard: cell (" << gx << "," << gy << ") held " << held
	   << " s for a navmesh job still working on it; it stays held until the job ends (said once per session)";
	LogError(ss.str());
}

static void EndEpisode(void* zoneEntry)
{
	int gx = 0, gy = 0;
	int cell = CellOf(zoneEntry, &gx, &gy);
	if (cell < 0)
		return;
	g_holdStart[cell] = -1.0;
	g_holdLast[cell]  = -1.0;
}

bool ZoneExpiryGuardUpdate(void* zoneEntry, ZoneMapUpdateFn original)
{
	ZoneExpiryGuardInit();

	// Lock-free first: a claim seen here holds without the queue lock. Its
	// "not claimed" decides nothing; the begin below re-reads under the lock.
	if (NavMeshZoneClaimed(zoneEntry))
	{
		Hold(zoneEntry);
		return original(zoneEntry);
	}

	NavMeshUnloadFence fence;
	NmFenceResult fr = fence.TryBegin(zoneEntry, NM_FENCE_MODE_CLAIMS);
	ZoneExpiryAction act = ZoneExpiryOnBegin(fr);
	if (act == ZONE_EXPIRY_HOLD)
	{
		Hold(zoneEntry);
		return original(zoneEntry);
	}

	EndEpisode(zoneEntry);
	if (act == ZONE_EXPIRY_RUN_UNFENCED)
	{
		if (fr == NM_FENCE_IDLE)
			g_idle++;
		else
			g_unfenced++;
		return original(zoneEntry);
	}

	g_fenced++;
	if (++g_fencedThisFrame > g_maxFencedPerFrame)
		g_maxFencedPerFrame = g_fencedThisFrame;
	bool kept = original(zoneEntry);
	fence.Release();
	return kept;
}

std::string ZoneExpiryGuardStatsFragment()
{
	std::ostringstream ss;
	ss << " expGuard=" << g_holds << "/" << g_fenced << "/" << g_idle << "/" << g_unfenced
	   << std::fixed << std::setprecision(1) << " maxHoldS=" << g_maxHoldS
	   << " maxFencedPerFrame=" << g_maxFencedPerFrame;
	return ss.str();
}

#endif // ZONEHAND_STEP >= 3
