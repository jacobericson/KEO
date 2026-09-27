// Host coverage for the memory figures and the fatal-event classification,
// including a real throw put through the same capture the vectored handler
// uses, so the pieces are exercised rather than assumed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "diag/mem_format.h"
#include "diag/mem_probe.h"
#include "diag/fatal_class.h"
#include "diag/cpp_exception.h"

// The two symbols the probe files expect the DLL to provide.
__declspec(thread) int g_inOurGuard = 0;

static std::vector<std::string> g_logged;
void LogMsg(const std::string& line) { g_logged.push_back(line); }

#include "check.h"

static bool Contains(const char* haystack, const char* needle)
{
	return strstr(haystack, needle) != 0;
}

namespace zoprobe { struct SampleFailure { int pad; }; struct OtherFailure { int pad; }; }

static const DWORD CPP_THROW_CODE = 0xE06D7363;

static LONG WINAPI ProbeVeh(PEXCEPTION_POINTERS info)
{
	if (info && info->ExceptionRecord
	    && info->ExceptionRecord->ExceptionCode == CPP_THROW_CODE)
		CppExceptionNote(info->ExceptionRecord);
	return EXCEPTION_CONTINUE_SEARCH;
}

// A real access violation, with the fault's own EXCEPTION_RECORD handed to
// the formatter. This is what the record cannot be checked by inspection: it
// proves the OS populates ExceptionInformation for an AV, that the two slots
// are ordered [0]=access type and [1]=address, and that the formatter reads
// the address the test itself chose. Kept in its own function, free of C++
// objects, so __try coexists with /EHsc.
static volatile int  g_avSink = 0;
static DWORD         g_avCode = 0;
static DWORD         g_avParams = 0;
static ULONG_PTR     g_avInfo0 = 0;
static ULONG_PTR     g_avInfo1 = 0;

static int CaptureFault(PEXCEPTION_POINTERS info)
{
	g_avCode = info->ExceptionRecord->ExceptionCode;
	g_avParams = info->ExceptionRecord->NumberParameters;
	g_avInfo0 = info->ExceptionRecord->ExceptionInformation[0];
	g_avInfo1 = info->ExceptionRecord->ExceptionInformation[1];
	return EXCEPTION_EXECUTE_HANDLER;
}

// `write` selects a store rather than a load, so rw= is exercised against a
// fault whose access type the test controls.
static bool InjectAccessViolation(ULONG_PTR target, bool write)
{
	g_avCode = 0; g_avParams = 0; g_avInfo0 = ~(ULONG_PTR)0; g_avInfo1 = ~(ULONG_PTR)0;
	__try
	{
		if (write)
			*(volatile int*)target = 1;
		else
			g_avSink = *(volatile int*)target;
		return false;
	}
	__except (CaptureFault(GetExceptionInformation()))
	{
		return true;
	}
}

