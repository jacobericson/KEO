// The throw-out fix's decisions (the gate-code ask, the flag write, the timeout, the drop
// classification, the hold, the candidate and the finder mode) and the hold table, through the
// real throwout_hold.cpp on one thread.
#include <cstdio>
#include <cstring>
#include "fixes/world/throwout_policy.h"
#include "fixes/world/throwout_hold.h"
#include "game/hand_key.h"

#include "check.h"

using fixes::ThrowoutHand;
using game::HandKey;

static HandKey Key(unsigned index, unsigned serial)
{
	HandKey k = { 2, 7, 31, index, serial };
	return k;
}

static ThrowoutHand Hand(unsigned char fill)
{
	ThrowoutHand h;
	std::memset(h.bytes, fill, sizeof h.bytes);
	return h;
}

static void CheckAsk()
{
	CHECK(ThrowoutMayAskGateCode(false, true, true, false), "ask: outdoors with gates and a live navmesh asks");
	CHECK(!ThrowoutMayAskGateCode(true, true, true, false), "ask: indoors never asks");
	CHECK(!ThrowoutMayAskGateCode(false, false, true, false), "ask: no gates singleton never asks");
	CHECK(!ThrowoutMayAskGateCode(false, true, false, false), "ask: an absent navmesh is never asked");
	CHECK(!ThrowoutMayAskGateCode(false, true, true, true), "ask: a stopped navmesh is never asked");
}

static void CheckFlag()
{
	CHECK(ThrowoutFlagDecide(true, 0) == TF_INDOORS && ThrowoutFlagDecide(true, 1) == TF_INDOORS
	      && ThrowoutFlagDecide(true, -1) == TF_INDOORS, "flag: indoors writes nothing");
	CHECK(ThrowoutFlagDecide(false, -1) == TF_UNRESOLVED, "flag: an unresolved code writes nothing");
	CHECK(ThrowoutFlagDecide(false, 0) == TF_WRITE, "flag: code 0 writes outside");
	CHECK(ThrowoutFlagDecide(false, 1) == TF_WRITE, "flag: code 1 writes inside");
}

static void CheckTimeout()
{
	CHECK(!ThrowoutTimedOut(0.0, 100.0), "timeout: no stamp never times out");
	CHECK(!ThrowoutTimedOut(2.0, 2.0 + 4.0 / 60.0), "timeout: four minutes is not five");
	CHECK(ThrowoutTimedOut(2.0, 2.0 + 5.0 / 60.0), "timeout: five minutes times out");
}

static void CheckDrop()
{
	CHECK(ThrowoutClassifyDrop(TR_NONE, true, TF_WRITE, 0) == TD_NONE, "drop: no reason is not a throw-out");
	CHECK(ThrowoutClassifyDrop(TR_PATH_IMPOSSIBLE, false, TF_WRITE, 0) == TD_PATH_IMPOSSIBLE, "drop: path impossible");
	CHECK(ThrowoutClassifyDrop(TR_TICK, true, TF_WRITE, 0) == TD_TIMEOUT, "drop: the tick's timeout");
	CHECK(ThrowoutClassifyDrop(TR_TICK, false, TF_WRITE, 0) == TD_REACHED_OUTSIDE, "drop: reached and written outside");
	CHECK(ThrowoutClassifyDrop(TR_TICK, false, TF_UNRESOLVED, -1) == TD_REACHED_INSIDE
	      && ThrowoutClassifyDrop(TR_TICK, false, TF_WRITE, 1) == TD_REACHED_INSIDE,
	      "drop: reached and unresolved counts inside");
	CHECK(ThrowoutTownDrop(TR_TICK, true) && ThrowoutTownDrop(TR_PATH_IMPOSSIBLE, true) && !ThrowoutTownDrop(TR_NONE, true),
	      "drop: the town task's slot drops are throw-outs");
	CHECK(!ThrowoutTownDrop(TR_TICK, false) && !ThrowoutTownDrop(TR_PATH_IMPOSSIBLE, false),
	      "drop: another task sharing the slots is not a throw-out");
}

