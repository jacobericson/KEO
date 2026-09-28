#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/stitch/stitch_byte_guard.h"
#include "fixes/stitch/stitch_byte_guard_policy.h"
#include "fixes/near_page.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include "base/config.h"
#include "diag/module_bases.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>

// calls    == inObject + sector + interior + unknown
// interior == skipped + kept
static volatile LONG s_calls     = 0;
static volatile LONG s_inObject  = 0;
static volatile LONG s_sector    = 0;
static volatile LONG s_interior  = 0;
static volatile LONG s_unknown   = 0;   // a stitch whose uid read faulted; the store is kept
static volatile LONG s_skipped   = 0;
static volatile LONG s_kept      = 0;   // interior stores left in place (observe mode)
static volatile LONG s_nonzero   = 0;   // the byte at +0x50 was not already 0
static volatile LONG s_unreadable = 0;  // the neighbour qword could not be read

// Read once at install, before the site is patched.
static bool s_actMode = true;

// Path-thread interior stores fill a ring slot after clearing seq, then
// atomically publish its number. Main drain and any-thread crash formatting
// check seq once before reading the live entry; a later overwrite can mix
// diagnostic values and is tolerated. Installation clears the ring once;
// later entries wrap. No behavior decision reads this diagnostic payload.
namespace stitch_byte_guard_detail {
struct StitchByteEntry
{
	volatile LONG    seq;     // 0 = unpublished
	LONG             number;
	unsigned __int64 output;
	unsigned __int64 prior;   // the qword at output+0x50 before the store
	unsigned __int64 qpc;
	unsigned int     uid;
	unsigned long    tid;
	unsigned char    readable;
	unsigned char    skipped;
};
} // namespace stitch_byte_guard_detail
using namespace stitch_byte_guard_detail;

static const int kRingSize = 16;
static StitchByteEntry s_ring[kRingSize];
static volatile LONG   s_next = 0;
static LONG            s_drained = 0;   // main thread only
static LONG            s_lost = 0;      // main thread only: overwritten before drained
static __int64         s_qpcFreq = 0;

// 0 = not attempted, 1 = armed, -1 = refused.
static int s_state = 0;
static const char* s_why = "not installed";
static double s_lastBeat = -1.0;
static const double kBeatSeconds = 60.0;

// The data page holds the gate slot; the code page holds the stub and the
// keep-all thunk. Never freed once the site is patched: the path thread can
// be inside the stub at any moment.
static unsigned char* s_page     = NULL;
static unsigned char* s_codePage = NULL;
static const size_t kDataPageSize = 0x1000;
static const size_t kCodePageSize = 0x1000;
static const size_t kKeepAllOffset = 0x40;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }


// --- SEH-guarded reads. Standalone and POD-only: MSVC 2010 rejects __try in
// a function that also holds an object needing unwinding. ---

static bool SafeReadQword(unsigned __int64 addr, unsigned __int64* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const unsigned __int64*)(uintptr_t)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool SafeReadDword(unsigned __int64 addr, unsigned int* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const unsigned int*)(uintptr_t)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool SafeReadBytes(const void* addr, void* out, size_t n)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		memcpy(out, addr, n);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}


static void Record(unsigned __int64 output, unsigned int uid, bool readable,
                   unsigned __int64 prior, bool skipped)
{
	LONG n = InterlockedIncrement(&s_next);
	StitchByteEntry* e = &s_ring[(n - 1) % kRingSize];
	InterlockedExchange(&e->seq, 0);

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	e->number   = n;
	e->output   = output;
	e->prior    = prior;
	e->qpc      = (unsigned __int64)now.QuadPart;
	e->uid      = uid;
	e->tid      = GetCurrentThreadId();
	e->readable = readable ? 1 : 0;
	e->skipped  = skipped ? 1 : 0;

	InterlockedExchange(&e->seq, n);
}

// Called from the stub for every drained task that is not type 3, on the path
// thread. No allocation, no lock, no logging. Nonzero keeps the store.
extern "C" int __fastcall ZoneOpt_StitchByteGate(const void* output, int type);

