#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/stitch/stitch_source.h"
#include "fixes/stitch/stitch_source_policy.h"
#include "fixes/stitch/unstitch_guard.h"
#include "fixes/stitch/unstitch_layout.h"
#include "fixes/stitch/unstitch_probe_policy.h"
#include "base/fixed_log_buf.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include <intrin.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

// ---------------------------------------------------------------------------
// Tables. Static storage: nothing on either side allocates.
// ---------------------------------------------------------------------------

static const int kWriteSlots = 8192;   // exact (graph, thisUid, oppUid)
static const int kPairSlots  = 4096;   // latest write per uid pair, any graph
static const int kUidSlots   = 2048;   // per section
static const int kProbe      = 64;

static StitchSlot               s_writeStore[kWriteSlots];
static StitchSlot               s_pairStore[kPairSlots];
static LfSlot<StitchUidAdd>     s_addStore[kUidSlots];
static LfSlot<StitchUidGen>     s_genStore[kUidSlots];
static StitchTable              s_write;
static StitchTable              s_pair;
static LfTable<StitchUidAdd>    s_add;
static LfTable<StitchUidGen>    s_gen;
static volatile long            s_overflowBits[STITCH_OVERFLOW_BITS / 32];

static bool s_installed   = false;
static bool s_addObserved = false;

// ---------------------------------------------------------------------------
// Write-side counters
// ---------------------------------------------------------------------------

static volatile LONG s_calls       = 0;
static volatile LONG s_inFlight    = 0;   // stitches inside the original right now
static volatile LONG s_inFlightMax = 0;
static volatile LONG s_seq         = 0;
static volatile LONG s_sets        = 0;   // graph sets recorded
static volatile LONG s_absent      = 0;   // the stitch removed the set: no connections
static volatile LONG s_conns       = 0;
static volatile LONG s_wrOob       = 0;   // records written already outside the opposite graph
static volatile LONG s_wrOobSets   = 0;
static volatile LONG s_unreadable  = 0;
static volatile LONG s_oppNoGi     = 0;
static volatile LONG s_noWrite     = 0;   // calls that wrote nothing: a side without a mesh or uid   // written against an opposite side with no graph instance
static volatile LONG s_site[STITCH_SITE_COUNT] = { 0 };
static volatile LONG s_adds        = 0;
static volatile LONG s_addSeq      = 0;

// ---------------------------------------------------------------------------
// Read-side counters. The read side runs inside the teardown's changeMutex,
// which serialises it; the counters are interlocked anyway so the main-thread
// heartbeat reads them whole.
// ---------------------------------------------------------------------------

static volatile LONG s_dropSets    = 0;
static volatile LONG s_repeat      = 0;
static volatile LONG s_class[STITCH_CLASS_COUNT] = { 0 };
static volatile LONG s_why[STITCH_WHY_COUNT] = { 0 };
static volatile LONG s_raceHint    = 0;   // a stitch was in flight, or the slot mid-write, at the drop
static volatile LONG s_dblGen      = 0;   // opposite section had two graphs stitched close together
static volatile LONG s_lines       = 0;

static const void* s_lastFiredGi   = NULL;
static long        s_lastFiredCall = -2;
static bool        s_repeatCall    = false;

static LONGLONG s_qpf = 0;
static double   s_nextBeat = 0.0;
static const double kBeatSeconds = 60.0;
static const int    kDoubleGenWindowSec = 10;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static __int64 Now()
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

static int MsBetween(__int64 from, __int64 to)
{
	if (s_qpf <= 0 || from == 0)
		return -1;
	__int64 ms = (to - from) * 1000 / s_qpf;
	return ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
}

static __int64 UidKey(int uid) { return (__int64)(unsigned int)uid + 1; }

// ---------------------------------------------------------------------------
// Guarded reads: standalone and POD-only.
// ---------------------------------------------------------------------------

static bool ReadI32(const void* at, int* out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const int*)at; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static bool ReadPtr(const void* at, const unsigned char** out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const unsigned char* const*)at; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

// ---------------------------------------------------------------------------
// Write side
// ---------------------------------------------------------------------------