static void CheckHold()
{
	CHECK(ThrowoutHoldDecide(10.5, 11.0, true, 1.0) == TH_HELD, "hold: before the expiry, unconscious, is held");
	CHECK(ThrowoutHoldDecide(11.0, 11.0, true, 1.0) == TH_CAP && ThrowoutHoldDecide(11.0, 11.0, false, 1.0) == TH_CAP,
	      "hold: the expiry itself ends it");
	CHECK(ThrowoutHoldDecide(10.5, 11.0, false, 1.0) == TH_WOKE, "hold: awake before the expiry is woke");
	CHECK(ThrowoutHoldDecide(10.5, 0.0, true, 1.0) == TH_NONE && ThrowoutHoldDecide(10.5, -1.0, false, 1.0) == TH_NONE,
	      "hold: an empty entry is none");

	const double e = ThrowoutHoldExpiry(10.0, 60);
	bool held = true;
	for (int i = 0; i < 10000; ++i)
		held = held && ThrowoutHoldDecide(10.5, e, true, 1.0) == TH_HELD;
	CHECK(held, "hold: a stopped clock never expires");

	const double fast = ThrowoutHoldExpiry(0.0, 60);
	CHECK(ThrowoutHoldDecide(0.999, fast, true, 1.0) == TH_HELD && ThrowoutHoldDecide(1.0, fast, true, 1.0) == TH_CAP,
	      "hold: a 5x clock expires five times sooner");
	CHECK(ThrowoutHoldExpiry(2.0, 90) == 3.5 && ThrowoutHoldExpiry(0.0, 1440) == 24.0,
	      "hold: the expiry is minutes over sixty");

	// A load into an earlier game time leaves an expiry further ahead than any hold could set.
	CHECK(ThrowoutHoldDecide(5.0, 11.0, true, 1.0) == TH_STALE && ThrowoutHoldDecide(5.0, 11.0, false, 1.0) == TH_STALE
	      && ThrowoutHoldDecide(10.0, 11.0 + 1.5 / 60.0, true, 1.0) == TH_STALE,
	      "hold: an expiry further ahead than the cap is stale");
	CHECK(ThrowoutHoldDecide(10.0, 11.0, true, 1.0) == TH_HELD && ThrowoutHoldDecide(10.0, 11.0 + 0.5 / 60.0, true, 1.0) == TH_HELD
	      && ThrowoutHoldDecide(0.0, 24.0, true, 24.0) == TH_HELD,
	      "hold: an expiry within the cap is held");
}

static void CheckCandidate()
{
	CHECK(ThrowoutCandidate(true, false, true, true, 0, 1, false), "candidate: the vanilla body is found");
	CHECK(!ThrowoutCandidate(false, false, true, true, 0, 1, false), "candidate: no character");
	CHECK(!ThrowoutCandidate(true, true, true, true, 0, 1, false), "candidate: carried");
	CHECK(!ThrowoutCandidate(true, false, false, true, 0, 1, false), "candidate: another town");
	CHECK(!ThrowoutCandidate(true, false, true, false, 0, 1, false), "candidate: conscious");
	CHECK(!ThrowoutCandidate(true, false, true, true, kThrowoutInPrison, 1, false), "candidate: in prison");
	CHECK(!ThrowoutCandidate(true, false, true, true, 0, 0, false) && !ThrowoutCandidate(true, false, true, true, 0, -1, false),
	      "candidate: walls flag outside");
	CHECK(!ThrowoutCandidate(true, false, true, true, 0, 1, true), "candidate: a held body is skipped");
}

static void CheckMode()
{
	CHECK(ThrowoutFinderModeFor(true) == TFM_CHAIN, "mode: another finder loaded chains the original");
	CHECK(ThrowoutFinderModeFor(false) == TFM_REPLACE, "mode: none loaded replaces the loop");
	CHECK(ThrowoutFinderModeBeforeTick() == TFM_CHAIN, "mode: before the first tick the finder chains the original");
	CHECK(ThrowoutChainFallback(true, false), "fallback: a held result with no live foreign hop runs the loop");
	CHECK(!ThrowoutChainFallback(true, true), "fallback: a held result with a live foreign hop is no candidate");
	CHECK(!ThrowoutChainFallback(false, false) && !ThrowoutChainFallback(false, true),
	      "fallback: a result not held is kept");
}