extern "C" int __fastcall ZoneOpt_StitchByteGate(const void* output, int type)
{
	InterlockedIncrement(&s_calls);

	const unsigned __int64 out = (unsigned __int64)(uintptr_t)output;
	unsigned int uid = 0;
	bool uidRead = false;
	if (type == kStitchByteStitchType)
		uidRead = SafeReadDword(out + kStitchByteUidOffset, &uid);

	const StitchByteClass cls = ClassifyStitchByteWrite(type, uidRead, uid);
	switch (cls)
	{
	case SBG_IN_OBJECT:      InterlockedIncrement(&s_inObject); return 1;
	case SBG_STITCH_SECTOR:  InterlockedIncrement(&s_sector);   return 1;
	case SBG_STITCH_UNKNOWN: InterlockedIncrement(&s_unknown);  return 1;
	default: break;
	}

	InterlockedIncrement(&s_interior);
	unsigned __int64 prior = 0;
	const bool readable = SafeReadQword(out + kStitchByteWriteOffset, &prior);
	if (!readable)
		InterlockedIncrement(&s_unreadable);
	else if ((prior & 0xFF) != 0)
		InterlockedIncrement(&s_nonzero);

	const bool skip = StitchByteSkipsWrite(cls, s_actMode);
	InterlockedIncrement(skip ? &s_skipped : &s_kept);
	Record(out, uid, readable, prior, skip);
	return skip ? 0 : 1;
}


// Havok.log prints uids as bare lowercase hex; matching it lets a line be
// found next to the log's own "Job added: Stitch".
static void FlbUid(FixedLogBuf* o, unsigned int v)
{
	char t[8];
	int n = 0;
	do { int d = (int)(v & 15); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
	while (v && n < 8);
	while (n)
		FlbChar(o, t[--n]);
}

static const GuardCounter kBeatRows[] =
{
	{ "calls",      GF_COUNT, &s_calls,      0 },
	{ "inObject",   GF_COUNT, &s_inObject,   0 },
	{ "sector",     GF_COUNT, &s_sector,     0 },
	{ "interior",   GF_COUNT, &s_interior,   0 },
	{ "unknown",    GF_COUNT, &s_unknown,    0 },
	{ "skipped",    GF_COUNT, &s_skipped,    0 },
	{ "kept",       GF_COUNT, &s_kept,       0 },
	{ "nonzero",    GF_COUNT, &s_nonzero,    0 },
	{ "unreadable", GF_COUNT, &s_unreadable, 0 },
};

static void EmitHeartbeat()
{
	const bool live = s_state == 1;
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "StitchByteGuard running:");
	if (!live)
	{
		FlbStr(&o, " armed=no("); FlbStr(&o, s_why); FlbStr(&o, ")");
	}
	FlbStr(&o, " mode="); FlbStr(&o, s_actMode ? "guard" : "observe");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), live);
	FlbStr(&o, " lost=");
	if (live) FlbDec(&o, s_lost); else FlbChar(&o, '?');
	LogMsg(FlbDone(&o));
}

// What the qword at +0x50 looks like: zero (the interior-neighbour case,
// whose vptr getInterior and loadZone clear), an address inside a loaded
// module (a live object's vptr), or anything else.
static void FlbNeighbour(FixedLogBuf* o, unsigned __int64 prior)
{
	if (prior == 0)
	{
		FlbStr(o, "zero");
		return;
	}
	int count = 0;
	const ModuleBaseEntry* mods = CapturedModuleBases(&count);
	unsigned __int64 off = 0;
	int idx = ResolveModuleForAddress(mods, count, prior, &off);
	if (idx >= 0)
	{
		FlbStr(o, "vptr:");
		FlbStr(o, mods[idx].name);
		FlbStr(o, "+");
		FlbHex(o, off);
	}
	else
	{
		FlbStr(o, "data");
	}
}

static void EmitEntryLine(const StitchByteEntry& e)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "StitchByteGuard FIRED: #"); FlbDec(&o, e.number);
	FlbStr(&o, " uid=");    FlbUid(&o, e.uid);
	FlbStr(&o, " output="); FlbHex(&o, e.output);
	FlbStr(&o, " +0x50=");
	if (e.readable)
	{
		FlbHex(&o, e.prior);
		FlbStr(&o, " byte0="); FlbHex(&o, e.prior & 0xFF);
		FlbStr(&o, " neighbour="); FlbNeighbour(&o, e.prior);
	}
	else
	{
		FlbStr(&o, "unreadable");
	}
	unsigned __int64 now = 0;
	FlbStr(&o, " now=");
	if (SafeReadQword(e.output + kStitchByteWriteOffset, &now))
		FlbHex(&o, now);
	else
		FlbStr(&o, "unreadable");
	FlbStr(&o, e.skipped ? "; interior stitch -- store skipped"
	                     : "; interior stitch -- store kept (observe mode)");
	FlbStr(&o, " tid="); FlbDec(&o, (__int64)e.tid);
	LogMsg(FlbDone(&o));
}

