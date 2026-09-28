#ifndef KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_POLICY_H

// Write-side record of the cross-section stitch, and the classifier that joins
// it to a record the un-stitch bounds guard drops. No game headers, no
// KenshiLib; the tables use Win32 interlocked intrinsics only, so the whole
// file is host-testable.
//
// The tables are written from the stitch (generator and worker threads, under
// the game's build mutex or the mod's collision-build lock) and read from the
// un-stitch (section-manager thread, under changeMutex). The two sides share no
// lock, so a slot is published through a per-slot sequence word: a writer
// takes it odd, fills the payload and makes it even; a reader copies the
// payload between two equal even reads, with three attempts. Failed or torn
// copies are reported as missing attribution, never accepted as a set; these
// tables classify diagnostics, not the un-stitch bounds decision. Initialized
// before installation, never reset: claimed slots are never freed. Nothing
// here allocates or blocks.

#include <stddef.h>
#include <string.h>
#include <intrin.h>

// Which call reached the stitch, from its return address.
enum StitchSite
{
	STITCH_SITE_UNKNOWN = 0,
	STITCH_SITE_UNLOADED,   // stitchUnloadedZone: a completed task or the live sector
	STITCH_SITE_DISK,       // stitchUnloadedZone: a sector loaded, stitched, saved and freed
	STITCH_SITE_INTERIORS,  // stitchWithInteriors
	STITCH_SITE_SPLICE,     // splice (partial generation)
	STITCH_SITE_COUNT
};

const char* StitchSiteName(int site);

// What one stitch left in one graph's set for one opposite section.
struct StitchWrite
{
	unsigned __int64 thisGraph;      // the graph whose set was written
	unsigned __int64 oppGraph;       // the graph the indices were drawn from
	unsigned __int64 oppNodesData;   // oppGraph's node array at write time
	__int64          qpc;
	int thisUid;
	int oppUid;
	int oppNodeCount;   // oppGraph's node count: the node-map size an instance of it gets
	int connCount;      // -1: the stitch found no connections and removed the set
	int minIdx;
	int maxIdx;
	int oobCount;       // records whose index was already outside oppNodeCount
	int site;
	int thisGiLive;     // the stitching side's NavInstance carried a graph instance
	int oppGiLive;      // the opposite side's did
	int oppGiMap;       // that instance's node-map size, -1 when absent
	int writeSeq;       // global stitch sequence number
	int oppAddSeq;      // the collection's last insert of the opposite section, -1 none, -2 not observed
	int oppAddSame;     // 1: that insert carried oppGraph, 0: another graph, -1: no insert known
	int pad;
};

// Per section: the last graph the streaming collection took for it.
struct StitchUidAdd
{
	unsigned __int64 graph;
	__int64          qpc;
	int              addSeq;   // global insert sequence at that insert
	int              pad;
};

// Per section: the last two distinct graphs the stitch saw for it. Two graphs
// of one section stitched close together is the double-generation shape.
struct StitchUidGen
{
	unsigned __int64 graph;
	unsigned __int64 prevGraph;
	__int64          qpc;       // when graph was first stitched
	__int64          prevQpc;   // when prevGraph was first stitched
};

// Records a stitch of graph for its section; returns true when the graph is
// new for it, i.e. the payload changed and needs publishing.
bool StitchGenAdvance(StitchUidGen* g, unsigned __int64 graph, __int64 qpc);

// The drop falls within windowTicks of a second distinct graph of the opposite
// section, which itself arrived within windowTicks of the one before it.
bool StitchNearDoubleGen(const StitchUidGen* g, __int64 dropQpc, __int64 windowTicks);

// ---------------------------------------------------------------------------
// Lock-free table keyed by two 64-bit words, claimed with a 128-bit CAS.
// Slots are never freed; latest write wins. Templated on the payload so the
// write table and the per-section tables share one implementation.
// ---------------------------------------------------------------------------

