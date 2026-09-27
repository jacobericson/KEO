// path_pool_hist.cpp - lock-free path search and pass histograms.
// Any thread records counters; no lock, allocation or log in a recorder.

#include "pathfind/path_pool_internal.h"

namespace path_pool_detail {

void PPHistAdd(PPHist* h, LONGLONG us)
{
	if (us < 0) us = 0;
	int b = 0;
	LONGLONG v = us;
	while (v > 1 && b < PP_HIST_BUCKETS - 1) { v >>= 1; ++b; }
	InterlockedIncrement(&h->buckets[b]);
	InterlockedIncrement(&h->count);
	InterlockedExchangeAdd64(&h->sumUs, us);
	// maxUs is a 32-bit LONG: clamp before the compare, or a sample past
	// ~35 minutes (0x7FFFFFFF us) would store as a negative maximum.
	LONG clamped = (us > 0x7FFFFFFF) ? (LONG)0x7FFFFFFF : (LONG)us;
	for (;;)
	{
		LONG cur = h->maxUs;
		if (clamped <= cur) break;
		if (InterlockedCompareExchange(&h->maxUs, clamped, cur) == cur) break;
	}
}

// Non-destructive. The total is derived from the bucket sum rather than
// h->count: h->count is incremented by PPHistAdd AFTER the bucket increment,
// so a reader racing a writer between those two increments could see a total
// the bucket sum had not caught up to yet -- the loop then never reaches
// `want` and falls through to the last bucket (1<<39 us, ~549,755,813 ms).
// Deriving the total from the bucket sum itself removes the dependency on
// that second counter: both the total and
// the running sum below read the same 40 buckets, so `running` is
// guaranteed to reach `total` by the last bucket even if a writer is
// concurrently incrementing (buckets only ever increase between resets).
LONGLONG PPHistPercentileUs(const PPHist* h, double frac)
{
	LONG total = 0;
	for (int b = 0; b < PP_HIST_BUCKETS; ++b)
		total += h->buckets[b];
	if (total <= 0) return 0;
	LONG want = (LONG)(frac * (double)total + 0.5);
	if (want < 1) want = 1;
	LONG running = 0;
	for (int b = 0; b < PP_HIST_BUCKETS; ++b)
	{
		running += h->buckets[b];
		if (running >= want)
			return (LONGLONG)1 << b;
	}
	return (LONGLONG)1 << (PP_HIST_BUCKETS - 1);
}

void PPHistReset(PPHist* h)
{
	for (int b = 0; b < PP_HIST_BUCKETS; ++b)
		InterlockedExchange(&h->buckets[b], 0);
	InterlockedExchange(&h->maxUs, 0);
	InterlockedExchange(&h->count, 0);
	InterlockedExchange64(&h->sumUs, 0);
}


// =========================================================================
// PathQueue: / GateRate: window state (fed by the four hooks)
// =========================================================================

volatile LONG     g_passCount    = 0;   // contentStream passes this window
volatile LONGLONG g_passTotalUs  = 0;   // sum of pass durations (busy%)

// PathBusy: attribution. A second, independently-consumed running total of
// the same per-pass duration PrintPathQueueLine already sums into
// g_passTotalUs above -- kept apart so PrintPathBusyLine can reset its own
// copy without racing PrintPathQueueLine's reset of the other. cause3/other
// are QPC ticks (converted to us at print time); gate time is read from the
// same GateWindowStats snapshot PathPoolTickMain already takes once per window.
volatile LONGLONG g_busyPassTotalUs     = 0;
volatile LONGLONG g_busyCharCause3Ticks = 0;
volatile LONGLONG g_busyCharOtherTicks  = 0;

volatile LONG     g_depthMax     = 0;
volatile LONGLONG g_depthSum     = 0;
volatile LONG     g_depthSamples = 0;

volatile LONG     g_arrivedCount = 0;   // requests dequeued from the input queue
volatile LONG     g_servedCount  = 0;   // completions seen at the result queue
PPHist             g_waitHist;          // completion - drain stamp
// "svc" is the tightened serve-start-to-completion span; see
// the path-thread gate-end and dequeue tick latches for how serve-start is derived.
PPHist             g_svcHist;
volatile LONGLONG  g_svcTotalUs  = 0;   // for preamble = passTotalUs - svcTotalUs

volatile LONG g_priNpcCount    = 0;     // req+0x2C <= 10
volatile LONG g_priPlayerCount = 0;     // req+0x2C == 20
volatile LONG g_priTierCount   = 0;     // req+0x2C >= 45

// req+0x90 at completion, one counter per raw status: 0 path found, 1 no
// start face, 2 no goal face, 3 not connected, 4 fallback search failed.
// Status 3 is kept apart from "unreach": hook_csCheckFaceConn returns 1
// under the cluster-graph bypass (pathfind_hooks.cpp), so RunPathRequest's
// step 6 -- the only place that can write status 3 -- never runs false;
// st3 is always 0 there by construction, not because goals are reachable.
// Genuinely unreachable goals surface as status 4 (fallback/full A* search
// ran and failed) mixed with 1 and 2 -- each status prints separately so
// this counter can be read on its own instead of folding it into a
// euphemism.
volatile LONG g_reqStatusCount[5] = { 0, 0, 0, 0, 0 };

// Completions with status 0 that ran no path-thread search this pass
// (g_passSearchCount == 0) took RunPathRequest's step-4 direct csFindPath
// success, not the step-7 fallback/full-search path.
volatile LONG g_directCount = 0;

volatile LONG     g_gatePassCount        = 0;
volatile LONGLONG g_gatePassTotalUs      = 0;
volatile LONG     g_gatePassMaxUs        = 0;
volatile LONG     g_gatePassInTransition = 0;

// Attributed inside a gate pass (PathPoolNoteSearch), not the queue counters.
volatile LONG g_gateSearchCount = 0;
volatile LONG g_gateIterLimit   = 0;    // cause == 1
volatile LONG g_gateStateFull   = 0;    // cause == 3



// Path-thread-owned working copy (single writer: the enqueue-result hook).
static PPSlowEntry g_slowWork[PP_SLOW_N];
static int         g_slowWorkCount = 0;

// Published snapshot + seqlock (island_components.cpp's PublishSnapshot pattern).
static volatile LONG g_slowSeq = 0;
static PPSlowEntry   g_slowPublished[PP_SLOW_N];
static volatile LONG g_slowPublishedCount = 0;

// Main thread requests a reset; the path thread clears its working copy the
// next time it has something to insert (no cross-thread array write).
volatile LONG g_slowResetRequested = 0;

void PPSlowConsider(LONGLONG svcUs, LONG status, LONG priority, LONG iterations,
                            float sx, float sz, float gx, float gz)
{
	if (InterlockedCompareExchange(&g_slowResetRequested, 0, 1) == 1)
		g_slowWorkCount = 0;

	int insertAt = -1;
	if (g_slowWorkCount < PP_SLOW_N)
	{
		insertAt = g_slowWorkCount++;
	}
	else
	{
		int minIdx = 0;
		for (int i = 1; i < PP_SLOW_N; ++i)
			if (g_slowWork[i].svcUs < g_slowWork[minIdx].svcUs) minIdx = i;
		if (svcUs > g_slowWork[minIdx].svcUs)
			insertAt = minIdx;
	}
	if (insertAt < 0)
		return;

	g_slowWork[insertAt].svcUs      = svcUs;
	g_slowWork[insertAt].status     = status;
	g_slowWork[insertAt].priority   = priority;
	g_slowWork[insertAt].iterations = iterations;
	g_slowWork[insertAt].startX   = sx;
	g_slowWork[insertAt].startZ   = sz;
	g_slowWork[insertAt].goalX    = gx;
	g_slowWork[insertAt].goalZ    = gz;

	InterlockedIncrement(&g_slowSeq);        // odd: writing
	_ReadWriteBarrier();
	memcpy(g_slowPublished, g_slowWork, sizeof(g_slowWork));
	InterlockedExchange(&g_slowPublishedCount, g_slowWorkCount);
	_ReadWriteBarrier();
	InterlockedIncrement(&g_slowSeq);        // even: consistent
}

// Main thread only. Returns the published count (0 on a failed snapshot,
// treated as empty for that window rather than retried indefinitely).
int PPSlowSnapshot(PPSlowEntry* out)
{
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		LONG s1 = g_slowSeq;
		if (s1 & 1) continue;
		_ReadWriteBarrier();
		PPSlowEntry tmp[PP_SLOW_N];
		memcpy(tmp, g_slowPublished, sizeof(tmp));
		LONG cnt = g_slowPublishedCount;
		_ReadWriteBarrier();
		LONG s2 = g_slowSeq;
		if (s1 == s2)
		{
			memcpy(out, tmp, sizeof(tmp));
			return (int)cnt;
		}
	}
	return 0;
}



