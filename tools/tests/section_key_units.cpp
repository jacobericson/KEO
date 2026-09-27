// Host tests for the section-key arithmetic the crash record is read against.
// No game headers, no KenshiLib: the policy is pure by design.

#include "fixes/streaming/section_key_policy.h"
#include <cstdio>

#include "check.h"


static unsigned int PackKey(unsigned int section, unsigned int element)
{
	return (section << SECTION_KEY_INDEX_SHIFT) | (element & SECTION_KEY_ELEMENT_MASK);
}

int main()
{
	// The two halves come apart the way the engine splits them.
	Check(SectionKeyIndex(PackKey(59, 113)) == 59, "index half");
	Check(SectionKeyElement(PackKey(59, 113)) == 113, "element half");
	Check(SectionKeyElement(PackKey(1023, 0x3FFFFF)) == 0x3FFFFF, "element half, full");

	// Ten bits: never negative, never past 1023, whatever the key holds.
	Check(SectionKeyIndex(0xFFFFFFFFu) == 1023, "index half of an all-ones key");
	Check(SectionKeyIndex(0) == 0, "index half of a zero key");

	// The reading the record turns on.
	Check(SectionKeyClassify(PackKey(5, 0), 289) == SECTION_KEY_IN_RANGE, "inside");
	Check(SectionKeyClassify(PackKey(288, 0), 289) == SECTION_KEY_IN_RANGE, "last slot");
	Check(SectionKeyClassify(PackKey(289, 0), 289) == SECTION_KEY_OUT_OF_RANGE, "one past");
	Check(SectionKeyClassify(PackKey(1023, 0), 289) == SECTION_KEY_OUT_OF_RANGE, "far past");

	// An empty or unreadable array has no slot to hit, so every key misses it.
	Check(SectionKeyClassify(PackKey(0, 0), 0) == SECTION_KEY_OUT_OF_RANGE, "empty array");
	Check(SectionKeyClassify(PackKey(0, 0), -1) == SECTION_KEY_OUT_OF_RANGE, "size unread");

	// The element half never changes the answer.
	Check(SectionKeyClassify(PackKey(5, 0x3FFFFF), 289) == SECTION_KEY_IN_RANGE,
		"element half is ignored");

	Check(SectionKeyCountPlausible(0), "zero keys is plausible");
	Check(SectionKeyCountPlausible(SECTION_KEY_MAX_KEYS), "the cap itself is plausible");
	Check(!SectionKeyCountPlausible(-1), "a negative count is not");
	Check(!SectionKeyCountPlausible(SECTION_KEY_MAX_KEYS + 1), "past the cap is not");

	return CheckExit("section_key_units");
}
