// splice_ring.cpp - The lock-free record ring between the wall progress detour and the drain.
#include "navmesh/construction/splice_ring.h"

namespace navmesh {

void SpliceRingInit(SpliceRing* r)
{
	for (int i = 0; i < SPLICE_RING_SLOTS; ++i)
		r->slot[i].seq = SPLICE_SEQ_EMPTY;
	r->written = r->claimFailed = r->lost = r->overwritten = 0;
	r->read = 0;
	r->stallAt = -1;
	r->stallPasses = 0;
}

void SpliceRingPush(SpliceRing* r, const float box[6])
{
	const LONG idx = InterlockedIncrement(&r->written) - 1;
	SpliceRingSlot* s = &r->slot[idx & (SPLICE_RING_SLOTS - 1)];
	const LONG cur = InterlockedCompareExchange(&s->seq, 0, 0);
	// Held by another producer, or already holding a newer lap's record: drop this one.
	if (cur == SPLICE_SEQ_WRITING || cur > idx
	 || InterlockedCompareExchange(&s->seq, SPLICE_SEQ_WRITING, cur) != cur)
	{
		InterlockedIncrement(&r->claimFailed);
		return;
	}
	for (int i = 0; i < 6; ++i)
		s->box[i] = box[i];
	InterlockedExchange(&s->seq, idx);
}

int SpliceRingDrain(SpliceRing* r, float (*out)[6], int max)
{
	int n = 0;
	const LONG w = InterlockedCompareExchange(&r->written, 0, 0);
	for (; r->read < w && n < max; )
	{
		if (w - r->read > SPLICE_RING_SLOTS)
		{
			// The producers lapped the cursor: the oldest indices' slots are reused.
			InterlockedExchangeAdd(&r->lost, (w - SPLICE_RING_SLOTS) - r->read);
			r->read = w - SPLICE_RING_SLOTS;
			r->stallPasses = 0;
			continue;
		}
		SpliceRingSlot* s = &r->slot[r->read & (SPLICE_RING_SLOTS - 1)];
		const LONG seq = InterlockedCompareExchange(&s->seq, 0, 0);
		if (seq == r->read)
		{
			float box[6];
			for (int i = 0; i < 6; ++i)
				box[i] = s->box[i];
			MemoryBarrier();
			if (InterlockedCompareExchange(&s->seq, 0, 0) == r->read)
			{
				for (int i = 0; i < 6; ++i)
					out[n][i] = box[i];
				++n;
			}
			else
				InterlockedIncrement(&r->lost);       // a later lap claimed the slot mid-copy
			++r->read;
			r->stallPasses = 0;
			continue;
		}
		if (seq > r->read)
		{
			InterlockedIncrement(&r->lost);           // a later lap published over it
			InterlockedIncrement(&r->overwritten);
			++r->read;
			r->stallPasses = 0;
			continue;
		}
		// Empty, being written, or an older lap's record: not published yet, or its claim failed.
		if (r->stallAt == r->read && ++r->stallPasses >= SPLICE_RING_STALL_PASSES)
		{
			InterlockedIncrement(&r->lost);           // abandoned
			++r->read;
			r->stallPasses = 0;
			continue;
		}
		if (r->stallAt != r->read)
		{
			r->stallAt = r->read;
			r->stallPasses = 1;
		}
		break;
	}
	return n;
}

} // namespace navmesh
