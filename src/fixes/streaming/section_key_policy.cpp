#include "fixes/streaming/section_key_policy.h"

int SectionKeyIndex(unsigned int packedKey)
{
	// Unsigned: the index half is 10 bits, so it is never negative and never
	// exceeds 1023 however corrupt the key is. A recorded index near 1023
	// against a table of a few hundred is therefore a key fault, not a
	// sign-extension artefact.
	return (int)(packedKey >> SECTION_KEY_INDEX_SHIFT);
}

int SectionKeyElement(unsigned int packedKey)
{
	return (int)(packedKey & SECTION_KEY_ELEMENT_MASK);
}

SectionKeyAction SectionKeyClassify(unsigned int packedKey, int tableSize)
{
	int index = SectionKeyIndex(packedKey);
	if (tableSize <= 0 || index >= tableSize)
		return SECTION_KEY_OUT_OF_RANGE;
	return SECTION_KEY_IN_RANGE;
}

bool SectionKeyCountPlausible(int count)
{
	return count >= 0 && count <= SECTION_KEY_MAX_KEYS;
}