static void DrainEntries()
{
	const LONG last = Read(&s_next);
	while (s_drained < last)
	{
		const LONG n = s_drained + 1;
		const StitchByteEntry& e = s_ring[(n - 1) % kRingSize];
		if (Read((volatile LONG*)&e.seq) == n)
			EmitEntryLine(e);
		else if (last - n >= kRingSize)
			++s_lost;
		else
			break;   // still being written; next frame
		s_drained = n;
	}
}

void StitchByteGuardTick(double now)
{
	if (s_state == 0)
		return;
	if (s_state == 1)
		DrainEntries();
	if (now - s_lastBeat < kBeatSeconds)
		return;
	s_lastBeat = now;
	EmitHeartbeat();
}


static const char* TryArm()
{
	unsigned __int64 navmesh = 0;
	if (!SafeReadQword((unsigned __int64)(uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR), &navmesh))
		return "pauseState.navmesh unreadable";
	if (navmesh != 0)
		return "the navmesh already exists; the site cannot be written while its thread may run it";

	const unsigned __int64 siteAddr   = (unsigned __int64)gameBase + kStitchByteSiteRva;
	const unsigned __int64 resumeAddr = (unsigned __int64)gameBase + kStitchByteResumeRva;
	const unsigned __int64 branchAddr = (unsigned __int64)gameBase + kStitchByteBranchRva;

	unsigned char site[kStitchByteSiteLen];
	unsigned char resume[kStitchByteResumeLen];
	unsigned char branch[kStitchByteBranchLen];
	if (!SafeReadBytes((const void*)(uintptr_t)siteAddr, site, sizeof(site))
	 || !SafeReadBytes((const void*)(uintptr_t)resumeAddr, resume, sizeof(resume))
	 || !SafeReadBytes((const void*)(uintptr_t)branchAddr, branch, sizeof(branch)))
		return "the site, its branch or its resume target is unreadable";
	if (!StitchByteBytesMatch(site, kStitchByteSiteBytes, kStitchByteSiteLen))
		return "site bytes differ";
	if (!StitchByteBytesMatch(resume, kStitchByteResumeBytes, kStitchByteResumeLen)
	 || !StitchByteBytesMatch(branch, kStitchByteBranchBytes, kStitchByteBranchLen))
		return "the branch or resume bytes differ";

	unsigned char* page = AllocateNearPages((unsigned __int64)gameBase,
	                                        kDataPageSize + kCodePageSize);
	if (!page)
		return "no free page within rel32 reach of the exe";

	unsigned char* codePage = page + kDataPageSize;
	const unsigned __int64 gateSlotAddr = (unsigned __int64)(uintptr_t)page;
	const unsigned __int64 stubAddr     = (unsigned __int64)(uintptr_t)codePage;

	unsigned char patch[kStitchByteSiteLen];
	if (!BuildStitchByteStub(codePage, kCodePageSize, stubAddr, gateSlotAddr, resumeAddr, NULL)
	 || !BuildStitchByteKeepAll(codePage + kKeepAllOffset, kCodePageSize - kKeepAllOffset)
	 || !BuildStitchByteSitePatch(site, siteAddr, stubAddr, patch))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		return "the stub's displacements do not reach from the page found";
	}

	*(volatile unsigned __int64*)page = (unsigned __int64)(uintptr_t)&ZoneOpt_StitchByteGate;

	DWORD oldProtect = 0;
	if (!VirtualProtect(codePage, kCodePageSize, PAGE_EXECUTE_READ, &oldProtect))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		return "the stub page could not be made executable";
	}
	FlushInstructionCache(GetCurrentProcess(), codePage, kCodePageSize);

	DWORD oldSite = 0;
	if (!VirtualProtect((void*)(uintptr_t)siteAddr, kStitchByteSiteLen,
	                    PAGE_EXECUTE_READWRITE, &oldSite))
		return "VirtualProtect on the site failed";   // the page stays; nothing points at it
	memcpy((void*)(uintptr_t)siteAddr, patch, kStitchByteSiteLen);
	DWORD ignore = 0;
	VirtualProtect((void*)(uintptr_t)siteAddr, kStitchByteSiteLen, oldSite, &ignore);
	FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)siteAddr, kStitchByteSiteLen);

	s_page = page;
	s_codePage = codePage;

	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "StitchByteGuard: armed at exe+"); FlbHex(&o, kStitchByteSiteRva);
	FlbStr(&o, " stub="); FlbHex(&o, stubAddr);
	FlbStr(&o, " mode="); FlbStr(&o, s_actMode ? "guard" : "observe");
	FlbStr(&o, " (a heartbeat line follows every minute)");
	LogMsg(FlbDone(&o));
	return NULL;
}

