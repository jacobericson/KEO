// astar_hier.cpp - the optional hierarchical arm around the original findPathFull call.
//
// AstarHierSearch binds astar_hier_policy's sequence to the original call on whichever thread
// calls findPathFull, then publishes the record through Interlocked counters and a capped event
// ring: no lock, no allocation and no logging there. AstarHierTick, on the main thread, prints.

#include "pathfind/astar_hier.h"
#include "pathfind/astar_hier_policy.h"
#include "pathfind/astar_cost.h"   // AstarResolveCallerClass, AstarLogBucket
#include "game/game.h"             // game::g_hookOrig

namespace astar_hier_detail {

// One findPathFull call's arguments: the context every operation below reads.
struct AhCall
{
	void* coll;
	void* in;
	void* out;
};

// One event slot. The search thread that claims it writes every field, then valid; the main
// tick prints a slot only once valid reads 1.
struct AhEvent
{
	volatile LONG valid;
	int   kind;
	float startX, startZ, goalX, goalZ;
	int   hierStatus, hierCause, hierIters;
	int   vanStatus, vanCause, vanIters;
};

} // namespace astar_hier_detail
using namespace astar_hier_detail;

static const size_t AH_IN_START = 0x10;   // FindPathInput start point, x y z
static const size_t AH_IN_GOALS = 0x20;   // FindPathInput goal point array
static const int    AH_EVENT_SLOTS = 32;

// Session totals. Any search thread increments them; the main tick reads each on its own, so
// a line printed during a search may be one search apart between two counters.
static volatile LONG g_ahPlayer = 0;
static volatile LONG g_ahHierOk = 0;
static volatile LONG g_ahLosMasked = 0;
static volatile LONG g_ahHierTerm = 0;
static volatile LONG g_ahHierUnreach = 0;
static volatile LONG g_ahHierInvalid = 0;
static volatile LONG g_ahHierOther = 0;
static volatile LONG g_ahInstRefuse = 0;
static volatile LONG g_ahPlFalse = 0;
static volatile LONG g_ahFallback = 0;
static volatile LONG g_ahRescued = 0;
static volatile LONG g_ahBothFail = 0;
static volatile LONG g_ahKeptTerm = 0;
static volatile LONG g_ahNpcRefuse = 0;
static volatile LONG g_ahNpcShadow = 0;
static volatile LONG g_ahNpcFalse = 0;
static volatile LONG g_ahNpcTrue = 0;
static volatile LONG g_ahNpcUndec = 0;
static volatile LONG g_ahLabelDisagree = 0;
static volatile LONG g_ahIterPairs = 0;

static volatile LONG     g_ahHierIter[ASTAR_HIST_BUCKETS];
static volatile LONG     g_ahVanIter[ASTAR_HIST_BUCKETS];
static volatile LONG     g_ahRatio[AHIER_RATIO_BUCKETS];
static volatile LONG     g_ahIterRatio[AHIER_ITER_RATIO_BUCKETS];
static volatile LONGLONG g_ahHierTicks = 0;
static volatile LONGLONG g_ahVanTicks = 0;

// The NPC shadow sampler: a refusal sequence and the QPC stamp of the last shadow, claimed by
// compare-exchange so two threads never both take one slot.
static volatile LONG     g_ahNpcRefusalSeq = 0;
static volatile LONGLONG g_ahLastShadowTicks = 0;

static AhEvent       g_ahEvents[AH_EVENT_SLOTS];
static volatile LONG g_ahEventsClaimed = 0;

// Main thread only: the printed-line state.
static LONG g_ahPrintedTotal = -1;
static LONG g_ahEventsPrinted = 0;

// =========================================================================
// The operations the policy's sequence calls (the search thread)
// =========================================================================

static void AhSearch(void* ctx)
{
	AhCall* c = (AhCall*)ctx;
	game::g_hookOrig.orig_findPathFull(c->coll, c->in, c->out);
}

static void AhSetHierByte(void* ctx, unsigned char value)
{
	AhCall* c = (AhCall*)ctx;
	*((unsigned char*)c->in + AHIER_IN_HIER_FLAG) = value;
}

static void AhReset(void* ctx)
{
	AhCall* c = (AhCall*)ctx;
	AstarHierResetOutput((unsigned char*)c->out);
}