static void CheckHops()
{
	CHECK(ThrowoutClassifyHop(false, false, true, false, false) == THK_LIVE
	      && ThrowoutClassifyHop(false, true, false, false, false) == THK_LIVE,
	      "hop: unreadable bytes are live");
	CHECK(ThrowoutClassifyHop(true, false, false, true, false) == THK_OURS
	      && ThrowoutClassifyHop(true, true, false, true, false) == THK_OURS,
	      "hop: our own image continues from our trampoline");
	CHECK(ThrowoutClassifyHop(true, true, true, false, false) == THK_FOLLOW
	      && ThrowoutClassifyHop(true, true, false, false, true) == THK_FOLLOW
	      && ThrowoutClassifyHop(true, true, false, false, false) == THK_FOLLOW,
	      "hop: a jump is followed wherever it sits");
	CHECK(ThrowoutClassifyHop(true, false, true, false, false) == THK_END, "hop: vanilla code in the exe ends the walk");
	CHECK(ThrowoutClassifyHop(true, false, false, false, false) == THK_END,
	      "hop: a non-jump in memory no module owns (a trampoline) ends the walk");
	CHECK(ThrowoutClassifyHop(true, false, false, false, true) == THK_LIVE, "hop: another module's code is live");
	CHECK(kThrowoutMaxHops == 12, "hop: the walk stops at twelve hops");

	const unsigned __int64 at = 0x140001000ULL;
	const unsigned char e9[6] = { 0xE9, 0x10, 0x00, 0x00, 0x00, 0x90 };
	const unsigned char e9back[6] = { 0xE9, 0xFB, 0xFF, 0xFF, 0xFF, 0x90 };
	CHECK(ThrowoutJumpTarget(e9, at, 0) == at + 0x15 && ThrowoutJumpTarget(e9back, at, 0) == at
	      && ThrowoutJumpSlot(e9, at) == 0, "jump: E9 rel32 lands past its five bytes");
	const unsigned char ff25[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	CHECK(ThrowoutJumpSlot(ff25, at) == at + 6 && ThrowoutJumpTarget(ff25, at, 0x7FF812345678ULL) == 0x7FF812345678ULL,
	      "jump: FF 25 reads its pointer right after its six bytes");
	const unsigned char plain[6] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 };
	CHECK(ThrowoutJumpTarget(plain, at, 0) == 0 && ThrowoutJumpSlot(plain, at) == 0 && ThrowoutJumpTarget(NULL, at, 0) == 0,
	      "jump: other bytes are not a jump");
}

static void CheckSlots()
{
	// The town task's vftable as the binary has it: slot 5 at 0x60BE and slot 7 at 0x4C460, each
	// an E9 thunk to the slot row's function.
	const unsigned __int64 base = 0x140000000ULL;
	const unsigned char thunk5[5] = { 0xE9, 0x5D, 0x79, 0x33, 0x00 };
	const unsigned char thunk7[5] = { 0xE9, 0x3B, 0x92, 0x2E, 0x00 };
	CHECK(ThrowoutSlotResolves(thunk5, base + 0x60BE, base + 0x33DA20)
	      && ThrowoutSlotResolves(thunk7, base + 0x4C460, base + 0x3356A0),
	      "slots: through a thunk, the shipped entries reach the slot functions");
	CHECK(ThrowoutSlotResolves(NULL, base + 0x33DA20, base + 0x33DA20), "slots: a direct entry resolves");
	CHECK(!ThrowoutSlotResolves(thunk5, base + 0x60BE, base + 0x3356A0)
	      && !ThrowoutSlotResolves(thunk7, base + 0x4C460, base + 0x33DA20),
	      "slots: a thunk elsewhere does not resolve");
	const unsigned char other[5] = { 0x48, 0x5D, 0x79, 0x33, 0x00 };
	CHECK(!ThrowoutSlotResolves(other, base + 0x60BE, base + 0x33DA20), "slots: a non-thunk entry elsewhere does not resolve");
}

