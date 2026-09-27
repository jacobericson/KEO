#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/section_key_ring.h"

#ifdef ZONEOPT_DEBUG
#include "base/core.h"
#include "base/fixed_log_buf.h"

// Offsets inside the streaming collection: the instance array's data pointer
// and element count. Held here rather than taken from game.h so this file
// builds on its own, like the other recorders in this folder.
static const size_t kCollInstancesData = 32;
static const size_t kCollInstancesSize = 40;

// One ring per site, because the two fire at very different rates: a single
// ring would hold nothing but the hot site's entries by the time a fault in
// the other one is recorded.
static const int kRingSize = 8;
static const int kSiteCount = 2;

static SectionKeyEntry s_ring[kSiteCount][kRingSize];
static volatile LONG   s_next[kSiteCount] = { 0, 0 };   // calls, and the record number

static int SiteSlot(unsigned char site)
{
	return site == SECTION_KEY_SITE_CUT ? 1 : 0;
}

static volatile LONG s_callsClearance = 0;
static volatile LONG s_callsCut       = 0;
static volatile LONG s_keys           = 0;
static volatile LONG s_oob            = 0;
static volatile LONG s_nullSlots      = 0;
static volatile LONG s_unreadable     = 0;
static volatile LONG s_implausible    = 0;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// Install time only, before the detours exist, so the plain stores below race
// nothing. Anything that could call it with the detours live would need the
// counter reset to be interlocked.
void SectionKeyRingInit()
{
	for (int r = 0; r < kSiteCount; ++r)
	{
		for (int i = 0; i < kRingSize; ++i)
		{
			SectionKeyEntry& e = s_ring[r][i];
			e.seq = 0;
			e.site = SECTION_KEY_SITE_NONE;
		}
		s_next[r] = 0;
	}
}

SectionKeyEntry* SectionKeyRingReserve(unsigned char site)
{
	int r = SiteSlot(site);
	LONG n = InterlockedIncrement(&s_next[r]);
	SectionKeyEntry* e = &s_ring[r][(n - 1) % kRingSize];

	// Unpublished first: a reader that arrives while this slot is being filled
	// must skip it rather than read half of the previous entry and half of this
	// one.
	InterlockedExchange(&e->seq, 0);

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	e->number = n;
	e->qpc = (unsigned __int64)now.QuadPart;
	e->tid = GetCurrentThreadId();
	e->site = site;
	e->flags = 0;
	e->method = 0xFF;
	e->coll = 0;
	e->tableBase = 0;
	e->tableSize = -1;
	e->keyCount = 0;
	e->keysA = 0;
	e->keysB = 0;
	e->minIndex = -1;
	e->maxIndex = -1;
	e->oobCount = 0;
	e->oobA = 0;
	e->oobB = 0;
	e->nullSlots = 0;
	e->firstNullIndex = -1;
	e->firstOobKey = 0;
	e->maxIndexKey = 0;

	return e;
}

// The slot's own call number, not the counter's current value: another thread
// may have reserved since, and a slot published under someone else's number
// would be dropped by the formatter's identity check.
void SectionKeyRingPublish(SectionKeyEntry* entry)
{
	if (!entry)
		return;
	InterlockedExchange(&entry->seq, entry->number);
}

void SectionKeyRingCount(unsigned char site, int keys, int oob, int nullSlots,
                         bool unreadable, bool implausible)
{
	if (site == SECTION_KEY_SITE_CUT)
		InterlockedIncrement(&s_callsCut);
	else
		InterlockedIncrement(&s_callsClearance);
	if (keys > 0)      InterlockedExchangeAdd(&s_keys, keys);
	if (oob > 0)       InterlockedExchangeAdd(&s_oob, oob);
	if (nullSlots > 0) InterlockedExchangeAdd(&s_nullSlots, nullSlots);
	if (unreadable)    InterlockedIncrement(&s_unreadable);
	if (implausible)   InterlockedIncrement(&s_implausible);
}

long SectionKeyRingCalls()
{
	return (long)(Read(&s_callsClearance) + Read(&s_callsCut));
}

// --- fixed-buffer formatting ---

typedef FlbExternal SkBuf;