static void AhRead(void* ctx, AstarHierResult* out)
{
	AhCall* c = (AhCall*)ctx;
	AstarHierReadOutput((const unsigned char*)c->out, out);
}

static long long AhNow(void*)
{
	return QpcNow();
}

// One call per NPC instant refusal: at most one in four refusals, and one shadow per 100 ms
// across every thread.
static bool AhNpcSampleDue(void*)
{
	unsigned long seq = (unsigned long)InterlockedIncrement(&g_ahNpcRefusalSeq);
	LONGLONG now = QpcNow();
	LONGLONG last = g_ahLastShadowTicks;
	if (!AstarHierNpcSampleDue(seq, now, last, qpcFrequency.QuadPart / 10))
		return false;
	return InterlockedCompareExchange64(&g_ahLastShadowTicks, now, last) == last;
}

static AstarHierOps AhOps(AhCall* call)
{
	AstarHierOps ops;
	ops.ctx = call;
	ops.search = AhSearch;
	ops.setHierByte = AhSetHierByte;
	ops.resetOutput = AhReset;
	ops.readOutput = AhRead;
	ops.now = AhNow;
	ops.npcSampleDue = AhNpcSampleDue;
	return ops;
}

// =========================================================================
// Publishing one record (the search thread)
// =========================================================================

static void AhBump(volatile LONG* counter, bool on)
{
	if (on)
		InterlockedIncrement(counter);
}

static void AhPublishCounters(const AstarHierBumps& b, const AstarHierRecord& rec, int playerByReq)
{
	AhBump(&g_ahPlayer, b.player);
	AhBump(&g_ahHierOk, b.hierOk);
	AhBump(&g_ahLosMasked, b.losMasked);
	AhBump(&g_ahHierTerm, b.hierTerm);
	AhBump(&g_ahHierUnreach, b.hierUnreach);
	AhBump(&g_ahHierInvalid, b.hierInvalid);
	AhBump(&g_ahHierOther, b.hierOther);
	AhBump(&g_ahInstRefuse, b.instRefuse);
	AhBump(&g_ahPlFalse, b.plFalse);
	AhBump(&g_ahFallback, b.fallback);
	AhBump(&g_ahRescued, b.rescued);
	AhBump(&g_ahBothFail, b.bothFail);
	AhBump(&g_ahKeptTerm, b.keptTerm);
	AhBump(&g_ahNpcRefuse, b.npcRefuse);
	AhBump(&g_ahNpcShadow, b.npcShadow);
	AhBump(&g_ahNpcFalse, b.npcFalse);
	AhBump(&g_ahNpcTrue, b.npcTrue);
	AhBump(&g_ahNpcUndec, b.npcUndec);
	AhBump(&g_ahIterPairs, b.iterPair);
	AhBump(&g_ahLabelDisagree, (rec.subject == AHIER_SUBJECT_PLAYER && playerByReq == 0)
	                        || (rec.subject == AHIER_SUBJECT_NPC && playerByReq == 1));
}

static void AhPublishHistograms(AstarHierMode mode, const AstarHierRecord& rec)
{
	if (rec.hierRan)
		InterlockedIncrement(&g_ahHierIter[AstarLogBucket(rec.hier.iters, ASTAR_HIST_BUCKETS)]);
	if (rec.vanRan)
		InterlockedIncrement(&g_ahVanIter[AstarLogBucket(rec.van.iters, ASTAR_HIST_BUCKETS)]);
	if (rec.ratioValid)
		InterlockedIncrement(&g_ahRatio[AstarHierRatioBucket(rec.ratio)]);
	float iterRatio = AstarHierIterRatio(mode, rec);
	if (iterRatio >= 0.0f)
		InterlockedIncrement(&g_ahIterRatio[AstarHierIterRatioBucket(iterRatio)]);
	if (rec.hierTicks)
		InterlockedExchangeAdd64(&g_ahHierTicks, rec.hierTicks);
	if (rec.vanTicks)
		InterlockedExchangeAdd64(&g_ahVanTicks, rec.vanTicks);
}