struct SideFacts
{
	int connCount;   // -1: no set for the pair
	int minIdx;
	int maxIdx;
	int oob;
};

// One graph's set for one opposite section, as the stitch left it.
static bool ReadSet(const unsigned char* graph, int thisUid, int oppUid, int oppNodeCount,
                    SideFacts* out)
{
	out->connCount = -1; out->minIdx = 0; out->maxIdx = -1; out->oob = 0;

	const unsigned char* sets = NULL;
	int setCount = 0;
	if (!ReadPtr(graph + OFF_GRAPH_SETS_DATA, &sets) || !ReadI32(graph + OFF_GRAPH_SETS_SIZE, &setCount))
		return false;
	if (!UnstitchSetCountPlausible(setCount) || (setCount > 0 && !sets))
		return false;

	for (int i = 0; i < setCount; ++i)
	{
		const unsigned char* e = sets + SET_STRIDE * (size_t)i;
		int a = 0, b = 0;
		if (!ReadI32(e + OFF_SET_THIS_UID, &a) || !ReadI32(e + OFF_SET_OPP_UID, &b))
			return false;
		if (a != thisUid || b != oppUid)
			continue;

		const unsigned char* conns = NULL;
		int n = 0;
		if (!ReadPtr(e + OFF_SET_CONN_DATA, &conns) || !ReadI32(e + OFF_SET_CONN_SIZE, &n))
			return false;
		if (!UnstitchConnCountPlausible(n) || (n > 0 && !conns))
			return false;
		out->connCount = n;
		for (int j = 0; j < n; ++j)
		{
			int idx = 0;
			if (!ReadI32(conns + CONN_STRIDE * (size_t)j + OFF_CONN_OPP_NODE, &idx))
				return false;
			if (j == 0 || idx < out->minIdx) out->minIdx = idx;
			if (j == 0 || idx > out->maxIdx) out->maxIdx = idx;
			if (idx < 0 || idx >= oppNodeCount)
				++out->oob;
		}
		return true;
	}
	return true;
}

struct NavFacts
{
	const unsigned char* mesh;
	const unsigned char* graph;
	const unsigned char* gi;
	int uid;
};

static bool ReadNav(const unsigned char* nav, NavFacts* out)
{
	out->mesh = NULL; out->graph = NULL; out->gi = NULL; out->uid = 0;
	if (!nav)
		return false;
	return ReadPtr(nav + OFF_NAVINST_MESH, &out->mesh)
	    && ReadPtr(nav + OFF_NAVINST_GRAPH, &out->graph)
	    && ReadPtr(nav + OFF_NAVINST_GRAPH_INST, &out->gi)
	    && ReadI32(nav + OFF_NAVINST_UID, &out->uid);
}

// The stitch writes nothing when either side has no mesh, and the pair
// helper nothing when either uid is unset; a set read back after such a call
// is an older write, and recording it would re-date it.
static bool StitchWrote(const NavFacts* a, const NavFacts* b)
{
	return a->mesh && b->mesh && a->uid != -1 && b->uid != -1;
}

static void NoteGeneration(int uid, const unsigned char* graph, __int64 qpc)
{
	StitchUidGen g;
	memset(&g, 0, sizeof(g));
	bool writing = false;
	LfTableGet(&s_gen, UidKey(uid), 0, &g, &writing);
	if (StitchGenAdvance(&g, (unsigned __int64)graph, qpc))
		LfTablePut(&s_gen, UidKey(uid), 0, &g);
}