PPClassStats g_classStats[PP_CLASS_COUNT];

// Path-thread, non-gate A* outcome/termination accumulation. Feeds
// PathQueue's term= field. cause: 1 = iteration limit, 2 = open set full,
// 3 = search state full; 0/other = "other".
volatile LONG g_pathSearchOk    = 0;  // status == 1
volatile LONG g_pathSearchFail  = 0;  // status != 1
volatile LONG g_pathTermIterLimit   = 0;
volatile LONG g_pathTermOpenSetFull = 0;
volatile LONG g_pathTermStateFull   = 0;
volatile LONG g_pathTermOther       = 0;

// Boost counters. Outcome index: 0 = success, iterations > 32768;
// 1 = success, iterations <= 32768; 2 = failure (status != 1).
volatile LONG g_boostByTag[3][2];   // [outcome][playerByTag: 0 npc / 1 player]
volatile LONG g_boostByReq[3][3];   // [outcome][playerByReq: 0 npc / 1 player / 2 unknown(-1)]
volatile LONG g_boostDisagree = 0;  // playerByReq != -1 && playerByReq != playerByTag


} // namespace path_pool_detail

using namespace path_pool_detail;

void PathPoolNoteSearch(const PathSearchSample* s)
{
	if (!s)
		return;

	DWORD tid = GetCurrentThreadId();
	int cls;
	if (tid == g_pathThreadId)
		cls = PP_CLASS_PATH;
	else if (tid == g_navMeshBgThreadId)
		cls = PP_CLASS_NAVMESH;
	else if (IsMainThread())
		cls = PP_CLASS_MAIN;
	else
		cls = PP_CLASS_OTHER;

	bool inGate = (cls == PP_CLASS_PATH) && (t_inGatePass != 0);

	if (inGate)
	{
		InterlockedIncrement(&g_gateSearchCount);
		if (s->cause == 1) InterlockedIncrement(&g_gateIterLimit);
		if (s->cause == 3) InterlockedIncrement(&g_gateStateFull);
		GatePassNoteCause(s->cause);
	}
	else
	{
		PPClassStats* cs = &g_classStats[cls];
		InterlockedIncrement(&cs->count);
		InterlockedExchangeAdd64(&cs->totalTicks, s->ticks);
		InterlockedExchangeAdd64(&cs->totalIterations, (LONGLONG)s->iterations);

		LONGLONG us = QpcToUs(s->ticks);
		PPHistAdd(&cs->latencyUs, us);

		LONGLONG ns = TicksToNs(s->ticks);
		LONGLONG nsPerIter = (s->iterations > 0) ? (ns / s->iterations) : ns;
		PPHistAdd(&cs->iterNsHist, nsPerIter);

		if (cls == PP_CLASS_PATH)
		{
			// Latch this pass's last path-thread iteration count and mark
			// that a search ran this pass, so hook_enqueueThreadSafe can
			// attach both to the PathSlow entry / direct= classification it
			// builds for the completion this pass produces.
			g_lastPathIterations = s->iterations;
			++g_passSearchCount;

			if (s->status == 1) InterlockedIncrement(&g_pathSearchOk);
			else                InterlockedIncrement(&g_pathSearchFail);
			switch (s->cause)
			{
				case 1:  InterlockedIncrement(&g_pathTermIterLimit);   break;
				case 2:  InterlockedIncrement(&g_pathTermOpenSetFull); break;
				case 3:  InterlockedIncrement(&g_pathTermStateFull);   break;
				default: InterlockedIncrement(&g_pathTermOther);       break;
			}

			// PathBusy: attribute this path-thread search's own ticks to one
			// of the two character buckets; gate and preamble time are read
			// from elsewhere at print time (PrintPathBusyLine), not
			// accumulated here.
			bool isCharacterCaller =
				s->callerClass == ASTAR_CALLER_CHARACTER_PLAYER ||
				s->callerClass == ASTAR_CALLER_CHARACTER_NPC ||
				s->callerClass == ASTAR_CALLER_CHARACTER_UNKNOWN;
			if (isCharacterCaller)
			{
				if (s->isCause3)
					InterlockedExchangeAdd64(&g_busyCharCause3Ticks, s->ticks);
				else
					InterlockedExchangeAdd64(&g_busyCharOtherTicks, s->ticks);
			}
		}
	}

	if (s->boosted)
	{
		int outcome = (s->status == 1) ? ((s->iterations > 32768) ? 0 : 1) : 2;
		int tagIdx  = (s->playerByTag != 0) ? 1 : 0;
		InterlockedIncrement(&g_boostByTag[outcome][tagIdx]);

		int reqIdx = (s->playerByReq < 0) ? 2 : ((s->playerByReq != 0) ? 1 : 0);
		InterlockedIncrement(&g_boostByReq[outcome][reqIdx]);

		if (s->playerByReq != -1 && s->playerByReq != s->playerByTag)
			InterlockedIncrement(&g_boostDisagree);
	}
}
