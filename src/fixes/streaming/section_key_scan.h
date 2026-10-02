#ifndef KEO_SECTION_KEY_SCAN_H
#define KEO_SECTION_KEY_SCAN_H

// Classifies the section-table lookups a call is about to make and leaves the
// result in the ring. Separate from the detours so it can be exercised, and
// faulted, without the game: every read it makes is fault-guarded, it writes
// nothing back and it returns no answer for anyone to branch on.

// ctx holds the collection and a second pending key list; keyArray is the
// caller's hkArray<int>, and may be NULL.
void SectionKeyScanClearance(const void* ctx, const void* keyArray);

void SectionKeyScanCut(const void* collection, unsigned int packedKey);

#endif