static void RecordSide(const NavFacts* self, const NavFacts* opp, int site, int seq, __int64 qpc)
{
	if (!self->graph || !opp->graph)
		return;

	StitchWrite w;
	memset(&w, 0, sizeof(w));
	w.thisGraph = (unsigned __int64)self->graph;
	w.oppGraph  = (unsigned __int64)opp->graph;
	w.qpc       = qpc;
	w.thisUid   = self->uid;
	w.oppUid    = opp->uid;
	w.site      = site;
	w.writeSeq  = seq;
	w.thisGiLive = self->gi ? 1 : 0;
	w.oppGiLive  = opp->gi ? 1 : 0;
	w.oppGiMap   = -1;
	w.oppAddSeq  = s_addObserved ? -1 : -2;
	w.oppAddSame = -1;

	const unsigned char* nodes = NULL;
	if (!ReadI32(opp->graph + OFF_GRAPH_NODES_SIZE, &w.oppNodeCount)
		|| !ReadPtr(opp->graph + OFF_GRAPH_NODES_DATA, &nodes)
		|| (opp->gi && !ReadI32(opp->gi + OFF_GI_NODEMAP_SIZE, &w.oppGiMap)))
	{
		InterlockedIncrement(&s_unreadable);
		return;
	}
	w.oppNodesData = (unsigned __int64)nodes;

	SideFacts f;
	if (!ReadSet(self->graph, self->uid, opp->uid, w.oppNodeCount, &f))
	{
		InterlockedIncrement(&s_unreadable);
		return;
	}
	w.connCount = f.connCount;
	w.minIdx = f.minIdx;
	w.maxIdx = f.maxIdx;
	w.oobCount = f.oob;

	if (s_addObserved)
	{
		StitchUidAdd a;
		bool writing = false;
		if (LfTableGet(&s_add, UidKey(opp->uid), 0, &a, &writing) == LF_GET_HIT)
		{
			w.oppAddSeq = a.addSeq;
			w.oppAddSame = (a.graph == w.oppGraph) ? 1 : 0;
		}
	}

	InterlockedIncrement(&s_sets);
	if (f.connCount < 0)
		InterlockedIncrement(&s_absent);
	else
		InterlockedExchangeAdd(&s_conns, f.connCount);
	if (f.oob > 0)
	{
		InterlockedExchangeAdd(&s_wrOob, f.oob);
		InterlockedIncrement(&s_wrOobSets);
	}
	if (!opp->gi)
		InterlockedIncrement(&s_oppNoGi);

	if (LfTablePut(&s_write, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &w)
		== LF_PUT_OVERFLOW)
		StitchOverflowMark(s_overflowBits, w.thisUid, w.oppUid);
	LfTablePut(&s_pair, STITCH_PAIR_KEY_A, StitchPairKey(w.thisUid, w.oppUid), &w);
}

static int SiteOf(void* ret)
{
	size_t rva = (size_t)((unsigned char*)ret - (unsigned char*)GameAddr(0));
	if (rva == RVA_STITCH_RET_UNLOADED)  return STITCH_SITE_UNLOADED;
	if (rva == RVA_STITCH_RET_DISK)      return STITCH_SITE_DISK;
	if (rva == RVA_STITCH_RET_INTERIORS) return STITCH_SITE_INTERIORS;
	if (rva == RVA_STITCH_RET_SPLICE)    return STITCH_SITE_SPLICE;
	return STITCH_SITE_UNKNOWN;
}

typedef int (*nmgStitch_t)(void* nmg, void* a, void* b);
static nmgStitch_t orig_nmgStitch = NULL;

// Which section pairs are inside the original right now, so a drop can ask
// whether its own sections are being stitched at that moment rather than
// whether any stitch is. 0 is a free slot; a claim that finds none is counted.
static const int kInFlightSlots = 16;
static volatile LONG64 s_inFlightPairs[kInFlightSlots];
static volatile LONG   s_inFlightFull = 0;

static int ClaimInFlight(int aUid, int bUid)
{
	LONG64 v = (LONG64)(((unsigned __int64)(unsigned int)aUid << 32) | (unsigned int)bUid);
	if (v == 0)
		v = 1;   // a pair of two zero uids is not a stitch; keep the slot claimable
	for (int i = 0; i < kInFlightSlots; ++i)
		if (InterlockedCompareExchange64(&s_inFlightPairs[i], v, 0) == 0)
			return i;
	InterlockedIncrement(&s_inFlightFull);
	return -1;
}

