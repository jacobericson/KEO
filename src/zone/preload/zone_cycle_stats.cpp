#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/preload/zone_cycle_stats.h"

#if ZONEHAND_STEP >= 1

#include "zone/preload/zone_cycle_math.h"
#include "base/core.h"
#include "game/game.h"
#include "zone/grid.h"
#include "zone/preload/preload.h"
#include <sstream>
#include <iomanip>

// Everything here runs on the main thread: processLoading has one caller
// (ZoneManager::updateMainThread) and the lease sample rides the zone-life
// tick. No interlocked counters, no deferred logging.

static ZoneCycleMachine g_cycle;
static bool   g_cycleInit   = false;
static double g_lastSample  = 0.0;   // ElapsedSec at the previous processLoading sample
static long   g_pauseRestores = 0;   // escape-menu pauses taken back since startup
static long   g_cyclesSeen  = 0;

static ZonePercentile g_pctWait3;
static ZonePercentile g_pctOgre;
static ZonePercentile g_pctPublish;
static ZonePercentile g_pctTotal;
static double g_nextSummary = 0.0;

#ifdef ZONEOPT_DEBUG
static const double CYCLE_SUMMARY_INTERVAL = 60.0;
static const double LEASE_SAMPLE_INTERVAL  = 10.0;
#else
static const double CYCLE_SUMMARY_INTERVAL = 120.0;
static const double LEASE_SAMPLE_INTERVAL  = 30.0;
#endif

// Cells the lease sample has seen at +176/+177 = 1/0, and when it first did.
// Sample-resolution only: a cell that turns private and back between two
// samples is never seen at all.
static double g_privateSince[ZONE_GRID_COUNT];
static bool   g_privateInit = false;
static double g_nextLease   = 0.0;

static void EnsureInit()
{
	if (g_cycleInit)
		return;
	ZoneCycleInit(&g_cycle);
	ZonePercentileReset(&g_pctWait3);
	ZonePercentileReset(&g_pctOgre);
	ZonePercentileReset(&g_pctPublish);
	ZonePercentileReset(&g_pctTotal);
	g_cycleInit = true;
}

// The boost::unordered_set walk game.cpp uses for membership, reading every
// member instead of looking for one. `outNativeLoading` counts Set B members
// whose flags read +176=1 / +177=0. Returns the set's own size_, or -1 when
// the table does not read as a set. POD only: MSVC 2010 refuses __try in a
// function with objects that need unwinding.
static int WalkSetBFlags(uintptr_t set, int* outNativeLoading)
{
	int size = -1;
	int nativeLoading = 0;
	GuardEnter();
	__try
	{
		unsigned long long n = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_size_, OFF_SET_SIZE));
		if (n <= (unsigned long long)ZONE_GRID_COUNT * 4)
		{
			size = (int)n;
			unsigned long long bucketCount = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_bucket_count_, OFF_SET_BUCKET_COUNT));
			uintptr_t buckets = *(uintptr_t*)(KLIB_MEMBER(2, set, ZoneSetTable_buckets_, OFF_SET_BUCKETS));
			if (n > 0 && buckets && bucketCount > 0 && bucketCount < (1ull << 24))
			{
				uintptr_t node = *(uintptr_t*)(buckets + 8 * bucketCount);
				int iter = 0;
				int maxIter = size + 16;
				while (node && iter++ < maxIter)
				{
					uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_value_base_, OFF_SET_NODE_VALUE);
					if (zone)
					{
						if (IsZoneLoading((void*)zone) && !IsZoneAccessible((void*)zone))
							nativeLoading++;
					}
					node = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_next_, 0);
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		size = -1;
		nativeLoading = 0;
	}
	GuardLeave();
	if (outNativeLoading)
		*outNativeLoading = nativeLoading;
	return size;
}

int ZoneCycleSetBSize(void* zoneMgr)
{
	if (!zoneCycleStatsEnabled || !zoneMgr)
		return -1;
	return WalkSetBFlags(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_activeZones, OFF_ZM_SET_B), NULL);
}

