// throwout_hold.cpp - The throw-out hold table: a lock-free ring with a sequence word per entry.
#include "fixes/world/throwout_hold.h"
#include "fixes/world/throwout_policy.h"
#include <windows.h>
#include <intrin.h>

namespace throwout_hold_detail {
// seq is odd while a writer owns the entry; the other fields are valid only under an even value
// read before and after them.
struct HoldSlot { volatile LONG seq; LONG live; game::HandKey key; double expiry; fixes::ThrowoutHand hand; };
} // namespace throwout_hold_detail
using namespace throwout_hold_detail;

static HoldSlot s_slots[fixes::THROWOUT_HOLD_SLOTS];
static volatile LONG s_next;

// Claims entry i for writing: an even sequence becomes odd. False when another writer holds it.
static bool BeginWrite(HoldSlot& s, LONG* seqOut)
{
	LONG v = InterlockedCompareExchange(&s.seq, 0, 0);
	if (v & 1)
		return false;
	if (InterlockedCompareExchange(&s.seq, v + 1, v) != v)
		return false;
	*seqOut = v + 1;
	return true;
}
static void EndWrite(HoldSlot& s, LONG seq) { InterlockedExchange(&s.seq, seq + 1); }

// Copies entry s when it was read whole: two equal even reads of its sequence around the copy, at
// most two attempts. The sequence reads are volatile loads (acquire on x64) and x64 keeps loads in
// order, so the barriers need only stop the compiler moving the copy.
static bool ReadEntry(const HoldSlot& s, LONG* live, game::HandKey* key, double* expiry, fixes::ThrowoutHand* hand)
{
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const LONG before = s.seq;
		if (before & 1)
			continue;
		_ReadWriteBarrier();
		const LONG l = s.live;
		const game::HandKey k = s.key;
		const double e = s.expiry;
		const fixes::ThrowoutHand h = s.hand;
		_ReadWriteBarrier();
		const LONG after = s.seq;
		if (before != after)
			continue;
		*live = l;
		*key = k;
		*expiry = e;
		if (hand)
			*hand = h;
		return true;
	}
	return false;
}

namespace fixes {

bool ThrowoutHoldAdd(const game::HandKey& key, const ThrowoutHand& hand, double expiryHours)
{
	HoldSlot& s = s_slots[(InterlockedIncrement(&s_next) - 1) & (THROWOUT_HOLD_SLOTS - 1)];
	LONG seq;
	if (!BeginWrite(s, &seq))
		return false;
	s.key = key;
	s.hand = hand;
	s.expiry = expiryHours;
	s.live = 1;
	EndWrite(s, seq);
	return true;
}

bool ThrowoutHoldIsHeld(const game::HandKey& key, double nowHours)
{
	for (int i = 0; i < THROWOUT_HOLD_SLOTS; ++i)
	{
		LONG live;
		game::HandKey k;
		double e;
		if (!ReadEntry(s_slots[i], &live, &k, &e, NULL) || !live)
			continue;
		if (game::HandKeyEqual(k, key) && ThrowoutHoldDecide(nowHours, e, true) == TH_HELD)
			return true;
	}
	return false;
}

bool ThrowoutHoldRead(int i, game::HandKey* key, ThrowoutHand* hand, double* expiryHours)
{
	if (i < 0 || i >= THROWOUT_HOLD_SLOTS)
		return false;
	LONG live;
	return ReadEntry(s_slots[i], &live, key, expiryHours, hand) && live;
}

bool ThrowoutHoldRelease(int i, const game::HandKey& key, double expiryHours)
{
	if (i < 0 || i >= THROWOUT_HOLD_SLOTS)
		return false;
	HoldSlot& s = s_slots[i];
	LONG seq;
	if (!BeginWrite(s, &seq))
		return false;
	bool ended = false;
	if (s.live && game::HandKeyEqual(s.key, key) && s.expiry == expiryHours)
	{
		s.live = 0;
		ended = true;
	}
	EndWrite(s, seq);
	return ended;
}

long ThrowoutHoldClear()
{
	long missed = 0;
	for (int i = 0; i < THROWOUT_HOLD_SLOTS; ++i)
	{
		HoldSlot& s = s_slots[i];
		LONG seq;
		bool claimed = false;
		for (int t = 0; t < THROWOUT_HOLD_CLEAR_TRIES && !claimed; ++t)
		{
			claimed = BeginWrite(s, &seq);
			if (!claimed)
				YieldProcessor();
		}
		if (!claimed)
		{
			++missed;
			continue;
		}
		s.live = 0;
		EndWrite(s, seq);
	}
	return missed;
}

long ThrowoutHoldLive()
{
	long n = 0;
	for (int i = 0; i < THROWOUT_HOLD_SLOTS; ++i)
	{
		LONG live;
		game::HandKey k;
		double e;
		if (ReadEntry(s_slots[i], &live, &k, &e, NULL) && live)
			++n;
	}
	return n;
}

} // namespace fixes