static bool PairInFlight(int uidX, int uidY)
{
	for (int i = 0; i < kInFlightSlots; ++i)
	{
		unsigned __int64 v = (unsigned __int64)InterlockedCompareExchange64(&s_inFlightPairs[i], 0, 0);
		if (v == 0)
			continue;
		int a = (int)(unsigned int)(v >> 32), b = (int)(unsigned int)v;
		if (a == uidX || b == uidX || a == uidY || b == uidY)
			return true;
	}
	return false;
}

// The in-flight marks must come down even if the original unwinds; POD-only
// so the termination handler can sit here.
static int CallOrigCounted(void* nmg, void* a, void* b, int slot)
{
	int r = 0;
	__try { r = orig_nmgStitch(nmg, a, b); }
	__finally
	{
		InterlockedDecrement(&s_inFlight);
		if (slot >= 0)
			InterlockedExchange64(&s_inFlightPairs[slot], 0);
	}
	return r;
}

static int ReadUidOr(const void* nav, int fallback)
{
	int uid = fallback;
	if (nav && !ReadI32((const unsigned char*)nav + OFF_NAVINST_UID, &uid))
		uid = fallback;
	return uid;
}

static int hook_nmgStitch(void* nmg, void* a, void* b)
{
	const int site = SiteOf(_ReturnAddress());
	InterlockedIncrement(&s_calls);
	InterlockedIncrement(&s_site[site]);

	LONG now = InterlockedIncrement(&s_inFlight);
	for (;;)
	{
		LONG max = Read(&s_inFlightMax);
		if (now <= max || InterlockedCompareExchange(&s_inFlightMax, now, max) == max)
			break;
	}
	// Before the original copies either side's mesh and graph: does this
	// stitch's job overlap another live navmesh job (nm_adjacency.h)?
	NmAdjCheckStitch(a, b, site == STITCH_SITE_SPLICE);
	const int slot = ClaimInFlight(ReadUidOr(a, -1), ReadUidOr(b, -1));
	int r = CallOrigCounted(nmg, a, b, slot);

	// After the original, still inside the caller's build lock: the records
	// read here are the ones it just wrote.
	const int seq = (int)InterlockedIncrement(&s_seq);
	const __int64 qpc = Now();
	NavFacts fa, fb;
	if (!ReadNav((const unsigned char*)a, &fa) || !ReadNav((const unsigned char*)b, &fb))
	{
		InterlockedIncrement(&s_unreadable);
		return r;
	}
	if (!StitchWrote(&fa, &fb))
	{
		InterlockedIncrement(&s_noWrite);
		return r;
	}
	NoteGeneration(fa.uid, fa.graph, qpc);
	NoteGeneration(fb.uid, fb.graph, qpc);
	RecordSide(&fa, &fb, site, seq, qpc);
	RecordSide(&fb, &fa, site, seq, qpc);
	return r;
}

// ---------------------------------------------------------------------------
// Insert observer
// ---------------------------------------------------------------------------

void StitchSourceNoteAddObserver()
{
	s_addObserved = true;
}

void StitchSourceOnAdd(void* collection, __int64 sectionData, __int64 graphInstance)
{
	if (!s_installed || !collection || !sectionData || !graphInstance)
		return;
	// The insert takes the graph instance only into an empty slot; read the
	// slot back to see whether this one did.
	int slot = -1;
	const unsigned char* slots = NULL;
	const unsigned char* held = NULL;
	if (!ReadI32((const unsigned char*)sectionData + 420, &slot) || slot < 0
		|| !ReadPtr((const unsigned char*)collection + OFF_COLL_SLOTS, &slots) || !slots
		|| !ReadPtr(slots + COLL_SLOT_STRIDE * (size_t)slot + OFF_COLL_SLOT_GRAPHINST, &held))
		return;
	if (held != (const unsigned char*)graphInstance)
		return;

	StitchUidAdd a;
	memset(&a, 0, sizeof(a));
	const unsigned char* graph = NULL;
	int uid = 0;
	if (!ReadI32(held + OFF_GI_UID, &uid) || !ReadPtr(held + OFF_GI_GRAPH, &graph))
		return;
	a.graph = (unsigned __int64)graph;
	a.qpc = Now();
	a.addSeq = (int)InterlockedIncrement(&s_addSeq);
	InterlockedIncrement(&s_adds);
	LfTablePut(&s_add, UidKey(uid), 0, &a);
}