// The three ways a Set A member fails ZoneManager::loadPhase2's own gate
// (0xA0D890): content == NULL (the else branch calls ZoneMap::_activate), the
// try-shared-lock on content's boost::shared_mutex (+0xE0) is refused, or
// content->loaded (+0xD8) reads 0. Any of the three makes loadPhase2 re-enter
// content->vtable[4] (processContent) forever instead of advancing to phase 3.
enum ZoneWedgeTest
{
	ZWT_CONTENT_NULL = 0,
	ZWT_LOCK_REFUSED,
	ZWT_NOT_LOADED,
	ZWT_UNREADABLE       // the guarded read itself faulted; not one of loadPhase2's own tests
};

static const char* ZoneWedgeTestName(int t)
{
	switch (t)
	{
	case ZWT_CONTENT_NULL: return "contentNull";
	case ZWT_LOCK_REFUSED: return "lockRefused";
	case ZWT_NOT_LOADED:   return "notLoaded";
	default:               return "unreadable";
	}
}

static const size_t OFF_ZMC_LOADED = 0xD8;
static const size_t OFF_ZMC_MUTEX  = 0xE0;
KLIB_ASSERT_OFFSET(ZoneMapContent_loaded, OFF_ZMC_LOADED);
KLIB_ASSERT_OFFSET(ZoneMapContent_mutex,  OFF_ZMC_MUTEX);

struct ZoneWedgeMember
{
	int gx, gy;
	int test;
};

const int ZC_WEDGE_MAX_MEMBERS = 12;   // one report per phase episode; enough to name every cell in a normal-sized Set A

// Read-only walk of a Set A/B-shaped boost::unordered_set<ZoneMap*>, applying
// loadPhase2's own three tests to each member and recording the ones that
// fail. Never takes an exclusive lock and never writes to the zone, the
// content or the set: this is measurement racing the real loadPhase2, not a
// second copy of it. A shared-lock refusal is reported as the test that
// failed, not retried -- the point is to see what the game itself would have
// seen, not to force a sample through.
static int WalkSetAWedge(uintptr_t set, ZoneWedgeMember* out, int cap, int* outTotal)
{
	int found = 0;
	int total = -1;
	GuardEnter();
	__try
	{
		unsigned long long n = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_size_, OFF_SET_SIZE));
		if (n <= (unsigned long long)ZONE_GRID_COUNT * 4)
		{
			total = (int)n;
			unsigned long long bucketCount = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_bucket_count_, OFF_SET_BUCKET_COUNT));
			uintptr_t buckets = *(uintptr_t*)(KLIB_MEMBER(2, set, ZoneSetTable_buckets_, OFF_SET_BUCKETS));
			if (n > 0 && buckets && bucketCount > 0 && bucketCount < (1ull << 24))
			{
				uintptr_t node = *(uintptr_t*)(buckets + 8 * bucketCount);
				int iter = 0;
				int maxIter = (int)n + 16;
				while (node && iter++ < maxIter)
				{
					uintptr_t zone = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_value_base_, OFF_SET_NODE_VALUE);
					if (zone && found < cap)
					{
						uintptr_t content = *(uintptr_t*)KLIB_MEMBER(2, zone, ZoneMap_mapContent, OFF_ZONE_CONTENT);
						int test = -1;
						if (!content)
						{
							test = ZWT_CONTENT_NULL;
						}
						else if (!fn_boostUnlockShared)
						{
							test = ZWT_UNREADABLE;   // can't safely release a lock we'd take
						}
						else
						{
							volatile LONG* state = (volatile LONG*)(content + OFF_ZMC_MUTEX);
							if (!BoostTryLockShared(state))
							{
								test = ZWT_LOCK_REFUSED;
							}
							else
							{
								unsigned char loaded = *(volatile unsigned char*)(content + OFF_ZMC_LOADED);
								fn_boostUnlockShared((void*)state);
								if (!loaded)
									test = ZWT_NOT_LOADED;
							}
						}
						if (test >= 0)
						{
							out[found].gx = GetZoneGridX((void*)zone);
							out[found].gy = GetZoneGridY((void*)zone);
							out[found].test = test;
							found++;
						}
					}
					node = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_next_, 0);
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		found = 0;
		total = -1;
	}
	GuardLeave();
	if (outTotal)
		*outTotal = total;
	return found;
}

