#ifndef ZONEOPT_SECTION_KEY_RING_H
#define ZONEOPT_SECTION_KEY_RING_H

#include <windows.h>

// The last N section-table lookups the navmesh step was about to make, so a
// fault inside one can be read afterwards. Nothing here logs or allocates: an
// entry is filled in place and published with one interlocked store, and the
// crash path only formats what is already there.

enum SectionKeySite
{
	SECTION_KEY_SITE_NONE      = 0,
	SECTION_KEY_SITE_CLEARANCE = 1,  // the clearance reset's key loop
	SECTION_KEY_SITE_CUT       = 2   // the per-key cut lookup
};

enum SectionKeyFlags
{
	SECTION_KEY_FLAG_UNREADABLE  = 0x01,  // a read faulted; the entry is partial
	SECTION_KEY_FLAG_IMPLAUSIBLE = 0x02,  // a key count this scan refused to walk
	SECTION_KEY_FLAG_NO_COLL     = 0x04   // the collection pointer was NULL
};

struct SectionKeyEntry
{
	volatile LONG    seq;        // published last; 0 means never written
	LONG             number;     // the call number this slot was reserved for
	unsigned __int64 qpc;
	unsigned long    tid;
	unsigned char    site;
	unsigned char    flags;
	unsigned char    method;     // the reset method byte, for the clearance site
	unsigned __int64 coll;       // the streaming collection, re-read at crash time
	unsigned __int64 tableBase;
	int              tableSize;
	int              keyCount;   // keys this entry classified
	int              keysA;      // from the caller's array
	int              keysB;      // from the context's own pending list
	int              minIndex;
	int              maxIndex;
	int              oobCount;
	int              oobA;
	int              oobB;
	int              nullSlots;
	int              firstNullIndex;
	unsigned int     firstOobKey;
	unsigned int     maxIndexKey;
};

void SectionKeyRingInit();

// Takes the next slot and stamps everything that does not depend on a read of
// game memory. Never NULL.
SectionKeyEntry* SectionKeyRingReserve(unsigned char site);

void SectionKeyRingPublish(SectionKeyEntry* entry);

// Writes up to maxEntries entries, most recent first, into buf. Returns the
// bytes written (never more than cap). Safe on the crash path: fixed buffer,
// no allocation, and every read of game memory it makes is fault-guarded.
size_t SectionKeyRingFormat(char* buf, size_t cap, int maxEntries);

// Totals for the heartbeat, so a silent ring and an absent instrument are not
// the same reading.
void SectionKeyRingCount(unsigned char site, int keys, int oob, int nullSlots,
                         bool unreadable, bool implausible);
void SectionKeyRingHeartbeatLine(char* buf, size_t cap);
long SectionKeyRingCalls();

#endif