static void CheckTable()
{
	const double kCap = 1.0;   // the 60-minute hold
	const double now = 100.0;
	const double until = ThrowoutHoldExpiry(now, 60);

	CHECK(fixes::ThrowoutHoldClear() == 0 && fixes::ThrowoutHoldLive() == 0,
	      "table: clear on an idle table misses nothing");

	const bool added = fixes::ThrowoutHoldAdd(Key(1, 501), Hand(0x11), until);
	CHECK(added && fixes::ThrowoutHoldIsHeld(Key(1, 501), now, kCap), "table: an added key is held");
	CHECK(!fixes::ThrowoutHoldIsHeld(Key(1, 502), now, kCap) && !fixes::ThrowoutHoldIsHeld(Key(2, 501), now, kCap),
	      "table: another key is not held");
	CHECK(!fixes::ThrowoutHoldIsHeld(Key(1, 501), until, kCap) && !fixes::ThrowoutHoldIsHeld(Key(1, 501), until + 1.0, kCap),
	      "table: an expired entry is not held");

	bool found = false, bytes = false;
	int at = -1;
	for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS; ++i)
	{
		HandKey k;
		ThrowoutHand h;
		double e;
		if (fixes::ThrowoutHoldRead(i, &k, &h, &e) && game::HandKeyEqual(k, Key(1, 501)))
		{
			found = true;
			at = i;
			const ThrowoutHand want = Hand(0x11);
			bytes = e == until && std::memcmp(h.bytes, want.bytes, sizeof h.bytes) == 0;
		}
	}
	CHECK(found && bytes, "table: read returns the stored hand bytes");

	// Release with the expiry the read returned ends the entry.
	CHECK(at >= 0 && fixes::ThrowoutHoldRelease(at, Key(1, 501), until) && !fixes::ThrowoutHoldIsHeld(Key(1, 501), now, kCap),
	      "table: release with the read expiry ends the entry");

	// The same key thrown out again between the read and the release keeps its fresh hold.
	fixes::ThrowoutHoldClear();
	fixes::ThrowoutHoldAdd(Key(3, 503), Hand(0x33), until);
	int first = -1;
	HandKey rk;
	ThrowoutHand rh;
	double re = 0.0;
	for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS && first < 0; ++i)
		if (fixes::ThrowoutHoldRead(i, &rk, &rh, &re) && game::HandKeyEqual(rk, Key(3, 503)))
			first = i;
	bool kept = first >= 0;
	// Overwrite that entry with the same key and a later expiry: walk the ring once round.
	for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS - 1; ++i)
		fixes::ThrowoutHoldAdd(Key(1000 + i, 9), Hand(0x44), until);
	fixes::ThrowoutHoldAdd(Key(3, 503), Hand(0x55), until + 0.5);
	kept = kept && !fixes::ThrowoutHoldRelease(first, rk, re);
	// Asked half an hour on, when that re-add happened: its expiry lies within the cap.
	kept = kept && fixes::ThrowoutHoldIsHeld(Key(3, 503), now + 0.5, kCap);
	CHECK(kept, "table: release with another expiry keeps the entry");

	// 256 adds fill the ring; the 257th lands on the first one's entry.
	fixes::ThrowoutHoldClear();
	for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS; ++i)
		fixes::ThrowoutHoldAdd(Key(2000 + i, 7), Hand(0x66), until);
	bool full = fixes::ThrowoutHoldIsHeld(Key(2000, 7), now, kCap) && fixes::ThrowoutHoldLive() == fixes::THROWOUT_HOLD_SLOTS;
	fixes::ThrowoutHoldAdd(Key(5000, 7), Hand(0x77), until);
	CHECK(full && !fixes::ThrowoutHoldIsHeld(Key(2000, 7), now, kCap) && fixes::ThrowoutHoldIsHeld(Key(2001, 7), now, kCap)
	      && fixes::ThrowoutHoldIsHeld(Key(5000, 7), now, kCap), "table: the 257th add overwrites the first");

	const long missed = fixes::ThrowoutHoldClear();
	bool none = missed == 0 && fixes::ThrowoutHoldLive() == 0;
	for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS; ++i)
		none = none && !fixes::ThrowoutHoldIsHeld(Key(2000 + i, 7), now, kCap);
	CHECK(none && !fixes::ThrowoutHoldIsHeld(Key(5000, 7), now, kCap), "table: clear ends every entry");

	ThrowoutHand onStack;
	CHECK(__alignof(ThrowoutHand) == 8 && ((size_t)&onStack % 8) == 0 && sizeof(ThrowoutHand) == 32,
	      "table: the hand buffer is 8-aligned");
}

int main()
{
	CheckAsk();
	CheckFlag();
	CheckTimeout();
	CheckDrop();
	CheckHold();
	CheckCandidate();
	CheckMode();
	CheckHops();
	CheckSlots();
	CheckTable();
	return CheckExit("throwout_policy_units");
}