// ---------------------------------------------------------------------------
// Read side
// ---------------------------------------------------------------------------

void StitchSourceBeginFired(const void* graphInstance, long callNo)
{
	// Both entries of one teardown are consecutive un-stitch calls on one
	// instance; anything else is a new teardown, even at a reused address.
	s_repeatCall = (graphInstance == s_lastFiredGi && callNo == s_lastFiredCall + 1);
	s_lastFiredGi = graphInstance;
	s_lastFiredCall = callNo;
}

static void Cell(FixedLogBuf* o, int uid)
{
	// Sector uids are x | y << 8; interiors use a wider form.
	if (uid >= 0 && uid < 0x10000)
	{
		FlbDec(o, uid & 0xFF); FlbChar(o, ','); FlbDec(o, uid >> 8);
	}
	else
		FlbStr(o, "int");
}

static void UHex(FixedLogBuf* o, int v) { FlbHex(o, (unsigned __int64)(unsigned int)v); }

void StitchSourceOnSetDrops(const void* graphInstance, int ownUid, int oppUid, int setConnCount,
                            const void* oppGraphInstance, int mapSize,
                            int minDropIdx, int maxDropIdx, int drops)
{
	if (!s_installed)
		return;
	if (s_repeatCall)
	{
		InterlockedIncrement(&s_repeat);
		return;
	}
	InterlockedIncrement(&s_dropSets);
	const __int64 qpc = Now();
	const LONG inFlight = Read(&s_inFlight);
	const bool pairInFlight = PairInFlight(ownUid, oppUid);

	const unsigned char* gi = (const unsigned char*)graphInstance;
	const unsigned char* opp = (const unsigned char*)oppGraphInstance;
	const unsigned char* dyingGraph = NULL;
	const unsigned char* oppGraph = NULL;
	const unsigned char* graphNodes = NULL;   // the graph's node array now
	const unsigned char* instNodes = NULL;    // the instance's copy, taken at its creation
	int graphNodeCount = -1, instNodeCount = -1;
	ReadPtr(gi + OFF_GI_GRAPH, &dyingGraph);
	ReadPtr(opp + OFF_GI_GRAPH, &oppGraph);
	ReadPtr(opp + OFF_GI_ORIG_NODES, &instNodes);
	ReadI32(opp + OFF_GI_NUM_ORIG_NODES, &instNodeCount);
	if (oppGraph)
	{
		ReadPtr(oppGraph + OFF_GRAPH_NODES_DATA, &graphNodes);
		ReadI32(oppGraph + OFF_GRAPH_NODES_SIZE, &graphNodeCount);
	}

	StitchDropFacts d;
	d.minDropIdx = minDropIdx;
	d.maxDropIdx = maxDropIdx;
	d.dyingConnCount = setConnCount;
	d.curMapSize = mapSize;
	d.curOppGraph = (unsigned __int64)oppGraph;
	d.curOppNodesData = (unsigned __int64)graphNodes;

	StitchJoin j;
	memset(&j, 0, sizeof(j));
	bool writing = false;
	LfGet g = LfTableGet(&s_write, (__int64)dyingGraph, StitchPairKey(ownUid, oppUid), &j.exact, &writing);
	j.exactHit = (g == LF_GET_HIT);
	j.exactTorn = (g == LF_GET_TORN);
	bool pairWriting = false;
	j.pairHit = LfTableGet(&s_pair, STITCH_PAIR_KEY_A, StitchPairKey(ownUid, oppUid), &j.pair, &pairWriting) == LF_GET_HIT
	            && j.pair.thisGraph != (unsigned __int64)dyingGraph;
	j.overflowMarked = StitchOverflowTest(s_overflowBits, ownUid, oppUid);
	j.graphSeen = j.exactHit ? 1 : LfCountKeyA(&s_write, (__int64)dyingGraph);

	StitchNoWriteWhy why = STITCH_WHY_NONE;
	StitchClass c = StitchClassify(&j, &d, &why);
	InterlockedIncrement(&s_class[c]);
	InterlockedIncrement(&s_why[why]);
	if (pairInFlight || writing)
		InterlockedIncrement(&s_raceHint);

	StitchUidGen gen;
	memset(&gen, 0, sizeof(gen));
	bool genWriting = false;
	LfTableGet(&s_gen, UidKey(oppUid), 0, &gen, &genWriting);
	const bool dbl = StitchNearDoubleGen(&gen, qpc, s_qpf * kDoubleGenWindowSec);
	if (dbl)
		InterlockedIncrement(&s_dblGen);

	StitchUidAdd addNow;
	memset(&addNow, 0, sizeof(addNow));
	bool addWriting = false;
	const bool addHit = s_addObserved
		&& LfTableGet(&s_add, UidKey(oppUid), 0, &addNow, &addWriting) == LF_GET_HIT;

	if (InterlockedIncrement(&s_lines) > fixes::g_fixesCfg.cfg_stitchSourceLines)
	{
		InterlockedDecrement(&s_lines);
		return;
	}

	// The write facts shown are the exact entry's, or the pair's latest when
	// the exact one is missing, so an otherGraph drop still names its site.
	const StitchWrite* w = j.exactHit ? &j.exact : (j.pairHit ? &j.pair : NULL);

	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "StitchSource DROP class="); FlbStr(&o, StitchClassName(c));
	FlbStr(&o, " why=");    FlbStr(&o, StitchWhyName(why));
	FlbStr(&o, " sec=");    UHex(&o, ownUid);
	FlbStr(&o, " opp=");    UHex(&o, oppUid);
	FlbStr(&o, " cell=");   Cell(&o, ownUid); FlbChar(&o, '/'); Cell(&o, oppUid);
	FlbStr(&o, " idx=");    FlbDec(&o, minDropIdx); FlbStr(&o, ".."); FlbDec(&o, maxDropIdx);
	FlbStr(&o, " drops=");  FlbDec(&o, drops);
	FlbStr(&o, " conns=");  FlbDec(&o, setConnCount);
	FlbStr(&o, " map=");    FlbDec(&o, mapSize);
	if (w)
	{
		FlbStr(&o, " wrNodes=");  FlbDec(&o, w->oppNodeCount);
		FlbStr(&o, " wrIdx=");    FlbDec(&o, w->minIdx); FlbStr(&o, ".."); FlbDec(&o, w->maxIdx);
		FlbStr(&o, " wrConns=");  FlbDec(&o, w->connCount);
		FlbStr(&o, " wrOob=");    FlbDec(&o, w->oobCount);
		FlbStr(&o, " site=");     FlbStr(&o, StitchSiteName(w->site));
		FlbStr(&o, " wrAgeMs=");  FlbDec(&o, MsBetween(w->qpc, qpc));
	}
	FlbStr(&o, " inFlight="); FlbDec(&o, inFlight);
	FlbStr(&o, " pairInFlight="); FlbDec(&o, pairInFlight ? 1 : 0);
	FlbStr(&o, " writing=");  FlbDec(&o, writing ? 1 : 0);
	LogMsgDeferrable(FlbDone(&o));

	FixedLogBuf p; FlbInit(&p);
	FlbStr(&p, "StitchSource DROP+ sec="); UHex(&p, ownUid);
	FlbStr(&p, " nowOpp=");  FlbHex(&p, (unsigned __int64)oppGraph);
	FlbStr(&p, "/");         FlbHex(&p, (unsigned __int64)graphNodes);
	FlbStr(&p, "#");         FlbDec(&p, graphNodeCount);
	FlbStr(&p, " inst=");    FlbHex(&p, (unsigned __int64)instNodes);
	FlbStr(&p, "#");         FlbDec(&p, instNodeCount);
	if (w)
	{
		FlbStr(&p, " wrOpp=");   FlbHex(&p, w->oppGraph);
		FlbStr(&p, "/");         FlbHex(&p, w->oppNodesData);
		FlbStr(&p, " wrGi=");    FlbDec(&p, w->thisGiLive); FlbChar(&p, '/'); FlbDec(&p, w->oppGiLive);
		FlbStr(&p, " wrGiMap="); FlbDec(&p, w->oppGiMap);
		FlbStr(&p, " wrGraph="); FlbHex(&p, w->thisGraph);
	}
	FlbStr(&p, " dying=");     FlbHex(&p, (unsigned __int64)dyingGraph);
	FlbStr(&p, " seen=");      FlbDec(&p, j.graphSeen);
	FlbStr(&p, " addSeq=");
	if (!s_addObserved)
		FlbStr(&p, "?/?");
	else
	{
		if (w) FlbDec(&p, w->oppAddSeq); else FlbChar(&p, '-');
		FlbChar(&p, '/');
		if (addHit) FlbDec(&p, addNow.addSeq); else FlbChar(&p, '-');
		if (w && w->oppAddSame >= 0) { FlbStr(&p, " addSame="); FlbDec(&p, w->oppAddSame); }
	}
	FlbStr(&p, " dblGen=");    FlbDec(&p, dbl ? 1 : 0);
	FlbStr(&p, " genGapMs=");  FlbDec(&p, gen.prevGraph ? MsBetween(gen.prevQpc, gen.qpc) : -1);
	FlbStr(&p, " genAgeMs=");  FlbDec(&p, gen.graph ? MsBetween(gen.qpc, qpc) : -1);
	LogMsgDeferrable(FlbDone(&p));
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