// Claims one of the capped slots, or none once every slot is taken; valid is stored last.
static void AhRecordEvent(AstarHierEvent kind, const AstarHierRecord& rec, const unsigned char* in)
{
	if (g_ahEventsClaimed >= AH_EVENT_SLOTS)
		return;
	LONG idx = InterlockedIncrement(&g_ahEventsClaimed) - 1;
	if (idx < 0 || idx >= AH_EVENT_SLOTS)
		return;
	AhEvent& e = g_ahEvents[idx];
	const float* start = (const float*)(in + AH_IN_START);
	const float* goal = *(const float* const*)(in + AH_IN_GOALS);
	e.kind = (int)kind;
	e.startX = start[0];
	e.startZ = start[2];
	e.goalX = goal ? goal[0] : 0.0f;
	e.goalZ = goal ? goal[2] : 0.0f;
	e.hierStatus = rec.hier.status;
	e.hierCause = rec.hier.cause;
	e.hierIters = rec.hier.iters;
	e.vanStatus = rec.van.status;
	e.vanCause = rec.van.cause;
	e.vanIters = rec.van.iters;
	InterlockedExchange(&e.valid, 1);
}

// =========================================================================
// The search (whichever thread calls findPathFull)
// =========================================================================

void AstarHierSearch(void* streamingCollection, void* findPathInput, void* findPathOutput,
                     void* returnAddr, int playerByReq)
{
	const AstarHierMode mode = (AstarHierMode)pathfind::g_pathfindCfg.playerHierarchicalMode;
	AstarHierSubject subject = AHIER_SUBJECT_OTHER;
	bool costModifier = false;
	if (mode != AHIER_OFF && findPathInput)
	{
		const unsigned char* in = (const unsigned char*)findPathInput;
		const bool atSite = AstarResolveCallerClass(returnAddr, -1) == ASTAR_CALLER_CHARACTER_UNKNOWN;
		subject = AstarHierSubjectOf(atSite, in[AHIER_IN_HIER_FLAG]);
		costModifier = *(void* const*)(in + AHIER_IN_COST_MODIFIER) != NULL;
	}

	AhCall call;
	call.coll = streamingCollection;
	call.in = findPathInput;
	call.out = findPathOutput;
	AstarHierRecord rec;
	AstarHierRun(mode, (AstarHierOnCap)pathfind::g_pathfindCfg.playerHierOnCapMode, subject, costModifier,
	             AhOps(&call), &rec);
	if (mode == AHIER_OFF || subject == AHIER_SUBJECT_OTHER)
		return;

	AstarHierBumps b;
	AstarHierTally(mode, rec, &b);
	AhPublishCounters(b, rec, playerByReq);
	AhPublishHistograms(mode, rec);
	if (b.event != AHIER_EVENT_NONE)
		AhRecordEvent(b.event, rec, (const unsigned char*)findPathInput);
}

// =========================================================================
// The log lines (main thread)
// =========================================================================

static void AhSnapshot(volatile LONG* src, long* dst, int n)
{
	for (int i = 0; i < n; ++i)
		dst[i] = src[i];
}

// A log2 bucket's upper bound, 2^k.
static void AppendIterPct(std::ostringstream& ss, const long* hist, int pct)
{
	int k = AstarHierPercentileBucket(hist, ASTAR_HIST_BUCKETS, pct);
	if (k < 0)
		ss << "-";
	else
		ss << (1LL << k);
}

static void AppendHundredths(std::ostringstream& ss, int hundredths)
{
	ss << hundredths / 100 << "." << (hundredths % 100) / 10 << hundredths % 10;
}

// A ratio bucket's upper edge, 0.90 + 0.01(k+1); the top bucket has none.
static void AppendRatioPct(std::ostringstream& ss, const long* hist, int pct)
{
	int k = AstarHierPercentileBucket(hist, AHIER_RATIO_BUCKETS, pct);
	if (k < 0)
		ss << "-";
	else if (k == AHIER_RATIO_BUCKETS - 1)
		ss << ">1.37";
	else
		AppendHundredths(ss, 91 + k);
}

// An iteration-ratio bucket's upper edge, 0.05(k+1); the top bucket takes 1.00 and above.
static void AppendIterRatioPct(std::ostringstream& ss, const long* hist, int pct)
{
	int k = AstarHierPercentileBucket(hist, AHIER_ITER_RATIO_BUCKETS, pct);
	if (k < 0)
		ss << "-";
	else if (k == AHIER_ITER_RATIO_BUCKETS - 1)
		ss << ">=1.00";
	else
		AppendHundredths(ss, 5 * (k + 1));
}

