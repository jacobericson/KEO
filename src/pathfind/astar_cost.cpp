// astar_cost.cpp — A* cost logging
//
// See astar_cost.h. Everything here runs on whatever thread calls
// hook_findPathFull -- the path thread, a NavMesh worker, the main thread,
// or (per the generation caller) a NavMesh build thread -- so AstarCostNote
// is interlocked-accumulator-only, with one short spinlock (below) guarding
// the rare "slowest capped search" ring insert. No allocation, no CRT
// string work, no logging outside AstarCostTick (main thread only).

#include "pathfind/astar_cost.h"
#include "plugin/hook_manifest.h"

#include "game/game.h"   // gameBase
#include <cmath>

bool AstarCostHookInstalled()
{
	return HookRowInstalled(HOOK_FIND_PATH_FULL);
}

// =========================================================================
// Caller resolution: return address -> module-relative RVA -> class
// =========================================================================

// Lazily resolved once: gameBase is set by startPlugin before any hook can
// fire, but its SizeOfImage is not stored anywhere else in this plugin.
// Same read-only PE-header walk as core.cpp's ProfilerImageResolve, applied
// to the game module instead of this DLL's own.
// Any cost caller may initialize this immutable image-size cache with an
// aligned scalar store; later callers read it directly. Racing initializers
// compute the same size. No reset, and the scalar cannot tear.
static volatile uintptr_t g_gameImageSize = 0;

static uintptr_t GameImageSize()
{
	uintptr_t size = g_gameImageSize;
	if (size)
		return size;
	if (!gameBase)
		return 0;

	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)gameBase;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(gameBase + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	size = nt->OptionalHeader.SizeOfImage;
	g_gameImageSize = size;   // idempotent recompute if raced; no atomic needed
	return size;
}

AstarCallerClass AstarResolveCallerClass(void* returnAddr, int playerByReq)
{
	uintptr_t addr = (uintptr_t)returnAddr;
	uintptr_t base = gameBase;
	uintptr_t size = GameImageSize();

	bool inExe = (base != 0 && size != 0 && addr >= base && (addr - base) < size);
	unsigned returnRva = inExe ? (unsigned)(addr - base) : 0;

	return AstarClassifyCaller(returnRva, inExe, playerByReq);
}


// =========================================================================
// Per-class accumulation
// =========================================================================

// FindPathOutput status is 0..5; index the raw byte directly and fold
// anything unexpected (there is nothing past 5) into slot 5 rather than
// reading out of bounds.
static const int ASTAR_STATUS_SLOTS = 6;

// One outcome's histogram: a count/ticks sum plus the latency (us) and
// iteration-count buckets (astar_cost_policy.h's log2 bucketing).
namespace astar_cost_detail {
// Any search-thread writer publishes each histogram bucket/count atomically;
// main reports and resets fields independently per window. Mixed diagnostic
// bucket epochs are tolerated, with no coherent struct publication.
struct AstarOutcomeHist
{
	volatile LONG     count;
	volatile LONGLONG ticks;
	volatile LONG     latHistUs[ASTAR_HIST_BUCKETS];
	volatile LONG     iterHist[ASTAR_HIST_BUCKETS];
};

// Reduced cut of "class x status": success, the two terminated/cause=3
// (search-state-full) buckets split by whether the 4x boost wrote
// this search's byte budgets (a boosted search cannot end in cause=2 at the
// unboosted open-set limit, so its cause=3 rate is not directly comparable
// to an unboosted one), and everything else. statusCount keeps the plain
// per-status counts a reader can total independently of this cut.
enum { ASTAR_OUT_OK = 0, ASTAR_OUT_CAUSE3_UNBOOSTED = 1, ASTAR_OUT_CAUSE3_BOOSTED = 2, ASTAR_OUT_OTHER = 3, ASTAR_OUT_COUNT = 4 };

// Any AstarCostNote caller atomically publishes outcome/status counters;
// the main tick reads and resets each field independently per window. No
// coherent set is copied; mixed diagnostic epochs are tolerated.
struct AstarClassStats
{
	volatile LONG   statusCount[ASTAR_STATUS_SLOTS];
	AstarOutcomeHist outcome[ASTAR_OUT_COUNT];
};
} // namespace astar_cost_detail
using namespace astar_cost_detail;

static AstarClassStats g_classStats[ASTAR_CALLER_CLASS_COUNT];

// A non-character (gate/generation/havok-internal/other) search that still
// carries boosted==1: the request-priority tag mismatches the request it
// runs for, or the sticky per-thread player flag was never cleared after a
// prior findPathFull call, so a search that is not a player order gets the
// budget bump anyway. Counted on its own rather than folded into any class's
// histogram, since it is a measurement of the tag's own reliability, not of
// search cost.
static volatile LONG g_staleBoostGate = 0;