int main()
{
	// --- byte formatting ---------------------------------------------------
	Check(MemBytesToMB(0) == 0, "zero bytes is zero MB");
	Check(MemBytesToMB(1024ULL * 1024ULL) == 1, "one MB");
	Check(MemBytesToMB(11867670938ULL) == 11318, "11.05 GB rounds to 11318 MB");
	Check(MemBytesToMB(1572864ULL) == 2, "1.5 MB rounds up, not down to 1");

	MemFigures f;
	MemFiguresClear(&f);
	f.privateCommit = 11867670938ULL;
	f.peakPrivate   = 12058624000ULL;
	f.workingSet    = 9479372800ULL;
	f.haveProcess   = 1;
	f.pagefileFree  = 1992294400ULL;
	f.pagefileTotal = 77201260544ULL;
	f.memLoadPct    = 88;
	f.haveSystem    = 1;

	char buf[256];
	MemFormatLong(buf, sizeof(buf), f);
	Check(Contains(buf, "privMB=11318"), "the long form carries private commit");
	Check(Contains(buf, "wsMB=9040"), "the long form carries the working set");
	Check(Contains(buf, "sysCommitFreeMB=1900"), "the long form carries free system commit");
	Check(Contains(buf, "sysLoad=88%"), "the long form carries the system load");

	MemFormatShort(buf, sizeof(buf), f);
	Check(Contains(buf, "priv11318MB"), "the short form carries private commit");
	Check(Contains(buf, "free1900MB"), "the short form carries free system commit");

	// A reading that failed must say so rather than read as an empty process.
	MemFigures missing;
	MemFiguresClear(&missing);
	MemFormatLong(buf, sizeof(buf), missing);
	Check(Contains(buf, "privMB=-"), "an unread process figure prints as -, not 0");
	Check(Contains(buf, "sysLoad=-"), "an unread system figure prints as -, not 0");

	// A short buffer truncates and still terminates.
	char tiny[8];
	MemFormatLong(tiny, sizeof(tiny), f);
	Check(strlen(tiny) < sizeof(tiny), "the long form never overruns its buffer");

	// --- fatal classification ---------------------------------------------
	Check(FatalKindForCode(0xC0000005UL) == FATAL_KIND_AV, "AV code");
	Check(FatalKindForCode(0xC0000094UL) == FATAL_KIND_DIV0, "divide by zero code");
	Check(FatalKindForCode(0xC00000FDUL) == FATAL_KIND_STACK, "stack overflow code");
	Check(FatalKindForCode(0xE06D7363UL) == FATAL_KIND_CPPEX, "C++ throw code");
	Check(FatalKindForCode(0xC0000409UL) == FATAL_KIND_FASTFAIL, "fail-fast code");
	// 0xC0000417 (the CRT's invalid-parameter abort) joins the same
	// CRT-abort family as fail-fast/heap-corruption/app-exit. PROD's own
	// unhandled filter (crash_record.cpp's ZoneOptUnhandledFilter, registered via
	// SetUnhandledExceptionFilter in every build) calls FatalKindForCode for
	// every code that reaches it, not only the ones the VEH itself records --
	// so a *raised* 0xC0000417 that goes unhandled all the way to that filter
	// is PROD-observable too: its crash_dump.txt record now reads kind=ABORT
	// instead of kind=OTHER. Harmless (ABORT already covers fail-fast et al.),
	// just not a no-op.
	Check(FatalKindForCode(0xC0000417UL) == FATAL_KIND_FASTFAIL, "invalid-parameter code");
	Check(FatalKindForCode(0x12345678UL) == FATAL_KIND_OTHER, "an unknown code is OTHER");

	Check(strcmp(FatalKindToken(FATAL_KIND_AV), FatalKindToken(FATAL_KIND_CPPEX)) != 0,
	      "an AV and a C++ throw never print the same token");
	Check(strcmp(FatalKindToken(FATAL_KIND_CPPEX), "CPPEX") == 0, "the C++ throw token");

	char name[128];
	FatalDemangleTypeName(".?AVRenderingAPIException@Ogre@@", name, sizeof(name));
	Check(strcmp(name, "Ogre::RenderingAPIException") == 0, "a scoped type reads outermost first");
	FatalDemangleTypeName(".?AVFoo@@", name, sizeof(name));
	Check(strcmp(name, "Foo") == 0, "an unscoped type");
	FatalDemangleTypeName("not a descriptor", name, sizeof(name));
	Check(strcmp(name, "not a descriptor") == 0, "anything unparsable comes through verbatim");

	// --- access= / rw= -----------------------------------------------------
	char acc[64];
	FatalFormatAccess(0xC0000005UL, 2, 0, 0x00000007408687BCULL, acc, sizeof(acc));
	Check(strcmp(acc, "access=0x00000007408687BC rw=0/read") == 0,
	      "a read AV carries the accessed address, zero-padded to 16 digits");

	FatalFormatAccess(0xC0000005UL, 2, 1, 0, acc, sizeof(acc));
	Check(strcmp(acc, "access=0x0000000000000000 rw=1/write") == 0,
	      "a write to NULL prints access=0, which is a reading, not an absence");

	FatalFormatAccess(0xC0000005UL, 2, 8, 0xFFFF0001ULL, acc, sizeof(acc));
	Check(Contains(acc, "rw=8/exec"), "an execute fault names itself");

	FatalFormatAccess(0xC0000005UL, 2, 3, 0x10ULL, acc, sizeof(acc));
	Check(strcmp(acc, "access=0x0000000000000010 rw=3") == 0,
	      "an access type outside 0/1/8 survives as its raw number");

	FatalFormatAccess(0xC0000094UL, 2, 0, 0, acc, sizeof(acc));
	Check(strcmp(acc, "access=- rw=-") == 0,
	      "a divide by zero has no accessed address, and says so rather than printing 0");

	FatalFormatAccess(0xC0000005UL, 1, 0, 0, acc, sizeof(acc));
	Check(strcmp(acc, "access=- rw=-") == 0,
	      "an AV declaring one parameter is absent, not zero");

	FatalFormatAccess(0xC0000006UL, 2, 0, 0x2000ULL, acc, sizeof(acc));
	Check(Contains(acc, "access=0x0000000000002000"),
	      "an in-page error carries the same pair");

	{
		// Short buffers must truncate and still NUL-terminate.
		char tiny[8];
		size_t n = FatalFormatAccess(0xC0000005UL, 2, 0, 0x1234ULL, tiny, sizeof(tiny));
		Check(n < sizeof(tiny) && tiny[n] == '\0', "never writes past a tiny cap");
		Check(FatalFormatAccess(0xC0000005UL, 2, 0, 0, acc, 0) == 0, "a zero cap writes nothing");
	}

	// --- injection: a real fault, through the real exception record --------
	Check(InjectAccessViolation(0, false), "the NULL read faulted");
	FatalFormatAccess(g_avCode, g_avParams, g_avInfo0, g_avInfo1, acc, sizeof(acc));
	Check(g_avCode == 0xC0000005UL && g_avParams >= 2,
	      "the OS record declares an AV with its two parameters");
	Check(strcmp(acc, "access=0x0000000000000000 rw=0/read") == 0,
	      "a real NULL read formats as access=0 rw=0/read");

	Check(InjectAccessViolation((ULONG_PTR)0x7408687BC, true), "the wild write faulted");
	FatalFormatAccess(g_avCode, g_avParams, g_avInfo0, g_avInfo1, acc, sizeof(acc));
	Check(strcmp(acc, "access=0x00000007408687BC rw=1/write") == 0,
	      "a real wild-pointer write reports the address the test chose, not the fault site");

	// --- record admission --------------------------------------------------
	Check(CppRecordAdmit(0, 0.0, -1.0), "the first throw is always recorded");
	Check(CppRecordAdmit(CPP_RECORD_BURST - 1, 0.1, 0.05), "the opening burst ignores spacing");
	Check(!CppRecordAdmit(CPP_RECORD_BURST, 100.0, 99.5), "a throw inside the spacing is refused");
	Check(CppRecordAdmit(CPP_RECORD_BURST, 100.0, 90.0), "a throw after the spacing is admitted");
	Check(!CppRecordAdmit(CPP_RECORD_CAP, 1e9, 0.0), "the cap refuses even a long-idle throw");
	Check(CppRecordAdmit(CPP_RECORD_CAP - 1, 1e9, 0.0),
	      "a late throw is still recorded below the cap -- the fatal one arrives last");

	// --- a real throw, through the real capture ----------------------------
	PVOID veh = AddVectoredExceptionHandler(1, ProbeVeh);
	Check(veh != 0, "the probe handler registered");
	long before = CppExceptionCount();
	try { throw zoprobe::SampleFailure(); }
	catch (const zoprobe::SampleFailure&) { }
	Check(CppExceptionCount() == before + 1, "the throw was counted");

	CppExceptionLastType(name, sizeof(name));
	Check(strcmp(name, "zoprobe::SampleFailure") == 0,
	      "the thrown type's name was walked out of the exception record");
	Check(CppExceptionLastAddress() != 0, "the throw site was recorded");

	// The walk is memoised per throw site, so a repeat must still count and a
	// different type must still displace the remembered name.
	try { throw zoprobe::SampleFailure(); }
	catch (const zoprobe::SampleFailure&) { }
	Check(CppExceptionCount() == before + 2, "a repeated throw still counts");
	try { throw zoprobe::OtherFailure(); }
	catch (const zoprobe::OtherFailure&) { }
	CppExceptionLastType(name, sizeof(name));
	Check(strcmp(name, "zoprobe::OtherFailure") == 0,
	      "a different type is walked, not served from the memo");
	try { throw zoprobe::SampleFailure(); }
	catch (const zoprobe::SampleFailure&) { }
	CppExceptionLastType(name, sizeof(name));
	Check(strcmp(name, "zoprobe::SampleFailure") == 0, "and the first type comes back");
	before = CppExceptionCount() - 1;

	char token[192];
	CppExceptionToken(token, sizeof(token));
	Check(Contains(token, "cppEx="), "the token names itself");
	Check(Contains(token, "zoprobe::SampleFailure"), "the token carries the last type");

	// A malformed record must be survived, not faulted on.
	EXCEPTION_RECORD bogus;
	memset(&bogus, 0, sizeof(bogus));
	bogus.ExceptionCode = CPP_THROW_CODE;
	bogus.NumberParameters = 4;
	bogus.ExceptionInformation[0] = 0x19930520;
	bogus.ExceptionInformation[2] = (ULONG_PTR)0x10;   // unreadable ThrowInfo
	bogus.ExceptionInformation[3] = (ULONG_PTR)0x1000;
	CppExceptionNote(&bogus);
	Check(CppExceptionCount() == before + 2, "a malformed throw still counts");
	CppExceptionLastType(name, sizeof(name));
	Check(strcmp(name, "zoprobe::SampleFailure") == 0,
	      "a malformed throw leaves the last good name alone");
	Check(g_inOurGuard == 0, "the guarded reads balanced their counter");

	long slot1 = CppExceptionClaimRecordSlot(0.0);
	Check(slot1 == 1, "the first record slot is 1");
	for (long i = 1; i < CPP_RECORD_BURST; ++i)
		CppExceptionClaimRecordSlot(0.0);
	Check(CppExceptionClaimRecordSlot(0.0) == 0, "a throw inside the spacing gets no slot");
	Check(CppExceptionClaimRecordSlot(CPP_RECORD_SPACING_SEC * 2) == CPP_RECORD_BURST + 1,
	      "a throw past the spacing gets the next slot");

	RemoveVectoredExceptionHandler(veh);

	// --- the live probe and the periodic line ------------------------------
	MemProbeInit();
	MemFigures live;
	Check(MemProbeRead(&live), "the live read succeeded");
	Check(live.haveProcess && live.privateCommit > 0, "the process commit charge is a real number");
	Check(live.haveSystem && live.pagefileTotal > 0, "the system commit limit is a real number");

	MemFigures last;
	double age = -1.0;
	Check(MemProbeLastSample(&last, &age, 5.0), "init published a sample");

	g_logged.clear();
	LogMemoryStats(1000.0);
	Check(g_logged.size() == 1, "the periodic line was emitted");
	if (!g_logged.empty())
	{
		const std::string& line = g_logged[0];
		Check(line.find("mem: ") == 0, "the line is greppable by its own marker");
		Check(line.find("privMB=") != std::string::npos, "the line carries private commit");
		Check(line.find("sysCommitFreeMB=") != std::string::npos,
		      "the line carries free system commit");
		Check(line.find("privMB=-") == std::string::npos,
		      "the live figures reached the line, not the unread placeholder");
		Check(line.find("cppEx=") != std::string::npos, "the line carries the throw counter");
	}
	LogMemoryStats(1001.0);
	Check(g_logged.size() == 1, "a second call inside the interval prints nothing");
	LogMemoryStats(1060.0);
	Check(g_logged.size() == 2, "a call past the interval prints again");

	return CheckExit("mem_diag_units");
}
