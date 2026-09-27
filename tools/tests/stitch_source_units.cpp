#include <cstdio>
#include <cstring>
#include "fixes/stitch/stitch_source_policy.h"

#include "check.h"

static const unsigned __int64 kOldGraph = 0x178A2D020ull;
static const unsigned __int64 kNewGraph = 0x179025420ull;
static const unsigned __int64 kOldNodes = 0xCFEC2B20ull;
static const unsigned __int64 kNewNodes = 0xD0001000ull;

// A write as the stitch leaves it: twelve records drawn from a graph of
// oppNodes nodes, the largest of them 58.
static StitchWrite Write(unsigned __int64 oppGraph, unsigned __int64 oppNodesData, int oppNodes)
{
	StitchWrite w;
	memset(&w, 0, sizeof(w));
	w.thisGraph = 0x150000000ull;
	w.oppGraph = oppGraph;
	w.oppNodesData = oppNodesData;
	w.thisUid = 0x2818;
	w.oppUid = 0x390A19;
	w.oppNodeCount = oppNodes;
	w.connCount = 12;
	w.minIdx = 12;
	w.maxIdx = 58;
	w.site = STITCH_SITE_INTERIORS;
	return w;
}

// The measured teardown: index 58 dropped against a 55-entry node map.
static StitchDropFacts Drop(unsigned __int64 curGraph, unsigned __int64 curNodes)
{
	StitchDropFacts d;
	d.minDropIdx = 58;
	d.maxDropIdx = 58;
	d.dyingConnCount = 12;
	d.curMapSize = 55;
	d.curOppGraph = curGraph;
	d.curOppNodesData = curNodes;
	return d;
}

static StitchJoin Hit(const StitchWrite& w)
{
	StitchJoin j;
	memset(&j, 0, sizeof(j));
	j.exactHit = true;
	j.exact = w;
	j.graphSeen = 1;
	return j;
}

static StitchClass Classify(const StitchJoin& j, const StitchDropFacts& d, StitchNoWriteWhy* why)
{
	return StitchClassify(&j, &d, why);
}

