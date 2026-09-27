#include "fixes/stitch/stitch_source_policy.h"
#include <intrin.h>
#include <string.h>

#pragma intrinsic(_InterlockedCompareExchange128, _InterlockedCompareExchange, _InterlockedExchange, _InterlockedOr, _InterlockedIncrement)

const char* StitchSiteName(int site)
{
	switch (site)
	{
	case STITCH_SITE_UNLOADED:  return "unloaded";
	case STITCH_SITE_DISK:      return "disk";
	case STITCH_SITE_INTERIORS: return "interiors";
	case STITCH_SITE_SPLICE:    return "splice";
	default:                    return "unknown";
	}
}

const char* StitchClassName(int c)
{
	switch (c)
	{
	case STITCH_CLASS_NOWRITE:      return "noWrite";
	case STITCH_CLASS_OOB_AT_WRITE: return "oobAtWrite";
	case STITCH_CLASS_REINCARNATED: return "reincarnated";
	case STITCH_CLASS_SHRANK:       return "shrank";
	default:                        return "?";
	}
}

const char* StitchWhyName(int w)
{
	switch (w)
	{
	case STITCH_WHY_NONE:        return "-";
	case STITCH_WHY_NEVER:       return "never";
	case STITCH_WHY_PAIR:        return "pair";
	case STITCH_WHY_OTHER_GRAPH: return "otherGraph";
	case STITCH_WHY_OVERFLOW:    return "overflow";
	case STITCH_WHY_MISMATCH:    return "mismatch";
	case STITCH_WHY_TORN:        return "torn";
	default:                     return "?";
	}
}

static unsigned __int64 Mix(unsigned __int64 x)
{
	x ^= x >> 33;
	x *= 0xFF51AFD7ED558CCDull;
	x ^= x >> 33;
	x *= 0xC4CEB9FE1A85EC53ull;
	x ^= x >> 33;
	return x;
}

__int64 StitchPairKey(int thisUid, int oppUid)
{
	return (__int64)(((unsigned __int64)(unsigned int)thisUid << 32)
	                 | (unsigned __int64)(unsigned int)oppUid);
}

unsigned int LfHash(__int64 keyA, __int64 keyB)
{
	return (unsigned int)Mix((unsigned __int64)keyA ^ Mix((unsigned __int64)keyB));
}

bool LfClaimOrMatch(volatile __int64* keys, __int64 keyA, __int64 keyB, bool* claimed)
{
	__int64 cmp[2] = { 0, 0 };
	*claimed = false;
	if (_InterlockedCompareExchange128(keys, keyB, keyA, cmp))
	{
		*claimed = true;
		return true;
	}
	return cmp[0] == keyA && cmp[1] == keyB;
}

// Exchanging 0 for 0 never changes a slot, so this is a 128-bit atomic read.
void LfReadKeys(volatile __int64* keys, __int64* a, __int64* b)
{
	__int64 cmp[2] = { 0, 0 };
	_InterlockedCompareExchange128(keys, 0, 0, cmp);
	*a = cmp[0];
	*b = cmp[1];
}

bool LfBeginWrite(volatile long* seq, long* even)
{
	long cur = _InterlockedCompareExchange(seq, 0, 0);
	if ((cur & 1) != 0)
		return false;
	if (_InterlockedCompareExchange(seq, cur + 1, cur) != cur)
		return false;
	*even = cur;
	return true;
}

void LfEndWrite(volatile long* seq, long even)
{
	_InterlockedExchange(seq, even + 2);
}

long LfReadSeq(volatile long* seq)
{
	return _InterlockedCompareExchange(seq, 0, 0);
}

void LfBarrier()
{
	_ReadWriteBarrier();
}

bool StitchGenAdvance(StitchUidGen* g, unsigned __int64 graph, __int64 qpc)
{
	if (g->graph == graph)
		return false;
	g->prevGraph = g->graph;
	g->prevQpc = g->qpc;
	g->graph = graph;
	g->qpc = qpc;
	return true;
}

bool StitchNearDoubleGen(const StitchUidGen* g, __int64 dropQpc, __int64 windowTicks)
{
	if (!g->prevGraph || !g->graph)
		return false;
	return (g->qpc - g->prevQpc) <= windowTicks && (dropQpc - g->qpc) <= windowTicks;
}

static unsigned int OverflowBit(int thisUid, int oppUid)
{
	return (unsigned int)Mix((unsigned __int64)StitchPairKey(thisUid, oppUid))
	       & (unsigned int)(STITCH_OVERFLOW_BITS - 1);
}

void StitchOverflowMark(volatile long* bits, int thisUid, int oppUid)
{
	unsigned int bit = OverflowBit(thisUid, oppUid);
	_InterlockedOr(&bits[bit >> 5], (long)(1u << (bit & 31)));
}

bool StitchOverflowTest(const volatile long* bits, int thisUid, int oppUid)
{
	unsigned int bit = OverflowBit(thisUid, oppUid);
	return (bits[bit >> 5] & (long)(1u << (bit & 31))) != 0;
}

// ---------------------------------------------------------------------------

StitchClass StitchClassify(const StitchJoin* j, const StitchDropFacts* d, StitchNoWriteWhy* why)
{
	*why = STITCH_WHY_NONE;

	if (!j->exactHit)
	{
		if (j->exactTorn)
			*why = STITCH_WHY_TORN;
		else if (j->overflowMarked)
			*why = STITCH_WHY_OVERFLOW;
		else if (j->pairHit)
			*why = STITCH_WHY_OTHER_GRAPH;
		else if (j->graphSeen > 0)
			*why = STITCH_WHY_PAIR;
		else
			*why = STITCH_WHY_NEVER;
		return STITCH_CLASS_NOWRITE;
	}

	const StitchWrite& w = j->exact;
	// The entry must describe the records the teardown is holding: same
	// count, and every dropped index inside the range the write produced.
	if (w.connCount != d->dyingConnCount
		|| d->minDropIdx < w.minIdx || d->maxDropIdx > w.maxIdx)
	{
		*why = STITCH_WHY_MISMATCH;
		return STITCH_CLASS_NOWRITE;
	}

	if (d->minDropIdx < 0 || d->maxDropIdx >= w.oppNodeCount)
		return STITCH_CLASS_OOB_AT_WRITE;

	if (w.oppGraph != d->curOppGraph || w.oppNodesData != d->curOppNodesData)
		return STITCH_CLASS_REINCARNATED;

	if (w.oppNodeCount > d->curMapSize)
		return STITCH_CLASS_SHRANK;

	// Same graph, every index inside it, and a map at least as large: the
	// indices cannot have missed that map, so the entry is not their write.
	*why = STITCH_WHY_MISMATCH;
	return STITCH_CLASS_NOWRITE;
}