static void EmitHeartbeat()
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "StitchSource writes: calls="); FlbDec(&o, Read(&s_calls));
	FlbStr(&o, " inFlightMax="); FlbDec(&o, Read(&s_inFlightMax));
	FlbStr(&o, " slotsFull=");   FlbDec(&o, Read(&s_inFlightFull));
	FlbStr(&o, " sets=");        FlbDec(&o, Read(&s_sets));
	FlbStr(&o, " absent=");      FlbDec(&o, Read(&s_absent));
	FlbStr(&o, " conns=");       FlbDec(&o, Read(&s_conns));
	FlbStr(&o, " wrOob=");       FlbDec(&o, Read(&s_wrOob));
	FlbStr(&o, "/");             FlbDec(&o, Read(&s_wrOobSets));
	FlbStr(&o, " unreadable=");  FlbDec(&o, Read(&s_unreadable));
	FlbStr(&o, " site=u");       FlbDec(&o, Read(&s_site[STITCH_SITE_UNLOADED]));
	FlbStr(&o, "/d");            FlbDec(&o, Read(&s_site[STITCH_SITE_DISK]));
	FlbStr(&o, "/i");            FlbDec(&o, Read(&s_site[STITCH_SITE_INTERIORS]));
	FlbStr(&o, "/s");            FlbDec(&o, Read(&s_site[STITCH_SITE_SPLICE]));
	FlbStr(&o, "/?");            FlbDec(&o, Read(&s_site[STITCH_SITE_UNKNOWN]));
	FlbStr(&o, " oppNoGi=");     FlbDec(&o, Read(&s_oppNoGi));
	FlbStr(&o, " noop=");        FlbDec(&o, Read(&s_noWrite));
	FlbStr(&o, " used=");        FlbDec(&o, Read(&s_write.used));
	FlbStr(&o, "/");             FlbDec(&o, kWriteSlots);
	FlbStr(&o, " overflow=");    FlbDec(&o, Read(&s_write.overflow));
	FlbStr(&o, "/");             FlbDec(&o, Read(&s_pair.overflow));
	FlbStr(&o, "/");             FlbDec(&o, Read(&s_gen.overflow));
	FlbStr(&o, "/");             FlbDec(&o, Read(&s_add.overflow));
	FlbStr(&o, " contended=");   FlbDec(&o, Read(&s_write.contended) + Read(&s_pair.contended)
	                                        + Read(&s_gen.contended) + Read(&s_add.contended));
	FlbStr(&o, " adds=");
	if (s_addObserved) FlbDec(&o, Read(&s_adds)); else FlbChar(&o, '?');
	LogMsgDeferrable(FlbDone(&o));

	FixedLogBuf p; FlbInit(&p);
	FlbStr(&p, "StitchSource drops: ");
	if (!UnstitchGuardInstalled())
	{
		// No guard, no drops to classify: "?" rather than a zero that would
		// read as "none happened".
		FlbStr(&p, "sets=? (unstitchGuard not installed)");
		LogMsgDeferrable(FlbDone(&p));
		return;
	}
	FlbStr(&p, "sets=");          FlbDec(&p, Read(&s_dropSets));
	FlbStr(&p, " noWrite=");      FlbDec(&p, Read(&s_class[STITCH_CLASS_NOWRITE]));
	FlbStr(&p, "(never=");        FlbDec(&p, Read(&s_why[STITCH_WHY_NEVER]));
	FlbStr(&p, " pair=");         FlbDec(&p, Read(&s_why[STITCH_WHY_PAIR]));
	FlbStr(&p, " otherGraph=");   FlbDec(&p, Read(&s_why[STITCH_WHY_OTHER_GRAPH]));
	FlbStr(&p, " overflow=");     FlbDec(&p, Read(&s_why[STITCH_WHY_OVERFLOW]));
	FlbStr(&p, " mismatch=");     FlbDec(&p, Read(&s_why[STITCH_WHY_MISMATCH]));
	FlbStr(&p, " torn=");         FlbDec(&p, Read(&s_why[STITCH_WHY_TORN]));
	FlbStr(&p, ") oobAtWrite=");  FlbDec(&p, Read(&s_class[STITCH_CLASS_OOB_AT_WRITE]));
	FlbStr(&p, " reincarnated="); FlbDec(&p, Read(&s_class[STITCH_CLASS_REINCARNATED]));
	FlbStr(&p, " shrank=");       FlbDec(&p, Read(&s_class[STITCH_CLASS_SHRANK]));
	FlbStr(&p, " repeat=");       FlbDec(&p, Read(&s_repeat));
	FlbStr(&p, " raceHint=");     FlbDec(&p, Read(&s_raceHint));
	FlbStr(&p, " dblGen=");       FlbDec(&p, Read(&s_dblGen));
	FlbStr(&p, " lines=");        FlbDec(&p, Read(&s_lines));
	FlbStr(&p, "/");              FlbDec(&p, fixes::g_fixesCfg.cfg_stitchSourceLines);
	LogMsgDeferrable(FlbDone(&p));
}