int main()
{
	StitchNoWriteWhy why;

	// --- the four classes, each on the measured 58-vs-55 drop -------------

	// Written against a 60-node graph, torn down against the 55-node graph the
	// neighbour carries now: the indices came from another incarnation.
	{
		StitchJoin j = Hit(Write(kNewGraph, kNewNodes, 60));
		Check(Classify(j, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_REINCARNATED
		      && why == STITCH_WHY_NONE, "58 written from a 60-node graph, map 55 of another graph: reincarnated");
	}
	// The same graph pointer but a different node array is address reuse, not
	// the same incarnation.
	{
		StitchJoin j = Hit(Write(kOldGraph, kNewNodes, 60));
		Check(Classify(j, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_REINCARNATED,
		      "same graph address, different node array: reincarnated");
	}
	// Same graph and node array, 60 nodes when written, 55 in the map now.
	{
		StitchJoin j = Hit(Write(kOldGraph, kOldNodes, 60));
		Check(Classify(j, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_SHRANK
		      && why == STITCH_WHY_NONE, "same incarnation, 60 at write and 55 now: shrank");
	}
	// The graph grew in place after the opposite instance was built: its node
	// array moved to kNewNodes and the write saw that array, while the
	// instance's own copy still names kOldNodes. The comparison is against
	// the graph's array now, so this is shrank, not a reincarnation.
	{
		StitchJoin j = Hit(Write(kOldGraph, kNewNodes, 60));
		Check(Classify(j, Drop(kOldGraph, kNewNodes), &why) == STITCH_CLASS_SHRANK,
		      "graph grown in place, 60 at write against a 55-entry map built before: shrank");
	}
	// Written from a 55-node graph: 58 was already out of range.
	{
		StitchJoin j = Hit(Write(kOldGraph, kOldNodes, 55));
		Check(Classify(j, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_OOB_AT_WRITE,
		      "58 written against a 55-node graph: oobAtWrite");
		// oobAtWrite outranks reincarnation: the writer was wrong whatever
		// the neighbour became afterwards.
		Check(Classify(j, Drop(kNewGraph, kNewNodes), &why) == STITCH_CLASS_OOB_AT_WRITE,
		      "an index out of range at write stays oobAtWrite across a reincarnation");
		StitchJoin k = Hit(Write(kOldGraph, kOldNodes, 58));
		Check(Classify(k, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_OOB_AT_WRITE,
		      "58 against 58 nodes is one past the end: oobAtWrite");
		StitchWrite neg = Write(kOldGraph, kOldNodes, 60);
		neg.minIdx = -1;
		StitchJoin n = Hit(neg);
		StitchDropFacts d = Drop(kOldGraph, kOldNodes);
		d.minDropIdx = -1;
		Check(Classify(n, d, &why) == STITCH_CLASS_OOB_AT_WRITE,
		      "a negative index written is oobAtWrite");
	}

	// --- noWrite and its reasons --------------------------------------------
	{
		StitchJoin j;
		memset(&j, 0, sizeof(j));
		StitchDropFacts d = Drop(kOldGraph, kOldNodes);
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_NEVER,
		      "a graph the stitch never touched: noWrite/never (records it was loaded with)");
		j.graphSeen = 3;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_PAIR,
		      "graph stitched, never against this neighbour: noWrite/pair");
		j.pairHit = true;
		j.pair = Write(kOldGraph, kOldNodes, 60);
		j.pair.site = STITCH_SITE_DISK;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_OTHER_GRAPH,
		      "the pair's latest write went into another graph: noWrite/otherGraph");
		j.overflowMarked = true;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_OVERFLOW,
		      "a pair whose write may have been lost to a full table: noWrite/overflow");
		j.exactTorn = true;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_TORN,
		      "a slot held by a writer through every read: noWrite/torn");
	}
	// An entry that is not the write behind these records.
	{
		StitchJoin j = Hit(Write(kNewGraph, kNewNodes, 60));
		StitchDropFacts d = Drop(kOldGraph, kOldNodes);
		d.dyingConnCount = 13;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_MISMATCH,
		      "a different connection count: noWrite/mismatch");
		d = Drop(kOldGraph, kOldNodes);
		d.maxDropIdx = 70;
		Check(Classify(j, d, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_MISMATCH,
		      "a dropped index the write never produced: noWrite/mismatch");
		// Same incarnation, index inside a graph no larger than today's map:
		// these facts cannot have produced a drop.
		StitchJoin k = Hit(Write(kOldGraph, kOldNodes, 55));
		k.exact.maxIdx = 50;
		StitchDropFacts e = Drop(kOldGraph, kOldNodes);
		e.minDropIdx = e.maxDropIdx = 50;
		Check(Classify(k, e, &why) == STITCH_CLASS_NOWRITE && why == STITCH_WHY_MISMATCH,
		      "a same-graph drop the write explains no way: noWrite/mismatch");
	}

	// --- the table ------------------------------------------------------------
	{
		static StitchSlot store[8];
		StitchTable t;
		LfInit(&t, store, 8, 8);
		StitchWrite w = Write(kNewGraph, kNewNodes, 60);
		Check(LfTablePut(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &w) == LF_PUT_OK,
		      "a first write lands");
		StitchWrite out;
		bool writing = false;
		Check(LfTableGet(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &out, &writing) == LF_GET_HIT
		      && out.oppNodeCount == 60 && out.maxIdx == 58, "and reads back whole");

		StitchWrite w2 = w;
		w2.oppNodeCount = 61;
		LfTablePut(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &w2);
		LfTableGet(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &out, &writing);
		Check(out.oppNodeCount == 61 && t.used == 1, "latest write wins, in the same slot");

		Check(LfTableGet(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, 7), &out, &writing) == LF_GET_MISS,
		      "another opposite section is a miss");
		Check(LfCountKeyA(&t, (__int64)w.thisGraph) == 1 && LfCountKeyA(&t, 0x999) == 0,
		      "the graph-seen scan counts only that graph's slots");
	}
	// Overflow is counted, marks the pair, and turns a later miss into
	// noWrite/overflow rather than noWrite/never.
	{
		static StitchSlot store[4];
		static volatile long bits[STITCH_OVERFLOW_BITS / 32];
		StitchTable t;
		LfInit(&t, store, 4, 4);
		StitchWrite w = Write(kNewGraph, kNewNodes, 60);
		int lostUid = -1;
		for (int i = 0; i < 5; ++i)
		{
			w.oppUid = 100 + i;
			if (LfTablePut(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, w.oppUid), &w) == LF_PUT_OVERFLOW)
			{
				StitchOverflowMark(bits, w.thisUid, w.oppUid);
				lostUid = w.oppUid;
			}
		}
		Check(t.overflow == 1 && t.used == 4 && lostUid == 104, "the fifth distinct key overflows and is counted");
		Check(StitchOverflowTest(bits, w.thisUid, 104), "the lost pair is marked");
		Check(!StitchOverflowTest(bits, w.thisUid, 100), "a stored pair is not");

		StitchJoin j;
		memset(&j, 0, sizeof(j));
		StitchWrite out;
		bool writing = false;
		j.exactHit = LfTableGet(&t, (__int64)w.thisGraph, StitchPairKey(w.thisUid, 104), &out, &writing) == LF_GET_HIT;
		j.overflowMarked = StitchOverflowTest(bits, w.thisUid, 104);
		j.graphSeen = LfCountKeyA(&t, (__int64)w.thisGraph);
		Check(!j.exactHit && Classify(j, Drop(kOldGraph, kOldNodes), &why) == STITCH_CLASS_NOWRITE
		      && why == STITCH_WHY_OVERFLOW, "the overflowed pair's drop reads noWrite/overflow");
	}
	// A slot mid-write: a second writer gives up and is counted, and a reader
	// that never sees it settle reports torn.
	{
		static StitchSlot store[4];
		StitchTable t;
		LfInit(&t, store, 4, 4);
		StitchWrite w = Write(kNewGraph, kNewNodes, 60);
		__int64 kb = StitchPairKey(w.thisUid, w.oppUid);
		LfTablePut(&t, (__int64)w.thisGraph, kb, &w);
		StitchSlot* s = 0;
		for (int i = 0; i < 4; ++i)
			if (store[i].keyA == (__int64)w.thisGraph)
				s = &store[i];
		long even = 0;
		Check(s && LfBeginWrite(&s->seq, &even), "a writer takes the slot");
		Check(LfTablePut(&t, (__int64)w.thisGraph, kb, &w) == LF_PUT_CONTENDED && t.contended == 1,
		      "a concurrent writer on the same key is counted, not silent");
		StitchWrite out;
		bool writing = false;
		Check(LfTableGet(&t, (__int64)w.thisGraph, kb, &out, &writing) == LF_GET_TORN && writing,
		      "a reader that never sees the slot settle reports torn");
		LfEndWrite(&s->seq, even);
		Check(LfTableGet(&t, (__int64)w.thisGraph, kb, &out, &writing) == LF_GET_HIT,
		      "and reads it once the writer is done");
	}

	// --- double generation ------------------------------------------------------
	{
		StitchUidGen g;
		memset(&g, 0, sizeof(g));
		const __int64 sec = 1000;
		Check(StitchGenAdvance(&g, kOldGraph, 1 * sec), "the first graph of a section is new");
		Check(!StitchGenAdvance(&g, kOldGraph, 2 * sec), "stitching it again is not");
		Check(!StitchNearDoubleGen(&g, 3 * sec, 10 * sec), "one graph is not a double generation");
		Check(StitchGenAdvance(&g, kNewGraph, 4 * sec) && g.prevGraph == kOldGraph, "a second graph shifts the first");
		Check(StitchNearDoubleGen(&g, 5 * sec, 10 * sec), "two graphs 3 s apart, drop 1 s later: near");
		Check(!StitchNearDoubleGen(&g, 30 * sec, 10 * sec), "a drop 26 s later is not");
		StitchUidGen h = g;
		StitchGenAdvance(&h, 0x1234, 60 * sec);
		Check(!StitchNearDoubleGen(&h, 61 * sec, 10 * sec), "two graphs 56 s apart are not near");
	}

	// The pair table's first key word is a constant, so a pair whose folded
	// value is zero still claims a slot.
	{
		static StitchSlot store[4];
		StitchTable t;
		LfInit(&t, store, 4, 4);
		StitchWrite w = Write(kNewGraph, kNewNodes, 60);
		StitchWrite out;
		bool writing = false;
		Check(StitchPairKey(0, 0) == 0
		      && LfTablePut(&t, STITCH_PAIR_KEY_A, StitchPairKey(0, 0), &w) == LF_PUT_OK
		      && LfTableGet(&t, STITCH_PAIR_KEY_A, StitchPairKey(0, 0), &out, &writing) == LF_GET_HIT,
		      "a zero pair key is stored under the pair table's constant first word");
		Check(StitchPairKey(1, 2) != StitchPairKey(2, 1), "the pair key is ordered");
	}

	return CheckExit("stitch_source_units");
}