// The collection as it stands now, not as it stood when the entry was taken.
// A difference between the two is the reading that says the array moved under
// the call rather than the key being wrong. POD-only and standalone: MSVC 2010
// rejects __try in a function that also holds an object needing unwinding.
static bool ReadCollectionNow(unsigned __int64 coll, unsigned __int64* base, int* size)
{
	if (!coll)
		return false;
	bool ok = true;
	GuardEnter();
	__try
	{
		*base = *(const unsigned __int64*)((const unsigned char*)coll + kCollInstancesData);
		*size = *(const int*)((const unsigned char*)coll + kCollInstancesSize);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static void FormatEntry(SkBuf* o, const SectionKeyEntry& e)
{
	FlbStr(o, "\r\n  SKEY #");
	FlbDec(o, (__int64)e.seq);
	FlbStr(o, " site=");
	FlbStr(o, e.site == SECTION_KEY_SITE_CUT ? "cut" : "clearance");
	FlbStr(o, " tid=");      FlbDec(o, (__int64)(unsigned long)e.tid);
	FlbStr(o, " qpc=0x");    FlbHexDigits(o, e.qpc, 16);
	if (e.site == SECTION_KEY_SITE_CLEARANCE)
	{
		FlbStr(o, " method=");
		FlbDec(o, (__int64)e.method);
	}
	FlbStr(o, " coll=0x");   FlbHexDigits(o, e.coll, 16);
	FlbStr(o, " table=0x");  FlbHexDigits(o, e.tableBase, 16);
	FlbChar(o, '/');         FlbDec(o, (__int64)e.tableSize);
	FlbStr(o, " keys=");     FlbDec(o, (__int64)e.keyCount);
	FlbStr(o, "(a=");        FlbDec(o, (__int64)e.keysA);
	FlbStr(o, ",b=");        FlbDec(o, (__int64)e.keysB);
	FlbStr(o, ") idx=");     FlbDec(o, (__int64)e.minIndex);
	FlbStr(o, "..");         FlbDec(o, (__int64)e.maxIndex);
	FlbStr(o, " oob=");      FlbDec(o, (__int64)e.oobCount);
	FlbStr(o, "(a=");        FlbDec(o, (__int64)e.oobA);
	FlbStr(o, ",b=");        FlbDec(o, (__int64)e.oobB);
	FlbStr(o, ") null=");    FlbDec(o, (__int64)e.nullSlots);
	if (e.nullSlots > 0)
	{
		FlbStr(o, "@");
		FlbDec(o, (__int64)e.firstNullIndex);
	}
	FlbStr(o, " maxIdxKey=0x"); FlbHexDigits(o, (unsigned __int64)e.maxIndexKey, 8);
	if (e.oobCount > 0)
	{
		FlbStr(o, " firstOobKey=0x");
		FlbHexDigits(o, (unsigned __int64)e.firstOobKey, 8);
	}
	if (e.flags & SECTION_KEY_FLAG_UNREADABLE)  FlbStr(o, " unreadable=1");
	if (e.flags & SECTION_KEY_FLAG_IMPLAUSIBLE) FlbStr(o, " implausible=1");
	if (e.flags & SECTION_KEY_FLAG_NO_COLL)     FlbStr(o, " noColl=1");

	unsigned __int64 nowBase = 0;
	int nowSize = 0;
	if (ReadCollectionNow(e.coll, &nowBase, &nowSize))
	{
		FlbStr(o, " now=0x"); FlbHexDigits(o, nowBase, 16);
		FlbChar(o, '/');      FlbDec(o, (__int64)nowSize);
		if (nowBase != e.tableBase || nowSize != e.tableSize)
			FlbStr(o, " MOVED");
	}
	else
	{
		FlbStr(o, " now=unreadable");
	}
}

size_t SectionKeyRingFormat(char* buf, size_t cap, int maxEntries)
{
	SkBuf o;
	o.b = buf;
	o.cap = cap;
	o.n = 0;
	if (!buf || cap == 0)
		return 0;

	LONG clearance = Read(&s_next[0]);
	LONG cut = Read(&s_next[1]);
	FlbStr(&o, "  SectionKeyRing: clearance=");
	FlbDec(&o, (__int64)clearance);
	FlbStr(&o, " cut=");
	FlbDec(&o, (__int64)cut);
	if (clearance == 0 && cut == 0)
	{
		// A ring with nothing in it says the probe never ran, which is a
		// finding about the instrument rather than about the fault.
		FlbStr(&o, " (the probe recorded no call before this fault)");
	}

	for (int r = 0; r < kSiteCount; ++r)
	{
		int shown = 0;
		for (LONG n = Read(&s_next[r]); n > 0 && shown < maxEntries; --n)
		{
			const SectionKeyEntry& e = s_ring[r][(n - 1) % kRingSize];
			if (Read((volatile LONG*)&e.seq) != n)
				continue;   // never written, overwritten, or being written now
			FormatEntry(&o, e);
			++shown;
		}
	}
	FlbStr(&o, "\r\n");
	if (o.n < o.cap)
		o.b[o.n] = '\0';
	return o.n;
}

void SectionKeyRingHeartbeatLine(char* buf, size_t cap)
{
	SkBuf o;
	o.b = buf;
	o.cap = cap;
	o.n = 0;
	FlbStr(&o, "SectionKeyProbe running: clearance="); FlbDec(&o, Read(&s_callsClearance));
	FlbStr(&o, " cut=");         FlbDec(&o, Read(&s_callsCut));
	FlbStr(&o, " keys=");        FlbDec(&o, Read(&s_keys));
	FlbStr(&o, " oob=");         FlbDec(&o, Read(&s_oob));
	FlbStr(&o, " nullSlots=");   FlbDec(&o, Read(&s_nullSlots));
	FlbStr(&o, " unreadable=");  FlbDec(&o, Read(&s_unreadable));
	FlbStr(&o, " implausible="); FlbDec(&o, Read(&s_implausible));
	if (o.n < o.cap)
		o.b[o.n] = '\0';
}

#else

// Nothing of this exists outside a DEV build; plugin_entry.cpp does not name it.
typedef int SectionKeyRingNotInThisBuild;

#endif // ZONEOPT_DEBUG