void InstallStitchByteGuard(bool allowed)
{
	// The key chooses skip or keep, never whether the site is watched.
	s_actMode = fixes::g_fixesCfg.stitchByteGuardEnabled;

	LARGE_INTEGER f;
	s_qpcFreq = QueryPerformanceFrequency(&f) ? f.QuadPart : 0;

	for (int i = 0; i < kRingSize; ++i)
		s_ring[i].seq = 0;

	const char* why = allowed ? TryArm() : "the build gate refused";
	if (!why)
	{
		s_state = 1;
		s_why = "";
	}
	else
	{
		s_state = -1;
		s_why = why;
		LogMsg(std::string("StitchByteGuard: not armed (") + why
		       + "); an interior stitch still writes one byte past its NavInstance");
	}
	EmitHeartbeat();
	s_lastBeat = ElapsedSec();
}


// --- crash path ---

namespace stitch_byte_guard_detail {
typedef FlbExternal SbBuf;
} // namespace stitch_byte_guard_detail
using namespace stitch_byte_guard_detail;

size_t StitchByteGuardCrashFormat(char* buf, size_t cap, int maxEntries)
{
	if (!buf || cap == 0)
		return 0;
	SbBuf o;
	o.b = buf;
	o.cap = cap;
	o.n = 0;

	FlbStr(&o, "  StitchByte: armed=");
	FlbDec(&o, s_state == 1 ? 1 : 0);
	FlbStr(&o, " mode=");       FlbStr(&o, s_actMode ? "guard" : "observe");
	FlbStr(&o, " calls=");      FlbDec(&o, Read(&s_calls));
	FlbStr(&o, " interior=");   FlbDec(&o, Read(&s_interior));
	FlbStr(&o, " skipped=");    FlbDec(&o, Read(&s_skipped));
	FlbStr(&o, " kept=");       FlbDec(&o, Read(&s_kept));
	FlbStr(&o, " nonzero=");    FlbDec(&o, Read(&s_nonzero));

	LARGE_INTEGER nowQpc;
	QueryPerformanceCounter(&nowQpc);

	int shown = 0;
	for (LONG n = Read(&s_next); n > 0 && shown < maxEntries; --n)
	{
		const StitchByteEntry& e = s_ring[(n - 1) % kRingSize];
		if (Read((volatile LONG*)&e.seq) != n)
			continue;
		++shown;
		FlbStr(&o, "\r\n  SBYTE #"); FlbDec(&o, n);
		FlbStr(&o, " uid=");    FlbHex(&o, e.uid);
		FlbStr(&o, " out=");    FlbHex(&o, e.output);
		FlbStr(&o, " prior=");
		if (e.readable) FlbHex(&o, e.prior); else FlbStr(&o, "unreadable");
		unsigned __int64 cur = 0;
		FlbStr(&o, " now=");
		if (SafeReadQword(e.output + kStitchByteWriteOffset, &cur)) FlbHex(&o, cur);
		else FlbStr(&o, "unreadable");
		FlbStr(&o, e.skipped ? " skipped" : " kept");
		if (s_qpcFreq > 0)
		{
			FlbStr(&o, " ageMs=");
			FlbDec(&o, (__int64)((nowQpc.QuadPart - (__int64)e.qpc) * 1000 / s_qpcFreq));
		}
		FlbStr(&o, " tid="); FlbDec(&o, (__int64)e.tid);
	}
	FlbStr(&o, "\r\n");
	if (o.n < o.cap)
		o.b[o.n] = '\0';
	return o.n;
}


void NeutralizeStitchByteGuard()
{
	if (!s_page || !s_codePage)
		return;
	InterlockedExchange64((volatile LONGLONG*)s_page,
		(LONGLONG)(unsigned __int64)(uintptr_t)(s_codePage + kKeepAllOffset));
	s_page = NULL;
}