template <typename P>
struct __declspec(align(16)) LfSlot
{
	volatile __int64 keyA;   // 0 = free
	volatile __int64 keyB;
	volatile long    seq;    // 0 = claimed, unpublished; odd = a writer is filling it
	long             pad;
	P                w;
};

template <typename P>
struct LfTable
{
	LfSlot<P>*     slots;
	int            capacity;   // power of two
	int            probeLimit;
	volatile long  overflow;   // writes that found no slot within probeLimit
	volatile long  contended;  // writes that found their slot mid-write and gave up
	volatile long  used;
};

enum LfPut { LF_PUT_OK = 0, LF_PUT_OVERFLOW, LF_PUT_CONTENDED };
enum LfGet { LF_GET_HIT = 0, LF_GET_MISS, LF_GET_TORN };

unsigned int LfHash(__int64 keyA, __int64 keyB);
// One 128-bit CAS: claims a free slot for the key or reports what it holds.
bool LfClaimOrMatch(volatile __int64* keys, __int64 keyA, __int64 keyB, bool* claimed);
void LfReadKeys(volatile __int64* keys, __int64* a, __int64* b);
bool LfBeginWrite(volatile long* seq, long* even);
void LfEndWrite(volatile long* seq, long even);
long LfReadSeq(volatile long* seq);
void LfBarrier();

template <typename P>
void LfInit(LfTable<P>* t, LfSlot<P>* storage, int capacity, int probeLimit)
{
	t->slots = storage;
	t->capacity = capacity;
	t->probeLimit = probeLimit;
	t->overflow = 0;
	t->contended = 0;
	t->used = 0;
	memset(storage, 0, sizeof(LfSlot<P>) * (size_t)capacity);
}

// keyA must be non-zero.
template <typename P>
LfPut LfTablePut(LfTable<P>* t, __int64 keyA, __int64 keyB, const P* w)
{
	const unsigned int mask = (unsigned int)t->capacity - 1;
	const unsigned int h = LfHash(keyA, keyB);
	for (int i = 0; i < t->probeLimit; ++i)
	{
		LfSlot<P>* s = &t->slots[(h + (unsigned int)i) & mask];
		bool claimed = false;
		if (!LfClaimOrMatch(&s->keyA, keyA, keyB, &claimed))
			continue;
		if (claimed)
			_InterlockedIncrement(&t->used);
		long even = 0;
		if (!LfBeginWrite(&s->seq, &even))
		{
			// Two writers on one key at once: the other write stands, and this
			// one is counted rather than lost silently.
			_InterlockedIncrement(&t->contended);
			return LF_PUT_CONTENDED;
		}
		memcpy(&s->w, w, sizeof(P));
		LfEndWrite(&s->seq, even);
		return LF_PUT_OK;
	}
	_InterlockedIncrement(&t->overflow);
	return LF_PUT_OVERFLOW;
}

// TORN: the slot exists but a writer held it through every read attempt.
template <typename P>
LfGet LfTableGet(LfTable<P>* t, __int64 keyA, __int64 keyB, P* out, bool* writing)
{
	const unsigned int mask = (unsigned int)t->capacity - 1;
	const unsigned int h = LfHash(keyA, keyB);
	*writing = false;
	for (int i = 0; i < t->probeLimit; ++i)
	{
		LfSlot<P>* s = &t->slots[(h + (unsigned int)i) & mask];
		__int64 a = 0, b = 0;
		LfReadKeys(&s->keyA, &a, &b);
		if (a == 0 && b == 0)
			return LF_GET_MISS;   // never freed, so the probe chain ends here
		if (a != keyA || b != keyB)
			continue;
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			long s1 = LfReadSeq(&s->seq);
			if (s1 == 0 || (s1 & 1) != 0)
			{
				*writing = true;
				continue;
			}
			memcpy(out, &s->w, sizeof(P));
			LfBarrier();
			if (LfReadSeq(&s->seq) == s1)
				return LF_GET_HIT;
			*writing = true;
		}
		return LF_GET_TORN;
	}
	return LF_GET_MISS;
}