// Character-only cap counters: status==3 && cause==3, split player/NPC,
// boosted/unboosted, and direct/edge leg. The leg dimension exists so wiring
// it up later is a classification change, not a new counter shape. It is
// unwired today: the only place that knows whether an order is currently
// travelling an edge leg is a main-thread, per-tracked-slot table with no
// request-pointer key, and this hook runs on the contentStream thread with
// only the request object in hand -- there is no join between the two, so
// every sample here lands in ASTAR_LEG_UNKNOWN.
namespace astar_cost_detail {
enum AstarLegTag { ASTAR_LEG_UNKNOWN = 0, ASTAR_LEG_DIRECT = 1, ASTAR_LEG_EDGE = 2, ASTAR_LEG_COUNT = 3 };
} // namespace astar_cost_detail
using namespace astar_cost_detail;

// Any search caller atomically increments caps; main transition completion
// increments transitions and the main tick reads each scalar separately.
// Session cumulative with no reset; mixed diagnostic totals are tolerated.
static volatile LONG g_capCumulative[2][2][ASTAR_LEG_COUNT];   // [player][boosted][leg]
static volatile LONG g_transitionsCompleted = 0;

// --- Slowest-capped-search ring: spin-guarded, not lock-free ---
//
// Any capped-character search writer inserts under g_slowLock; release
// publishes the set. Main PrintAstarSlowLine takes that same spinlock, copies
// and clears the count each window, then logs outside it. Torn copied sets
// are rejected by mutual exclusion. The writer also reads an unlocked
// minimum/count hint, which may miss a diagnostic insert but cannot alter
// the set without taking the lock. No whole-session payload reset.
namespace astar_cost_detail {
struct AstarSlowEntry
{
	LONGLONG ticks;
	int      iterations;
	int      callerClass;
	int      boosted;
	unsigned startFaceKey;
	unsigned goalFaceKey;
	float    goalDist3D;
};
} // namespace astar_cost_detail
using namespace astar_cost_detail;

static const int ASTAR_SLOW_N = 8;
static AstarSlowEntry     g_slowEntries[ASTAR_SLOW_N];
static volatile LONG      g_slowCount = 0;
static volatile LONG      g_slowLock  = 0;

static void AstarSlowConsider(const AstarCostSample* s, AstarCallerClass cls)
{
	// Cheap pre-check before taking the lock: skip anything that plainly
	// cannot beat the current minimum. Reading g_slowCount/g_slowEntries
	// without the lock here is a hint only; the insert below re-checks
	// under the lock, so a stale hint can only cost a missed insert, never
	// a corrupt one.
	if (g_slowCount >= ASTAR_SLOW_N)
	{
		LONGLONG minTicks = -1;
		for (int i = 0; i < ASTAR_SLOW_N; ++i)
			if (minTicks < 0 || g_slowEntries[i].ticks < minTicks)
				minTicks = g_slowEntries[i].ticks;
		if (s->ticks <= minTicks)
			return;
	}

	// Bounded spin: this insert path is rare (cause==3 only) and each holder
	// does a handful of comparisons, no allocation, no logging.
	while (InterlockedCompareExchange(&g_slowLock, 1, 0) != 0)
		/* spin */;

	int insertAt = -1;
	if (g_slowCount < ASTAR_SLOW_N)
	{
		insertAt = g_slowCount++;
	}
	else
	{
		int minIdx = 0;
		for (int i = 1; i < ASTAR_SLOW_N; ++i)
			if (g_slowEntries[i].ticks < g_slowEntries[minIdx].ticks) minIdx = i;
		if (s->ticks > g_slowEntries[minIdx].ticks)
			insertAt = minIdx;
	}

	if (insertAt >= 0)
	{
		g_slowEntries[insertAt].ticks        = s->ticks;
		g_slowEntries[insertAt].iterations   = s->iterations;
		g_slowEntries[insertAt].callerClass  = (int)cls;
		g_slowEntries[insertAt].boosted      = s->boosted;
		g_slowEntries[insertAt].startFaceKey = s->startFaceKey;
		g_slowEntries[insertAt].goalFaceKey  = s->goalFaceKey;
		g_slowEntries[insertAt].goalDist3D   = s->goalDist3D;
	}

	InterlockedExchange(&g_slowLock, 0);
}


