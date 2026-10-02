#ifndef KEO_SECTION_KEY_POLICY_H
#define KEO_SECTION_KEY_POLICY_H

// The arithmetic behind a streaming-collection section lookup, kept pure so the
// decision a record is read against can be tested off the game.
//
// A packed key names a section in its top 10 bits and an element in its low 22.
// The engine indexes the collection's instance array with the section half and
// makes no bounds test, so "was the index inside the array" is the whole
// question these functions answer.

enum SectionKeyAction
{
	SECTION_KEY_IN_RANGE,
	SECTION_KEY_OUT_OF_RANGE
};

const int SECTION_KEY_INDEX_SHIFT = 22;
const unsigned int SECTION_KEY_ELEMENT_MASK = 0x3FFFFFu;

// The largest key count this scan will walk. Past it the count is not trusted
// and the source is recorded as implausible rather than followed.
const int SECTION_KEY_MAX_KEYS = 65536;

int SectionKeyIndex(unsigned int packedKey);
int SectionKeyElement(unsigned int packedKey);

// tableSize <= 0 makes every key out of range: an empty array has no slot to
// hit, which is exactly the shape one of the recorded faults had.
SectionKeyAction SectionKeyClassify(unsigned int packedKey, int tableSize);

bool SectionKeyCountPlausible(int count);

#endif