static const char* s_notInstalledWhy = "not attempted";

void StitchSourceTick(double now)
{
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;
	if (!s_installed)
	{
		// Absent, said on the same cadence: "?" rather than silence or a zero.
		FixedLogBuf o; FlbInit(&o);
		FlbStr(&o, "StitchSource writes: calls=? (not installed: ");
		FlbStr(&o, s_notInstalledWhy);
		FlbStr(&o, ") drops: sets=?");
		LogMsgDeferrable(FlbDone(&o));
		return;
	}
	EmitHeartbeat();
}

void InstallStitchSource(int* installed, int*)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;

	LfInit(&s_write, s_writeStore, kWriteSlots, kProbe);
	LfInit(&s_pair,  s_pairStore,  kPairSlots,  kProbe);
	LfInit(&s_add,   s_addStore,   kUidSlots,   kProbe);
	LfInit(&s_gen,   s_genStore,   kUidSlots,   kProbe);

	const char* why = HookInstallRow(HOOK_NMG_STITCH, hook_nmgStitch, (void**)&orig_nmgStitch,
			installed, true);

	if (!why)
	{
		s_installed = true;
		NmAdjNoteCheckerPresent();
		LogMsg("Stitch source: installed (writes recorded at the stitch, drops classified by the un-stitch guard)");
		// A baseline at zero, so a session with no stitch at all reads as
		// armed and quiet rather than as one that never looked.
		EmitHeartbeat();
		s_nextBeat = kBeatSeconds;
	}
	else
	{
		orig_nmgStitch = NULL;
		s_notInstalledWhy = why;
		ErrorLog(std::string("Stitch source: not installed (") + why
		         + "); un-stitch drops go unclassified");
	}
}