// Whether the mod's own preload table lists this cell (game-owned, registered,
// pending or handed off) -- the field that tells a wedged member's origin
// apart from a cell the game's own streaming loaded.
static bool CellIsPreloadedByMod(int gx, int gy)
{
	int n = numPreloaded;
	if (n > MAX_PRELOADED) n = MAX_PRELOADED;
	if (n < 0) n = 0;
	for (int i = 0; i < n; ++i)
	{
		if (preloadedZones[i].gridX == gx && preloadedZones[i].gridY == gy)
			return true;
	}
	return false;
}

// One-shot report for a cycle stuck in one loadingPhase past the wedge
// threshold. Log only: no zone flag, no tracking set and no phase are ever
// written here. Publishing a zone flag before the game's own gate accepts it
// masks incomplete loading and can corrupt the navmesh permanently; this
// function exists to name the stuck member, not to unstick it.
static void EmitWedgeReport(void* zoneMgr, int phase, double dwellMs)
{
	int setA = -1;
	ZoneWedgeMember members[ZC_WEDGE_MAX_MEMBERS];
	int failCount = 0;
	if (phase == 2)
	{
		uintptr_t setAPtr = KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_processingNewActiveZones, OFF_ZM_SET_A);
		failCount = WalkSetAWedge(setAPtr, members, ZC_WEDGE_MAX_MEMBERS, &setA);
	}
	// Not ZoneCycleSetBSize: that helper is itself gated on zoneCycleStats, and
	// this report must carry a setBsz= reading even when only zoneWedgeGuard
	// is on (the PROD default).
	int setB = WalkSetBFlags(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_activeZones, OFF_ZM_SET_B), NULL);

	std::ostringstream ss;
	ss << "ZoneWedge: phase=" << phase << std::fixed << std::setprecision(1)
	   << " dwellMs=" << dwellMs << " setAsz=";
	if (setA >= 0) ss << setA; else ss << "-";
	ss << " setBsz=";
	if (setB >= 0) ss << setB; else ss << "-";
	ss << " fails=" << failCount;
	if (failCount >= ZC_WEDGE_MAX_MEMBERS)
		ss << "(truncated at " << ZC_WEDGE_MAX_MEMBERS << ")";
	LogMsg(ss.str());

	for (int i = 0; i < failCount; ++i)
	{
		std::ostringstream sm;
		sm << "ZoneWedgeMember: (" << members[i].gx << "," << members[i].gy << ")"
		   << " test=" << ZoneWedgeTestName(members[i].test)
		   << " modPrepared=" << (CellIsPreloadedByMod(members[i].gx, members[i].gy) ? 1 : 0);
		LogMsg(sm.str());
	}
}

static void AppendMs(std::ostringstream& ss, const char* name, double ms)
{
	ss << " " << name << "=" << std::fixed << std::setprecision(1) << ms;
}