static void PrintAstarHierLine(int mode)
{
	long hierIter[ASTAR_HIST_BUCKETS], vanIter[ASTAR_HIST_BUCKETS];
	long ratio[AHIER_RATIO_BUCKETS], iterRatio[AHIER_ITER_RATIO_BUCKETS];
	AhSnapshot(g_ahHierIter, hierIter, ASTAR_HIST_BUCKETS);
	AhSnapshot(g_ahVanIter, vanIter, ASTAR_HIST_BUCKETS);
	AhSnapshot(g_ahRatio, ratio, AHIER_RATIO_BUCKETS);
	AhSnapshot(g_ahIterRatio, iterRatio, AHIER_ITER_RATIO_BUCKETS);

	std::ostringstream ss;
	ss << "AstarHier: mode=" << AstarHierModeName(mode)
	   << " onCap=" << AstarHierOnCapName(pathfind::g_pathfindCfg.playerHierOnCapMode)
	   << " player=" << g_ahPlayer << " hierOk=" << g_ahHierOk << " losMasked=" << g_ahLosMasked
	   << " hierTerm=" << g_ahHierTerm << " hierUnreach=" << g_ahHierUnreach
	   << " hierInvalid=" << g_ahHierInvalid << " hierOther=" << g_ahHierOther
	   << " instRefuse=" << g_ahInstRefuse << " plFalse=" << g_ahPlFalse
	   << " fallback=" << g_ahFallback << " rescued=" << g_ahRescued << " bothFail=" << g_ahBothFail
	   << " keptTerm=" << g_ahKeptTerm << " hierIter=";
	AppendIterPct(ss, hierIter, 50);
	ss << "/";
	AppendIterPct(ss, hierIter, 90);
	ss << " vanIter=";
	AppendIterPct(ss, vanIter, 50);
	ss << "/";
	AppendIterPct(ss, vanIter, 90);
	ss << " ratio=";
	AppendRatioPct(ss, ratio, 50);
	ss << "/";
	AppendRatioPct(ss, ratio, 90);
	ss << " iterRatio=";
	AppendIterRatioPct(ss, iterRatio, 50);
	ss << " iterPairs=" << g_ahIterPairs
	   << " hierMs=" << (LONGLONG)QpcToMs(g_ahHierTicks) << " vanMs=" << (LONGLONG)QpcToMs(g_ahVanTicks)
	   << " npcRefuse=" << g_ahNpcRefuse << " npcShadow=" << g_ahNpcShadow << " npcFalse=" << g_ahNpcFalse
	   << " npcTrue=" << g_ahNpcTrue << " npcUndec=" << g_ahNpcUndec
	   << " labelDisagree=" << g_ahLabelDisagree;
	LogMsg(ss.str());
}

static const char* AhEventName(int kind)
{
	switch (kind)
	{
	case AHIER_EVENT_RESCUE:        return "rescue";
	case AHIER_EVENT_FALSE_REFUSAL: return "falseRefusal";
	case AHIER_EVENT_NPC_FALSE:     return "npcFalse";
	default:                        return "diverge";
	}
}

// Each claimed slot once, in claim order, stopping at the first slot not yet published.
static void PrintAstarHierEvents()
{
	while (g_ahEventsPrinted < AH_EVENT_SLOTS && g_ahEventsPrinted < g_ahEventsClaimed)
	{
		AhEvent& e = g_ahEvents[g_ahEventsPrinted];
		if (InterlockedCompareExchange(&e.valid, 0, 0) == 0)
			return;
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(1);
		ss << "AstarHier event: " << AhEventName(e.kind)
		   << " start=" << e.startX << "," << e.startZ << " goal=" << e.goalX << "," << e.goalZ
		   << " hier=" << e.hierStatus << "/" << e.hierCause << "/" << e.hierIters
		   << " van=" << e.vanStatus << "/" << e.vanCause << "/" << e.vanIters;
		LogMsg(ss.str());
		++g_ahEventsPrinted;
	}
}

void AstarHierTick()
{
	const int mode = pathfind::g_pathfindCfg.playerHierarchicalMode;
	if (mode == AHIER_OFF)
		return;
	LONG total = g_ahPlayer + g_ahNpcRefuse;
	if (total != g_ahPrintedTotal)
	{
		g_ahPrintedTotal = total;
		PrintAstarHierLine(mode);
	}
	PrintAstarHierEvents();
}