// Slots whose keyA is this value: for the write table, "was this graph ever
// stitched this session", asked only on the rare read-side miss.
template <typename P>
int LfCountKeyA(LfTable<P>* t, __int64 keyA)
{
	int n = 0;
	for (int i = 0; i < t->capacity; ++i)
	{
		__int64 a = 0, b = 0;
		LfReadKeys(&t->slots[i].keyA, &a, &b);
		if (a == keyA)
			++n;
	}
	return n;
}

typedef LfTable<StitchWrite> StitchTable;
typedef LfSlot<StitchWrite>  StitchSlot;

// A uid pair folded to one word: the second key word of the write table and
// the pair table (whose first word is STITCH_PAIR_KEY_A), and the overflow
// bitmap's input. It can be zero, so it is never a table's first word.
__int64 StitchPairKey(int thisUid, int oppUid);
const __int64 STITCH_PAIR_KEY_A = 1;

// Overflow memory: a pair whose exact write was lost to a full table sets its
// bit, so a later miss on it reads as overflow rather than as no write. A set
// bit means "possibly lost"; a clear one means "certainly not lost".
const int STITCH_OVERFLOW_BITS = 8192;
void StitchOverflowMark(volatile long* bits, int thisUid, int oppUid);
bool StitchOverflowTest(const volatile long* bits, int thisUid, int oppUid);

// ---------------------------------------------------------------------------
// Classification of a dropped record
// ---------------------------------------------------------------------------

enum StitchClass
{
	STITCH_CLASS_NOWRITE = 0,
	STITCH_CLASS_OOB_AT_WRITE,
	STITCH_CLASS_REINCARNATED,
	STITCH_CLASS_SHRANK,
	STITCH_CLASS_COUNT
};

enum StitchNoWriteWhy
{
	STITCH_WHY_NONE = 0,       // not a noWrite
	STITCH_WHY_NEVER,          // the dying graph was never stitched this session: records it loaded with
	STITCH_WHY_PAIR,           // the graph was stitched, never against this opposite section
	STITCH_WHY_OTHER_GRAPH,    // the pair's latest write went into another graph of this section
	STITCH_WHY_OVERFLOW,       // the exact write may have been lost to a full table
	STITCH_WHY_MISMATCH,       // an entry exists, but the records now are not the ones it saw
	STITCH_WHY_TORN,           // a writer held the slot through every read
	STITCH_WHY_COUNT
};

const char* StitchClassName(int c);
const char* StitchWhyName(int w);

// What the teardown sees for one set that lost records.
struct StitchDropFacts
{
	int minDropIdx;
	int maxDropIdx;
	int dyingConnCount;          // the set's connection count now
	int curMapSize;              // opposite instance's node-map size now
	unsigned __int64 curOppGraph;     // opposite instance's original graph
	unsigned __int64 curOppNodesData; // that graph's node array now -- not the instance's
	                                  // copy, which is fixed at creation and would turn a
	                                  // graph that grew in place into a reincarnation
};

// What the tables answered for it.
struct StitchJoin
{
	bool exactHit;
	bool exactTorn;
	bool pairHit;            // the uid pair's latest write, whichever graph it went into
	bool overflowMarked;
	int  graphSeen;          // exact-table slots naming the dying graph
	StitchWrite exact;
	StitchWrite pair;
};

// Precedence: noWrite (with its reason), then oobAtWrite, then reincarnated,
// then shrank. "shrank" needs the same graph, and the same node array as that
// graph holds now, at both ends, and a write-time node count above the map
// the opposite instance was built with; a same-graph drop that is neither
// is inconsistent with the entry, which makes the entry not the write that
// produced the record -- a mismatch.
StitchClass StitchClassify(const StitchJoin* j, const StitchDropFacts* d, StitchNoWriteWhy* why);

#endif // KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_POLICY_H
