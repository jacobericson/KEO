#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/physx/physx_query_guard.h"
#include "fixes/physx/physx_query_guard_policy.h"
#include "fixes/physx/purecall_layout.h"
#include "fixes/near_page.h"
#include "game/game.h"
#include "base/core.h"
#include "base/config.h"
#include "diag/module_bases.h"
#include <windows.h>
#include <string>
#include <sstream>

namespace physx_query_guard_detail
{

// Resolved once, before the site is patched, and read-only afterwards. The
// per-entry path never calls the loader and never takes a lock: every test it
// makes is an integer compare against these.
volatile unsigned __int64 g_physBase = 0;
volatile unsigned __int64 g_physEnd  = 0;
volatile unsigned __int64 g_purecall = 0;   // 0 = unconfirmed, pure test off

// Per-entry counters. One per rejection class, because a bare skip count
// cannot separate a use-after-free from an unknown-but-legitimate shape.
volatile LONG s_entries     = 0;
volatile LONG s_ok          = 0;
volatile LONG s_rejAddr     = 0;
volatile LONG s_rejUnread   = 0;
volatile LONG s_rejPure     = 0;
volatile LONG s_rejFVptr    = 0;
volatile LONG s_rejFSlot    = 0;

// The first few foreign rejections carry their vptr out to the main thread,
// which is the only place a module name can be resolved. A fixed ring, no
// lock: a producer claims a slot with one InterlockedIncrement and publishes
// it with one store; the main thread only ever reads published slots.
const int kForeignReports = 8;
volatile LONG s_foreignClaim = 0;
volatile unsigned __int64 s_foreignVptr[kForeignReports] = { 0 };
volatile LONG s_foreignReady[kForeignReports] = { 0 };
int s_foreignDrained = 0;   // main thread only

// Install state (main thread only).
enum GuardState { kOff = 0, kPending, kArmed, kGaveUp };
int    s_state       = kOff;
int    s_attempts    = 0;
double s_firstSec    = -1.0;
double s_lastSec     = -1.0;
double s_nextBeat    = 0.0;
const double kBeatSeconds = 60.0;
std::string s_why;          // why the guard is not armed, for the stats line

// The stub's page. Allocated once, never freed and never made writable
// again: the game's own code jumps into it, and the callee of the re-issued
// virtual call returns into it, so a thread can be inside it at any moment
// with no way to prove otherwise.
unsigned char* s_page     = NULL;   // data page (RW): the gate slot
unsigned char* s_codePage = NULL;   // code page (RX): stub + accept-all thunk
const size_t kDataPageSize = 0x1000;
const size_t kCodePageSize = 0x1000;
const size_t kAcceptAllOffset = 0x40;


// --- SEH-guarded reads. Standalone and POD-only: MSVC 2010 rejects __try in
// a function that also holds an object needing unwinding. ---

bool SafeReadQword(unsigned __int64 addr, unsigned __int64* out)
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

bool SafeReadBytes(const void* addr, void* out, size_t n)
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

bool ResolveModuleSize(unsigned __int64 base, unsigned __int64* outSize)
{
	if (!base)
		return false;
	bool ok = true;
	GuardEnter();
	__try
	{
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			ok = false;
		}
		else
		{
			const IMAGE_NT_HEADERS64* nt =
				(const IMAGE_NT_HEADERS64*)((uintptr_t)base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				ok = false;
			else
				*outSize = nt->OptionalHeader.SizeOfImage;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}


void CountClass(PhysQueryClass cls)
{
	switch (cls)
	{
	case PHYSQ_OK:                 InterlockedIncrement(&s_ok);        break;
	case PHYSQ_REJ_ADDR:           InterlockedIncrement(&s_rejAddr);   break;
	case PHYSQ_REJ_UNREAD:         InterlockedIncrement(&s_rejUnread); break;
	case PHYSQ_REJ_PURE:           InterlockedIncrement(&s_rejPure);   break;
	case PHYSQ_REJ_FOREIGN_VPTR:   InterlockedIncrement(&s_rejFVptr);  break;
	case PHYSQ_REJ_FOREIGN_SLOT:   InterlockedIncrement(&s_rejFSlot);  break;
	}
}

// A foreign rejection is the only kind that could have been a live shape, so
// the first few carry their vptr out for module attribution. Claim-and-
// publish only; no formatting and no loader call on this path.
void ReportForeign(unsigned __int64 vptr)
{
	LONG slot = InterlockedIncrement(&s_foreignClaim) - 1;
	if (slot < 0 || slot >= kForeignReports)
		return;
	s_foreignVptr[slot] = vptr;
	InterlockedExchange(&s_foreignReady[slot], 1);
}

PhysQueryClass EvaluateEntry(unsigned __int64 p, unsigned __int64* outVptr)
{
	unsigned __int64 vptr = 0, slot1 = 0;
	bool vptrRead = false, slot1Read = false;

	if (PhysQueryAddressPlausible(p))
	{
		vptrRead = SafeReadQword(p, &vptr);
		if (vptrRead)
			slot1Read = SafeReadQword(vptr + 8, &slot1);
	}

	*outVptr = vptr;
	return ClassifyPhysQueryEntry(p, vptrRead, vptr, slot1Read, slot1,
		g_physBase, g_physEnd, g_purecall);
}

} // namespace
using namespace physx_query_guard_detail;


// Called from the stub, once per result entry, on whichever thread ran the
// query (the AI back thread and the main thread both reach it). No
// allocation, no lock, no logging: counters are Interlocked and a report is a
// claimed slot the main thread drains.
extern "C" int __fastcall ZoneOpt_PhysQEntryOk(const void* shape);

extern "C" int __fastcall ZoneOpt_PhysQEntryOk(const void* shape)
{
	InterlockedIncrement(&s_entries);

	unsigned __int64 vptr = 0;
	PhysQueryClass cls = EvaluateEntry((unsigned __int64)(uintptr_t)shape, &vptr);
	CountClass(cls);

	if (PhysQueryClassIsForeign(cls))
		ReportForeign(vptr);

	return cls == PHYSQ_OK ? 1 : 0;
}


namespace physx_query_guard_detail
{

// DEV only: runs the classifier on three constructed vptr values before the
// site is patched, so a build that cannot tell the abstract vtable from a
// concrete one says so in the log instead of being discovered in a fight.
// Informational: it never refuses to arm.
#ifdef ZONEOPT_DEBUG
const unsigned __int64 kAbstractShapeVtableRva = 0x3B3F90;
const unsigned __int64 kConcreteShapeVtableRva = 0x3B40D0;

const char* ClassName(PhysQueryClass cls)
{
	switch (cls)
	{
	case PHYSQ_OK:               return "ok";
	case PHYSQ_REJ_ADDR:         return "addr";
	case PHYSQ_REJ_UNREAD:       return "unread";
	case PHYSQ_REJ_PURE:         return "pure";
	case PHYSQ_REJ_FOREIGN_VPTR: return "fvptr";
	case PHYSQ_REJ_FOREIGN_SLOT: return "fslot";
	}
	return "?";
}

void SelfTestClassifier()
{
	unsigned __int64 fakes[3];
	fakes[0] = g_physBase + kAbstractShapeVtableRva;  // expect pure
	fakes[1] = g_physBase + kConcreteShapeVtableRva;  // expect ok
	fakes[2] = (unsigned __int64)gameBase + 0x7857A0; // expect fvptr

	std::ostringstream ss;
	ss << "PhysQ selftest:";
	static const char* kWant[3] = { "pure", "ok", "fvptr" };
	for (int i = 0; i < 3; ++i)
	{
		unsigned __int64 seen = 0;
		PhysQueryClass cls = EvaluateEntry((unsigned __int64)(uintptr_t)&fakes[i], &seen);
		ss << " [" << i << "] want=" << kWant[i] << " got=" << ClassName(cls);
		if (PhysQueryClassIsForeign(cls))
		{
			// The same attribution a real foreign rejection gets, so the
			// module lookup is exercised before it is ever needed in anger.
			int count = 0;
			const ModuleBaseEntry* mods = CapturedModuleBases(&count);
			unsigned __int64 off = 0;
			int idx = ResolveModuleForAddress(mods, count, seen, &off);
			ss << " mod=";
			if (idx >= 0)
				ss << mods[idx].name << "+0x" << std::hex << off << std::dec;
			else
				ss << "?";
		}
	}
	LogMsg(ss.str());
}
#endif

bool TryArmPhysQueryGuard()
{
	unsigned __int64 physBase = (unsigned __int64)(uintptr_t)GetModuleHandleA("PhysXCore64.dll");
	if (!physBase)
		return false;   // not loaded yet; retried, nothing logged

	unsigned __int64 physSize = 0;
	if (!ResolveModuleSize(physBase, &physSize))
	{
		s_why = "PhysXCore64.dll image header unreadable";
		return true;
	}

	// Exact, fail-closed byte checks on all three exe addresses: the site and
	// both of the stub's return targets.
	unsigned char block[kPhysQuerySiteBlockLen];
	unsigned char resume[4];
	unsigned char skip[6];
	unsigned __int64 siteAddr   = (unsigned __int64)gameBase + kPhysQuerySiteRva;
	unsigned __int64 resumeAddr = (unsigned __int64)gameBase + kPhysQueryResumeRva;
	unsigned __int64 skipAddr   = (unsigned __int64)gameBase + kPhysQuerySkipRva;

	if (!SafeReadBytes((const void*)(uintptr_t)(siteAddr - kPhysQuerySitePrefixLen),
	                   block, sizeof(block))
	 || !SafeReadBytes((const void*)(uintptr_t)resumeAddr, resume, sizeof(resume))
	 || !SafeReadBytes((const void*)(uintptr_t)skipAddr, skip, sizeof(skip)))
	{
		s_why = "the site or a return target is unreadable";
		return true;
	}
	if (!PhysQueryBytesMatch(block, kPhysQuerySiteBlock, kPhysQuerySiteBlockLen))
	{
		s_why = "site bytes differ (mid-function: an already-detoured site is "
		        "an unknown binary, not a shared site)";
		return true;
	}
	if (!PhysQueryBytesMatch(resume, kPhysQueryResumeBytes, kPhysQueryResumeLen)
	 || !PhysQueryBytesMatch(skip, kPhysQuerySkipBytes, kPhysQuerySkipLen))
	{
		s_why = "a return target's bytes differ";
		return true;
	}

	// _purecall's own address, shared with the forensic recorder. If its
	// signature does not match this PhysX build the pure test is left off and
	// the range tests carry the guard alone.
	unsigned __int64 purecall = 0;
	unsigned char sig[16];
	if (SafeReadBytes((const void*)(uintptr_t)(physBase + kPurecallRva), sig, sizeof(sig))
	    && PurecallSignatureMatches(sig, kPurecallSignatureLen))
		purecall = physBase + kPurecallRva;

	// A page rejected below (a displacement that does not reach) is released;
	// nothing frees it once the site is patched.
	unsigned char* page = AllocateNearPages((unsigned __int64)gameBase,
	                                        kDataPageSize + kCodePageSize);
	if (!page)
	{
		s_why = "no free page within rel32 reach of the exe";
		return true;
	}

	unsigned char* codePage = page + kDataPageSize;
	unsigned __int64 gateSlotAddr = (unsigned __int64)(uintptr_t)page;
	unsigned __int64 stubAddr     = (unsigned __int64)(uintptr_t)codePage;

	size_t stubLen = 0;
	unsigned __int64 siteQword = 0;
	if (!BuildPhysQueryStub(codePage, kCodePageSize, stubAddr, gateSlotAddr,
	                        resumeAddr, skipAddr, &stubLen)
	 || !BuildPhysQueryAcceptAll(codePage + kAcceptAllOffset,
	                             kCodePageSize - kAcceptAllOffset)
	 || !BuildPhysQuerySiteQword(block, siteAddr, stubAddr, &siteQword))
	{
		VirtualFree(page, 0, MEM_RELEASE);   // never armed: nothing can be in it
		s_why = "the stub's displacements do not reach from the page found";
		return true;
	}

	*(volatile unsigned __int64*)page = (unsigned __int64)(uintptr_t)&ZoneOpt_PhysQEntryOk;

	DWORD oldProtect = 0;
	if (!VirtualProtect(codePage, kCodePageSize, PAGE_EXECUTE_READ, &oldProtect))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		s_why = "the stub page could not be made executable";
		return true;
	}
	FlushInstructionCache(GetCurrentProcess(), codePage, kCodePageSize);

	// Every value the per-entry path reads is published before the site can
	// call it, so a query already running on another thread never sees the
	// stub with an empty module range behind it.
	g_physBase = physBase;
	g_physEnd  = physBase + physSize;
	g_purecall = purecall;
	s_page     = page;
	s_codePage = codePage;

#ifdef ZONEOPT_DEBUG
	SelfTestClassifier();
#endif

	// One aligned 8-byte store over an 8-byte-aligned block: a thread already
	// walking results sees either both original instructions or the whole
	// jump, never a half-written mixture.
	DWORD oldSite = 0;
	void* blockAddr = (void*)(uintptr_t)(siteAddr - kPhysQuerySitePrefixLen);
	if (!VirtualProtect(blockAddr, 8, PAGE_EXECUTE_READWRITE, &oldSite))
	{
		s_why = "VirtualProtect on the site failed";
		return true;   // the page stays; nothing points at it
	}
	InterlockedExchange64((volatile LONGLONG*)blockAddr, (LONGLONG)siteQword);
	DWORD ignore = 0;
	VirtualProtect(blockAddr, 8, oldSite, &ignore);
	FlushInstructionCache(GetCurrentProcess(), blockAddr, 8);

	s_state = kArmed;
	s_why.clear();

	std::ostringstream msg;
	msg << "PhysQ: armed at exe+0x" << std::hex << kPhysQuerySiteRva
	    << " stub=0x" << stubAddr << " physx=0x" << physBase
	    << "+0x" << physSize
	    << " purecall=0x" << purecall << std::dec
	    << " (attempt " << s_attempts << ")";
	LogMsg(msg.str());
	return true;
}

void DrainForeignReports()
{
	while (s_foreignDrained < kForeignReports
	       && InterlockedCompareExchange(&s_foreignReady[s_foreignDrained], 0, 0) != 0)
	{
		unsigned __int64 vptr = s_foreignVptr[s_foreignDrained];
		++s_foreignDrained;

		int count = 0;
		const ModuleBaseEntry* mods = CapturedModuleBases(&count);
		unsigned __int64 off = 0;
		int idx = ResolveModuleForAddress(mods, count, vptr, &off);

		std::ostringstream ss;
		ss << "PhysQ foreign vptr=0x" << std::hex << vptr << std::dec << " mod=";
		if (idx >= 0)
			ss << mods[idx].name << "+0x" << std::hex << off << std::dec;
		else
			ss << "? (not in the startup module snapshot)";
		LogMsg(ss.str());
	}
}

} // namespace
using namespace physx_query_guard_detail;


void InstallPhysQueryGuard(bool enabled)
{
	if (!enabled)
	{
		s_state = kOff;
		s_why = "physQueryGuard=false";
		LogMsg("PhysQ: off (physQueryGuard=false)");
		return;
	}

	s_state = kPending;
	s_firstSec = ElapsedSec();
	s_lastSec = s_firstSec;
	++s_attempts;
	if (TryArmPhysQueryGuard())
	{
		if (s_state != kArmed)
		{
			s_state = kGaveUp;
			LogMsg("PhysQ: off (" + s_why + ")");
		}
		return;
	}

	s_why = "PhysXCore64.dll not loaded yet";
	LogMsg("PhysQ: deferred (PhysXCore64.dll not loaded at plugin init; "
	       "retrying on the main thread)");
}

void PhysQueryGuardTick(double now)
{
	if (s_state == kPending && PhysQueryRetryDue(now, s_lastSec))
	{
		s_lastSec = now;
		++s_attempts;
		if (TryArmPhysQueryGuard())
		{
			if (s_state != kArmed)
			{
				s_state = kGaveUp;
				LogMsg("PhysQ: off (" + s_why + ")");
			}
		}
		else if (PhysQueryRetryExpired(now, s_firstSec))
		{
			s_state = kGaveUp;
			s_why = "PhysXCore64.dll never loaded this session";
			LogMsg("PhysQ: off (" + s_why + ")");
		}
	}

	DrainForeignReports();

	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;

	// Unconditional, whatever the guard's state: "the guard never rejected
	// anything" and "the guard was never armed" have to read differently.
	long fvptr = InterlockedCompareExchange(&s_rejFVptr, 0, 0);
	long fslot = InterlockedCompareExchange(&s_rejFSlot, 0, 0);
	long addr  = InterlockedCompareExchange(&s_rejAddr, 0, 0);
	long unread = InterlockedCompareExchange(&s_rejUnread, 0, 0);
	long pure  = InterlockedCompareExchange(&s_rejPure, 0, 0);

	std::ostringstream ss;
	ss << "PhysQ: enabled=" << (physQueryGuardEnabled ? 1 : 0)
	   << " armed=" << (s_state == kArmed ? 1 : 0)
	   << " entries=" << InterlockedCompareExchange(&s_entries, 0, 0)
	   << " ok=" << InterlockedCompareExchange(&s_ok, 0, 0)
	   << " skip=" << (addr + unread + pure + fvptr + fslot)
	   << "(pure=" << pure << " fvptr=" << fvptr << " fslot=" << fslot
	   << " unread=" << unread << " addr=" << addr << ")";
	if (s_state != kArmed && !s_why.empty())
		ss << " why=" << s_why;
	LogMsg(ss.str());
}

void NeutralizePhysQueryGuard()
{
	if (!s_page || !s_codePage)
		return;

	// One aligned store into the data page, which is still PAGE_READWRITE:
	// after it, the patched path calls the accept-all thunk in the stub's own
	// page and never touches this DLL again. The six patched bytes stay as
	// they are -- a thread can be inside the stub, and the callee of the
	// re-issued virtual call returns into it, so there is no moment at which
	// restoring them is known to be safe.
	InterlockedExchange64((volatile LONGLONG*)s_page,
		(LONGLONG)(unsigned __int64)(uintptr_t)(s_codePage + kAcceptAllOffset));
	s_page = NULL;
}