static void PrintCycle(const ZoneCycleTotals& t)
{
	std::ostringstream ss;
	ss << "ZoneCycle: cause=" << ZoneCycleCauseName(t.cause)
	   << " frames=" << t.frames << "/" << t.phaseFrames
	   << " setBsz=";
	if (t.setBAtEntry >= 0) ss << t.setBAtEntry;
	else                    ss << "-";
	ss << " natLoad=" << t.nativeLoading;
	AppendMs(ss, "total", t.totalMs);
	AppendMs(ss, "p1", t.phaseMs[1]);
	AppendMs(ss, "p2", t.phaseMs[2]);
	AppendMs(ss, "p3wait", t.phaseMs[3]);
	AppendMs(ss, "p4", t.phaseMs[4]);
	AppendMs(ss, "p5", t.phaseMs[5]);
	AppendMs(ss, "ogre", t.ogreUnloadMs);
	AppendMs(ss, "publish", t.publishMs);
	ss << " restart=" << t.restarts
	   << " escPause=" << g_pauseRestores
	   << " desync=" << g_cycle.desyncs;
	LogMsg(ss.str());
}

static void PrintSummary()
{
	std::ostringstream ss;
	ss << "ZoneCycleSum: n=" << g_cyclesSeen << std::fixed << std::setprecision(1)
	   << " p3wait=" << ZonePercentileValue(&g_pctWait3, 50) << "/"
	   << ZonePercentileValue(&g_pctWait3, 90) << "/" << g_pctWait3.max
	   << " ogre=" << ZonePercentileValue(&g_pctOgre, 50) << "/"
	   << ZonePercentileValue(&g_pctOgre, 90) << "/" << g_pctOgre.max
	   << " publish=" << ZonePercentileValue(&g_pctPublish, 50) << "/"
	   << ZonePercentileValue(&g_pctPublish, 90) << "/" << g_pctPublish.max
	   << " total=" << ZonePercentileValue(&g_pctTotal, 50) << "/"
	   << ZonePercentileValue(&g_pctTotal, 90) << "/" << g_pctTotal.max
	   << " escPause=" << g_pauseRestores;
	if (g_pctTotal.dropped)
		ss << " dropped=" << g_pctTotal.dropped;
	LogMsg(ss.str());
}

void ZoneCycleOnProcessLoading(void* zoneMgr, int phaseBefore, int phaseAfter, double callMs)
{
	bool statsOn = zoneCycleStatsEnabled;
	bool wedgeOn = zoneWedgeGuardEnabled;
	if ((!statsOn && !wedgeOn) || !zoneMgr)
		return;
	EnsureInit();

	double now = ElapsedSec();
	double dwellMs = (g_lastSample > 0.0) ? (now - g_lastSample) * 1000.0 : 0.0;
	g_lastSample = now;
	// A dwell that spans a load screen or a breakpoint is not phase time.
	if (dwellMs > 10000.0)
		dwellMs = 0.0;

	bool justLoaded = *(unsigned char*)(KLIB_MEMBER(1, (uintptr_t)zoneMgr,
		ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;

	// The dwell/phase machine itself is pure arithmetic and always runs (cheap,
	// and the wedge guard needs it even with zoneCycleStats off); everything
	// past this point that walks the game's own sets is behind statsOn.
	ZoneCycleStep step = ZoneCycleSample(&g_cycle, phaseBefore, phaseAfter,
	                                     dwellMs, callMs, justLoaded);

	if (step.closed && statsOn)
	{
		g_cyclesSeen++;
		ZonePercentileAdd(&g_pctWait3, step.done.phaseMs[3]);
		ZonePercentileAdd(&g_pctOgre, step.done.ogreUnloadMs);
		ZonePercentileAdd(&g_pctPublish, step.done.publishMs);
		ZonePercentileAdd(&g_pctTotal, step.done.totalMs);
		PrintCycle(step.done);
	}

	if (statsOn)
	{
		if (step.opened)
		{
			int nativeLoading = 0;
			int setB = WalkSetBFlags(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_activeZones, OFF_ZM_SET_B),
			                         &nativeLoading);
			ZoneCycleNoteSetB(&g_cycle, setB);
			if (nativeLoading > 0)
				ZoneCycleNoteNativeLoading(&g_cycle);
		}
		else if (phaseAfter != 0)
		{
			// One walk per call while a cycle runs: Set B is tens of entries and
			// a cycle is a handful of frames.
			int nativeLoading = 0;
			WalkSetBFlags(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_activeZones, OFF_ZM_SET_B),
			              &nativeLoading);
			if (nativeLoading > 0)
				ZoneCycleNoteNativeLoading(&g_cycle);
		}
	}

	if (wedgeOn && step.wedgeFire)
		EmitWedgeReport(zoneMgr, step.wedgePhase, step.wedgeDwellMs);

	if (statsOn)
	{
		if (g_nextSummary == 0.0)
			g_nextSummary = now + CYCLE_SUMMARY_INTERVAL;
		else if (now >= g_nextSummary)
		{
			g_nextSummary = now + CYCLE_SUMMARY_INTERVAL;
			if (g_cyclesSeen > 0)
				PrintSummary();
		}
	}
}

