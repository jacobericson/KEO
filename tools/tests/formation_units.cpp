#include <cstdio>
#include "movement/formation_members.h"

#include "check.h"

int main()
{
	// A newer order zeroes only the matching member.
	{
		uintptr_t chars[3]  = { 0x10, 0x20, 0x30 };
		uintptr_t movs[3]   = { 0x110, 0x120, 0x130 };
		uintptr_t detach[1] = { 0x20 };
		int n = FormationZeroMatchingMembers(chars, movs, 3, detach, 1);
		Check(n == 1, "one member zeroed");
		Check(chars[0] == 0x10 && movs[0] == 0x110, "member 0 untouched");
		Check(chars[1] == 0 && movs[1] == 0, "member 1 zeroed (character + charMovement)");
		Check(chars[2] == 0x30 && movs[2] == 0x130, "member 2 untouched");
	}

	// A slot already zero never matches, and re-detaching it is a no-op.
	{
		uintptr_t chars[2]  = { 0, 0x20 };
		uintptr_t movs[2]   = { 0, 0x120 };
		uintptr_t detach[1] = { 0 };
		int n = FormationZeroMatchingMembers(chars, movs, 2, detach, 1);
		Check(n == 0, "a zero character never matches a zero detach entry");
		Check(chars[1] == 0x20, "unrelated member untouched");
	}

	// Leader (index 0) zeroed: the rest of the group is untouched, so the
	// next alive member becomes the representative rather than the group
	// being killed outright.
	{
		uintptr_t chars[3]  = { 0x10, 0x20, 0x30 };
		uintptr_t movs[3]   = { 0x110, 0x120, 0x130 };
		uintptr_t detach[1] = { 0x10 };
		FormationZeroMatchingMembers(chars, movs, 3, detach, 1);
		Check(chars[0] == 0, "leader zeroed");
		Check(!FormationGroupExhausted(chars, 3), "group not exhausted while members 1 and 2 remain");
	}

	// Detaching every member exhausts the group.
	{
		uintptr_t chars[3]  = { 0x10, 0x20, 0x30 };
		uintptr_t movs[3]   = { 0x110, 0x120, 0x130 };
		uintptr_t detach[3] = { 0x10, 0x20, 0x30 };
		int n = FormationZeroMatchingMembers(chars, movs, 3, detach, 3);
		Check(n == 3, "every member zeroed");
		Check(FormationGroupExhausted(chars, 3), "group exhausted once every member is zero");
	}

	// An empty group counts as exhausted.
	Check(FormationGroupExhausted(NULL, 0), "an empty group is exhausted");

	// A 12-member roster losing 2 members to a newer order: not exhausted,
	// 10 members remain live.
	{
		uintptr_t chars[12];
		uintptr_t movs[12];
		for (int i = 0; i < 12; ++i) { chars[i] = 0x100 + i; movs[i] = 0x1000 + i; }
		uintptr_t detach[2] = { 0x105, 0x108 };
		int n = FormationZeroMatchingMembers(chars, movs, 12, detach, 2);
		Check(n == 2, "two of twelve zeroed");
		Check(!FormationGroupExhausted(chars, 12), "ten members remain, not exhausted");
		int live = 0;
		for (int i = 0; i < 12; ++i) if (chars[i]) ++live;
		Check(live == 10, "exactly ten members left live");
	}

	// FormationSameMemberSet: exact match is order-independent; a subset,
	// a superset, or a disjoint set is never a match.
	{
		uintptr_t existing[3] = { 0x10, 0x20, 0x30 };
		uintptr_t sameOrder[3]   = { 0x10, 0x20, 0x30 };
		uintptr_t sameReordered[3] = { 0x30, 0x10, 0x20 };
		uintptr_t subset[2]      = { 0x10, 0x20 };
		uintptr_t superset[4]    = { 0x10, 0x20, 0x30, 0x40 };
		uintptr_t disjoint[3]    = { 0x40, 0x50, 0x60 };
		Check(FormationSameMemberSet(existing, 3, sameOrder, 3), "identical order matches");
		Check(FormationSameMemberSet(existing, 3, sameReordered, 3), "same set, different order matches");
		Check(!FormationSameMemberSet(existing, 3, subset, 2), "a subset is not the same set");
		Check(!FormationSameMemberSet(existing, 3, superset, 4), "a superset is not the same set");
		Check(!FormationSameMemberSet(existing, 3, disjoint, 3), "a disjoint set is not the same set");

		// A zeroed (already-detached) slot among the existing members is not
		// live and must not count toward the set.
		uintptr_t withGap[3] = { 0x10, 0, 0x30 };
		uintptr_t twoLive[2] = { 0x10, 0x30 };
		Check(FormationSameMemberSet(withGap, 3, twoLive, 2), "live members only match, ignoring a zeroed slot");
		Check(!FormationSameMemberSet(withGap, 3, existing, 3), "a zeroed slot never matches the missing member");
	}

	// Gather-completion radius grows with squad size, floored at 40 units.
	{
		float r1  = FormationGatherRadius(1);
		float r20 = FormationGatherRadius(20);
		float r50 = FormationGatherRadius(50);
		Check(r1 == 40.0f, "n=1 floors at 40");
		Check(r20 > 44.5f && r20 < 44.7f, "n=20 is about 44.6");
		Check(r50 > 64.7f && r50 < 64.9f, "n=50 is about 64.8");
	}

	// FormationMemberNewlyHeld: only an off-at-creation -> on-now transition
	// counts. Already-holding or already-inSomething at creation never
	// triggers it, no matter its current value.
	Check(FormationMemberNewlyHeld(false, true, false, false), "hold picked up after creation");
	Check(FormationMemberNewlyHeld(false, false, false, true), "inSomething picked up after creation");
	Check(!FormationMemberNewlyHeld(true, true, false, false), "already holding at creation stays");
	Check(!FormationMemberNewlyHeld(false, false, true, true), "already inSomething at creation stays");
	Check(!FormationMemberNewlyHeld(false, false, false, false), "neither state set");
	Check(!FormationMemberNewlyHeld(true, false, false, false), "hold cleared since creation");

	// Max pairwise spread: live members only, and 0 whenever there is nothing
	// two members apart to measure.
	{
		float xs[4] = { 0.0f, 30.0f, 0.0f, 300.0f };
		float zs[4] = { 0.0f,  40.0f, 0.0f,   400.0f };
		uintptr_t all[4]  = { 0x10, 0x20, 0x30, 0x40 };
		uintptr_t noFar[4] = { 0x10, 0x20, 0x30, 0 };
		uintptr_t one[4]  = { 0, 0x20, 0, 0 };
		uintptr_t none[4] = { 0, 0, 0, 0 };

		float s = FormationMaxPairwiseSpread(xs, zs, all, 4);
		Check(s > 499.9f && s < 500.1f, "worst pair is member 0 to member 3 (500)");
		float sNoFar = FormationMaxPairwiseSpread(xs, zs, noFar, 4);
		Check(sNoFar > 49.9f && sNoFar < 50.1f, "a zeroed straggler is excluded (50)");
		Check(FormationMaxPairwiseSpread(xs, zs, one, 4) == 0.0f, "one live member is 0");
		Check(FormationMaxPairwiseSpread(xs, zs, none, 4) == 0.0f, "no live member is 0");
		Check(FormationMaxPairwiseSpread(xs, zs, all, 1) == 0.0f, "count 1 is 0");
		Check(FormationMaxPairwiseSpread(0, zs, all, 4) == 0.0f, "null positions are 0");
	}

	return CheckExit("formation_units");
}