static void NoteOutcome(AstarOutcomeHist* h, LONGLONG ticks, LONGLONG us, int iterations)
{
	InterlockedIncrement(&h->count);
	InterlockedExchangeAdd64(&h->ticks, ticks);
	int latBucket  = AstarLogBucket(us, ASTAR_HIST_BUCKETS);
	int iterBucket = AstarLogBucket((long long)iterations, ASTAR_HIST_BUCKETS);
	InterlockedIncrement(&h->latHistUs[latBucket]);
	InterlockedIncrement(&h->iterHist[iterBucket]);
}

void AstarCostNote(const AstarCostSample* s, AstarCallerClass cls)
{
	if (!s)
		return;

	AstarClassStats* cs = &g_classStats[cls];

	int statusSlot = (s->status >= 0 && s->status < ASTAR_STATUS_SLOTS) ? s->status : (ASTAR_STATUS_SLOTS - 1);
	InterlockedIncrement(&cs->statusCount[statusSlot]);

	LONGLONG us = QpcToUs(s->ticks);
	bool isCause3 = (s->status == 3 && s->cause == 3);

	if (s->status == 1)
	{
		NoteOutcome(&cs->outcome[ASTAR_OUT_OK], s->ticks, us, s->iterations);
	}
	else if (isCause3)
	{
		int outIdx = s->boosted ? ASTAR_OUT_CAUSE3_BOOSTED : ASTAR_OUT_CAUSE3_UNBOOSTED;
		NoteOutcome(&cs->outcome[outIdx], s->ticks, us, s->iterations);
	}
	else
	{
		NoteOutcome(&cs->outcome[ASTAR_OUT_OTHER], s->ticks, us, s->iterations);
	}

	bool isCharacter = (cls == ASTAR_CALLER_CHARACTER_PLAYER ||
	                     cls == ASTAR_CALLER_CHARACTER_NPC ||
	                     cls == ASTAR_CALLER_CHARACTER_UNKNOWN);

	if (isCharacter && isCause3)
	{
		int playerIdx = (cls == ASTAR_CALLER_CHARACTER_PLAYER) ? 1 : 0;
		int boostIdx  = s->boosted ? 1 : 0;
		InterlockedIncrement(&g_capCumulative[playerIdx][boostIdx][ASTAR_LEG_UNKNOWN]);
		AstarSlowConsider(s, cls);
	}
	else if (cls == ASTAR_CALLER_GATE && s->boosted)
	{
		InterlockedIncrement(&g_staleBoostGate);
	}
}

void AstarCostOnTransitionClosed()
{
	InterlockedIncrement(&g_transitionsCompleted);
}


// =========================================================================
// Reporting (main thread only)
// =========================================================================

static const char* ClassName(int cls)
{
	switch (cls)
	{
		case ASTAR_CALLER_CHARACTER_PLAYER:  return "charPlayer";
		case ASTAR_CALLER_CHARACTER_NPC:     return "charNpc";
		case ASTAR_CALLER_CHARACTER_UNKNOWN: return "charUnk";
		case ASTAR_CALLER_GATE:              return "gate";
		case ASTAR_CALLER_GENERATION:        return "generation";
		case ASTAR_CALLER_HAVOK_INTERNAL:    return "havokInternal";
		default:                             return "other";
	}
}

static const char* OutcomeName(int out)
{
	switch (out)
	{
		case ASTAR_OUT_OK:                return "ok";
		case ASTAR_OUT_CAUSE3_UNBOOSTED:  return "cause3";
		case ASTAR_OUT_CAUSE3_BOOSTED:    return "cause3boost";
		default:                          return "other";
	}
}

// Appends "idxNxcount,idxNxcount,..." for every non-empty bucket, read (not
// cleared -- these histograms are session-cumulative, like the rest of
// AstarClassStats) or "-" if none. A bucket index maps to
// astar_cost_policy.h's AstarLogBucket ranges, not printed here since it is
// fixed for the session.
static void AppendNonemptyBuckets(std::ostringstream& ss, volatile LONG* buckets, int n)
{
	bool any = false;
	for (int i = 0; i < n; ++i)
	{
		LONG v = buckets[i];
		if (v == 0)
			continue;
		if (any) ss << ",";
		ss << i << "x" << v;
		any = true;
	}
	if (!any)
		ss << "-";
}

