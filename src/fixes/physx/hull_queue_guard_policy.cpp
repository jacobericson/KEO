#include "fixes/physx/hull_queue_guard_policy.h"

namespace hull_queue_guard_policy_detail
{

const unsigned F_FREED   = 1;
const unsigned F_PENDING = 2;
const unsigned F_MADE    = 4;
const unsigned F_SEEN    = 8;

// Probes from p's hash; a slot from an older epoch counts as empty.
HqgSlot* Slot(HqgState* s, uintptr_t p)
{
	unsigned long long h = ((unsigned long long)p >> 3) * 0x9E3779B97F4A7C15ull;
	unsigned i = (unsigned)(h >> 32) & s->mask;
	for (;;)
	{
		HqgSlot* e = &s->slots[i];
		if (e->epoch != s->epoch)
		{
			e->epoch = s->epoch;
			e->key = p;
			e->flags = 0;
			return e;
		}
		if (e->key == p)
			return e;
		i = (i + 1) & s->mask;
	}
}

void Mark(HqgState* s, const uintptr_t* data, unsigned n, unsigned flag)
{
	for (unsigned i = 0; i < n; ++i)
		if (data[i])
			Slot(s, data[i])->flags |= flag;
}

HqgResult Blank()
{
	HqgResult r;
	r.outcome = HQG_OUT_EMPTY;
	r.scanned = r.dups = r.inList = r.pending = r.freed = 0;
	r.keptRemade = r.nulled = r.batchCount = r.pendingCount = r.findings = 0;
	return r;
}

} // namespace
using namespace hull_queue_guard_policy_detail;

void HqgInit(HqgState* s, HqgSlot* slots, unsigned slotCount)
{
	s->slots = slots;
	s->mask = slotCount - 1;
	for (unsigned i = 0; i < slotCount; ++i)
		slots[i].epoch = 0;
	s->epoch = 0;
	s->haveRec = false;
	s->recData = 0;
	s->recCount = 0;
}

void HqgRecord(HqgState* s, const uintptr_t* backData, unsigned backCount)
{
	s->haveRec = true;
	s->recData = backData;
	s->recCount = backCount;
}

void HqgForget(HqgState* s)
{
	s->haveRec = false;
	s->recData = 0;
	s->recCount = 0;
}

HqgResult HqgJudge(HqgState* s, const HqgLists* in, bool act, HqgNullFn nullFn, void* ctx,
                   HqgFinding* out, unsigned outCap)
{
	HqgResult r = Blank();
	const unsigned nd = in->destroyMainCount;
	if (nd == 0)
		return r;
	const unsigned b0 = in->destroyBackCount;
	const unsigned nm = in->makeMainCount;
	if (nd > HQG_MAX_LIST || !in->destroyMain ||
	    b0 > HQG_MAX_LIST || (b0 && !in->destroyBack) ||
	    nm > HQG_MAX_LIST || (nm && !in->makeMain))
	{
		r.outcome = HQG_OUT_SKIP_LIST;
		return r;
	}

	// The physics thread only zeroes the back count; only the flush appends
	// to it or moves its buffer. So a zero count means the recorded batch was
	// deleted, and an unchanged count and buffer mean it is still queued.
	const uintptr_t* batch = 0;
	unsigned batchN = 0;
	const uintptr_t* pend = 0;
	unsigned pendN = 0;
	if (!s->haveRec)
	{
		pend = in->destroyBack;
		pendN = b0;
	}
	else if (b0 == 0)
	{
		if (s->recCount)
		{
			if (in->destroyBack != s->recData)
			{
				r.outcome = HQG_OUT_SKIP_STATE;
				return r;
			}
			batch = s->recData;
			batchN = s->recCount;
		}
	}
	else if (b0 == s->recCount && in->destroyBack == s->recData)
	{
		pend = in->destroyBack;
		pendN = b0;
	}
	else
	{
		r.outcome = HQG_OUT_SKIP_STATE;
		return r;
	}

	// Half the table at most, so every probe ends quickly.
	unsigned long long need = (unsigned long long)batchN + pendN + nm + nd;
	if (need > (unsigned long long)(s->mask + 1) / 2)
	{
		r.outcome = HQG_OUT_SKIP_OVERFLOW;
		return r;
	}

	if (++s->epoch == 0)
	{
		for (unsigned i = 0; i <= s->mask; ++i)
			s->slots[i].epoch = 0;
		s->epoch = 1;
	}
	Mark(s, batch, batchN, F_FREED);
	Mark(s, pend, pendN, F_PENDING);
	if (batchN)
		Mark(s, in->makeMain, nm, F_MADE);
	r.batchCount = batchN;
	r.pendingCount = pendN;

	for (unsigned i = 0; i < nd; ++i)
	{
		uintptr_t p = in->destroyMain[i];
		if (!p)
			continue;
		++r.scanned;
		HqgSlot* e = Slot(s, p);
		HqgDup kind = HQG_DUP_NONE;
		if (e->flags & F_SEEN)
			kind = HQG_DUP_IN_LIST;
		else if (e->flags & F_PENDING)
			kind = HQG_DUP_PENDING;
		else if (e->flags & F_FREED)
		{
			if (e->flags & F_MADE)
				++r.keptRemade;
			else
				kind = HQG_DUP_FREED;
		}
		e->flags |= F_SEEN;
		if (kind == HQG_DUP_NONE)
			continue;

		++r.dups;
		if (kind == HQG_DUP_IN_LIST)       ++r.inList;
		else if (kind == HQG_DUP_PENDING)  ++r.pending;
		else                               ++r.freed;
		if (r.findings < outCap)
		{
			out[r.findings].index = i;
			out[r.findings].ptr = p;
			out[r.findings].kind = kind;
			++r.findings;
		}
		if (act && HqgDupDrops(kind))
		{
			if (!nullFn(ctx, i, p))
			{
				r.outcome = HQG_OUT_RACED;
				return r;
			}
			++r.nulled;
		}
	}
	r.outcome = HQG_OUT_JUDGED;
	return r;
}

const char* HqgDupName(HqgDup d)
{
	switch (d)
	{
	case HQG_DUP_IN_LIST: return "inList";
	case HQG_DUP_PENDING: return "pending";
	case HQG_DUP_FREED:   return "freed";
	default:              return "none";
	}
}