void ZoneCyclePauseRestored()
{
	if (!zoneCycleStatsEnabled)
		return;
	++g_pauseRestores;
}

void ZoneCycleSampleLeases(void* zoneMgr, double now)
{
	if (!zoneCycleStatsEnabled || !zoneMgr || !IsMainThread())
		return;
	if (now < g_nextLease)
		return;
	g_nextLease = now + LEASE_SAMPLE_INTERVAL;

	if (!g_privateInit)
	{
		for (int i = 0; i < ZONE_GRID_COUNT; ++i)
			g_privateSince[i] = -1.0;
		g_privateInit = true;
	}

	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return;
	unsigned int count = GetPlayerCharCount(playerIntf);
	uintptr_t* stuff   = GetPlayerCharStuff(playerIntf);
	if (!stuff || count == 0 || count > 2000)
		return;

	int privChars = 0;       // characters in a 1/0 cell the game does not track
	int natChars  = 0;       // characters in a 1/0 cell that is in Set A or Set B
	int privCells = 0;
	double oldest = -1.0;
	int oldestX = -1, oldestY = -1;
	bool seen[ZONE_GRID_COUNT];
	for (int i = 0; i < ZONE_GRID_COUNT; ++i)
		seen[i] = false;

	for (unsigned int i = 0; i < count; ++i)
	{
		uintptr_t character = stuff[i];
		if (!character)
			continue;
		int gx, gy;
		if (!WorldToZoneGrid(GetCharPosX(character), GetCharPosZ(character), &gx, &gy))
			continue;
		void* ze = GetZoneEntry(zoneMgr, gx, gy);
		if (!ze)
			continue;
		if (!IsZoneLoading(ze) || IsZoneAccessible(ze))
			continue;

		// The flags alone do not say whose the cell is: the game's own
		// activation also writes 1/0. A cell in neither tracking set is the
		// mod's private hold.
		bool tracked = ZoneInSetA(zoneMgr, ze) || ZoneInSetB(zoneMgr, ze);
		int cell = gy + gx * 64;
		if (tracked)
		{
			natChars++;
			g_privateSince[cell] = -1.0;
			continue;
		}

		privChars++;
		if (!seen[cell])
		{
			seen[cell] = true;
			privCells++;
			if (g_privateSince[cell] < 0.0)
				g_privateSince[cell] = now;
			double held = now - g_privateSince[cell];
			if (held > oldest)
			{
				oldest = held;
				oldestX = gx;
				oldestY = gy;
			}
		}
	}

	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		if (!seen[c] && g_privateSince[c] >= 0.0)
			g_privateSince[c] = -1.0;

	if (privChars == 0 && natChars == 0)
		return;

	std::ostringstream ss;
	ss << "ZonePriv: chars=" << privChars << " cells=" << privCells
	   << " natLoad=" << natChars << " held=";
	if (oldest >= 0.0) ss << (long)(oldest + 0.5) << "s at (" << oldestX << "," << oldestY << ")";
	else               ss << "-";
	ss << " setBsz=" << ZoneCycleSetBSize(zoneMgr);
	LogMsg(ss.str());
}

#endif // ZONEHAND_STEP >= 1