static void PrintAstarCapLine()
{
	std::ostringstream ss;
	ss << "AstarCap:";

	if (!AstarCostHookInstalled())
	{
		ss << " findPathFull=?";
		LogMsg(ss.str());
		return;
	}

	LONG npcUnboosted    = g_capCumulative[0][0][ASTAR_LEG_UNKNOWN];
	LONG npcBoosted      = g_capCumulative[0][1][ASTAR_LEG_UNKNOWN];
	LONG playerUnboosted = g_capCumulative[1][0][ASTAR_LEG_UNKNOWN];
	LONG playerBoosted   = g_capCumulative[1][1][ASTAR_LEG_UNKNOWN];
	LONG staleBoost      = g_staleBoostGate;
	LONG transitions     = g_transitionsCompleted;

	ss << " npc=" << npcUnboosted << "/" << npcBoosted << "(unboosted/boosted)"
	   << " player=" << playerUnboosted << "/" << playerBoosted << "(unboosted/boosted)"
	   << " staleBoost=" << staleBoost
	   << " legs=unwired";

	LONG totalCaps = npcUnboosted + npcBoosted + playerUnboosted + playerBoosted;
	if (transitions > 0)
	{
		std::ostringstream rate;
		rate << std::fixed << std::setprecision(2)
		     << (double)totalCaps * 100.0 / (double)transitions;
		// per100tx divides the session-cumulative cap count above by the
		// session-cumulative count of completed transitions -- a session
		// rate, not a windowed one.
		ss << " per100tx=" << rate.str() << " (tx=" << transitions << ")";
	}
	else
	{
		ss << " per100tx=? (tx=0)";
	}
	LogMsg(ss.str());
}

// Prints the slowest capped searches seen since the previous call, then
// clears the ring, so the next window starts empty rather than accreting
// the whole session's worth of entries under the lock above.
static void PrintAstarSlowLine()
{
	if (!AstarCostHookInstalled())
	{
		LogMsg("AstarSlow: findPathFull=?");
		return;
	}

	AstarSlowEntry snapshot[ASTAR_SLOW_N];
	int count;

	while (InterlockedCompareExchange(&g_slowLock, 1, 0) != 0)
		/* spin */;
	count = g_slowCount;
	for (int i = 0; i < count; ++i)
		snapshot[i] = g_slowEntries[i];
	g_slowCount = 0;
	InterlockedExchange(&g_slowLock, 0);

	std::ostringstream ss;
	ss << "AstarSlow: n=" << count;
	for (int i = 0; i < count; ++i)
	{
		const AstarSlowEntry& e = snapshot[i];
		ss << std::fixed << std::setprecision(1);
		ss << " [" << ClassName(e.callerClass)
		   << " iters=" << e.iterations
		   << " boosted=" << e.boosted
		   << " startFace=0x" << std::hex << e.startFaceKey << std::dec
		   << " goalFace=0x" << std::hex << e.goalFaceKey << std::dec
		   << " dist=" << e.goalDist3D
		   << " svc=" << QpcToMs(e.ticks) << "ms]";
	}
	LogMsg(ss.str());
}

// One LogMsg call per class, each carrying its own "AstarClass:" prefix, so
// a line-based reader never sees an unprefixed continuation line. Empty
// classes (no calls this session) are skipped.
static void PrintAstarClassLine()
{
	if (!pathfind::g_pathfindCfg.pathCostLinesEnabled)
		return;
	if (!AstarCostHookInstalled())
	{
		LogMsg("AstarClass: findPathFull=?");
		return;
	}

	for (int c = 0; c < ASTAR_CALLER_CLASS_COUNT; ++c)
	{
		AstarClassStats* cs = &g_classStats[c];
		LONG n = 0;
		for (int st = 0; st < ASTAR_STATUS_SLOTS; ++st)
			n += cs->statusCount[st];
		if (n == 0)
			continue;

		std::ostringstream ss;
		ss << std::fixed << std::setprecision(1);
		ss << "AstarClass: " << ClassName(c) << " n=" << n
		   << " st=" << cs->statusCount[0] << "/" << cs->statusCount[1] << "/"
		   << cs->statusCount[2] << "/" << cs->statusCount[3] << "/"
		   << cs->statusCount[4] << "/" << cs->statusCount[5]
		   << "(inProgress/ok/unreach/term/trunc/invalid)";

		for (int out = 0; out < ASTAR_OUT_COUNT; ++out)
		{
			AstarOutcomeHist* h = &cs->outcome[out];
			LONG hn = h->count;
			if (hn == 0)
				continue;
			double ms = QpcToMs(h->ticks);
			ss << " " << OutcomeName(out) << "=" << hn << "/" << ms << "ms"
			   << " lat=[";
			AppendNonemptyBuckets(ss, h->latHistUs, ASTAR_HIST_BUCKETS);
			ss << "] iter=[";
			AppendNonemptyBuckets(ss, h->iterHist, ASTAR_HIST_BUCKETS);
			ss << "]";
		}
		LogMsg(ss.str());
	}
}

void AstarCostTick()
{
	PrintAstarCapLine();
	PrintAstarSlowLine();
	PrintAstarClassLine();
}

