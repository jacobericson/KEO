// The crash recorder: the vectored handler, the unhandled filter, the record writers, the module
// snapshot and the crash-dump claim. Every function on the fault path uses no CRT, no heap and no lock.

#include <cstdio>     // _snprintf_s (module-base log line)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h> // module snapshot for the crash record's module-base table
#include "base/core.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "navmesh/cache/nm_cache_core.h"
#include "navmesh/jobs/nm_buildlock.h"
#include "fixes/crash_claim.h"
#include "diag/mem_probe.h"
#include "diag/fatal_class.h"
#include "diag/cpp_exception.h"
#include "diag/throw_ring.h"
#include "diag/module_bases.h"
#include "diag/exit_capture.h"
#include "fixes/stitch/stitch_byte_guard.h"
#include "fixes/streaming/section_key_ring.h"
#include "plugin/crash_record.h"

// =========================================================================
// Crash handler — record-only
// =========================================================================
//
// A vectored handler runs in the faulting thread's context, before any frame
// unwinds, so everything it touches must be safe in that context. It therefore
// uses no CRT, no heap, no lock and no logging: the text is built with the
// small formatter below into a stack buffer and written with CreateFileA /
// WriteFile. The crash file's path is resolved once in startPlugin, so the
// handler never calls GetModuleFileNameA (loader lock) while faulting.
//
// It always returns EXCEPTION_CONTINUE_SEARCH: the record is a diagnostic, and
// RE_Kenshi's own handler still produces the minidump players send us.
//
// Faults inside the mod's own __try blocks never reach here — see
// GuardEnter/GuardLeave in core.h.

char   g_crashFilePath[MAX_PATH] = { 0 };
char   g_cppExFilePath[MAX_PATH] = { 0 };
static volatile LONG g_crashSeq = 0;

// --- crash-context formatter: fixed buffer, no CRT, no allocation ---

// Storage belongs to the caller, so the frame a record is built on is the
// caller's choice. A stack overflow is recorded on the stack that overflowed,
// and a second fault there would cost the minidump.
namespace crash_record_detail {
typedef FlbExternal CrashBuf;
} // namespace crash_record_detail
using namespace crash_record_detail;

static void cbReg(CrashBuf* o, const char* name, unsigned __int64 v)
{
	FlbStr(o, name);
	FlbStr(o, "=0x");
	FlbHexDigits(o, v, 16);
	FlbChar(o, ' ');
}

static volatile LONG g_crashSkipProfiler = 0;

LONG CrashSkipProfilerCount()
{
	return InterlockedCompareExchange(&g_crashSkipProfiler, 0, 0);
}

// Records written to each of the mod's record files this process. The first
// one truncates its file, later ones append. Counted per file, so a C++ throw
// recorded in its own file never makes the next real fault look like a
// continuation of something else.
static volatile LONG g_crashRecordsWritten = 0;
static volatile LONG g_cppExRecordsWritten = 0;

// The fault the vectored handler last recorded, so the unhandled filter can
// tell the same event arriving a second time from a genuinely new one.
// Any vectored fault writer atomically updates address then code; any
// unhandled-filter reader tests those scalars to suppress a duplicate record.
// They are session state with no reset or coherent pair publication, so
// mixed diagnostic identity can affect attribution under concurrent faults.
static volatile LONG64 g_lastRecordedAddr = 0;
static volatile LONG   g_lastRecordedCode = 0;

// Whether crash_dump.txt has anything in it yet this process. The DEV
// exit-capture instrument (diag/exit_capture.cpp) reads this so a
// termination that follows an *already diagnosed* AV, div-by-zero or stack
// overflow -- RE_Kenshi's own dialog and minidump, after the user dismisses
// it -- doesn't also get a redundant EXIT record: the symptom that
// instrument exists for is a session that never wrote crash_dump.txt at all.
bool ExitCaptureAnyCrashRecorded()
{
	return InterlockedCompareExchange(&g_crashRecordsWritten, 0, 0) != 0;
}

// Loaded modules, captured once at startPlugin (see SnapshotModuleBases).
// Reading this array at fault time touches no loader state and takes no
// lock: main SnapshotModuleBases fills it before handlers are registered;
// main attribution and any-thread fault/exit readers then see an immutable
// array. Never reset or republished, so no reader can see a torn update. A module the game loads after startup -- a codec, a
// late plugin -- never appears here, and this curated table also drops
// whatever SelectModuleBases had no room for; EmitCrashRecord's addrMod=
// resolves against the fuller g_rawModuleBases and a live VirtualQuery
// instead of this table, so those two gaps don't cost it.
ModuleBaseEntry g_moduleBases[kMaxModuleBases];
int             g_moduleBaseCount = 0;
// True only if the process had more modules than kMaxModuleBases could
// hold; printed once, at snapshot time, since the crash record's own
// modTrunc= counts a different thing (entries dropped by buffer size).

// Handed out so a caller elsewhere resolves an address against this snapshot
// instead of building a second module table (diag/module_bases.h).
const ModuleBaseEntry* CapturedModuleBases(int* outCount)
{
	if (outCount) *outCount = g_moduleBaseCount;
	return g_moduleBases;
}

// Headroom for the raw enumeration, ahead of SelectModuleBases's priority
// filter. A generous fixed bound, kept as its own array (rather than only
// the curated g_moduleBases) so resolving one address against it never
// depends on whether that address's module survived the cap.
static const int kRawModuleCap = 256;
// Same startup writer and fault/exit readers as the curated table: filled
// before handler registration, then immutable. Count/valid are plain startup
// publication, with no reset or concurrent update to tear.
static ModuleBaseEntry g_rawModuleBases[kRawModuleCap];
static int             g_rawModuleCount = 0;
// False only if CreateToolhelp32Snapshot itself failed at startup; a real,
// even empty, enumeration leaves this true.
static bool            g_rawModuleListValid = false;

const ModuleBaseEntry* FullModuleBases(int* outCount, bool* outValid)
{
	if (outCount) *outCount = g_rawModuleCount;
	if (outValid) *outValid = g_rawModuleListValid;
	return g_rawModuleBases;
}

// EnumProcessModules/GetModuleInformation take the loader lock, which a
// thread already inside the loader (or the loader itself, mid-fault) can
// deadlock on. CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) does not, but it
// is still only safe to call here, at startup, never from the crash path.
//
// A crash address outside kenshi_x64.exe can only be attributed against a
// module this table carries, and `rva=` cannot give an answer for one at all
// (see EmitCrashRecord). RE_Kenshi.dll and this DLL are the two the mod's own
// crash records are read against, so they are pinned into the table by base
// address before the ordinary fill runs, ahead of every other module the
// process happens to have loaded -- raising kMaxModuleBases would help only
// until the next process with more DLLs than that; pinning the two that
// matter does not depend on the count at all.
void SnapshotModuleBases()
{
	g_moduleBaseCount = 0;

	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&SnapshotModuleBases, &self);
	HMODULE reKenshi = GetModuleHandleA("RE_Kenshi.dll");

	unsigned __int64 priority[2];
	int priorityCount = 0;
	if (self)     priority[priorityCount++] = (unsigned __int64)(uintptr_t)self;
	if (reKenshi) priority[priorityCount++] = (unsigned __int64)(uintptr_t)reKenshi;

	ModuleBaseEntry* raw = g_rawModuleBases;
	int rawCount = 0;

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
		GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;

	int seen = 0;
	MODULEENTRY32 me;
	me.dwSize = sizeof(me);
	if (Module32First(snap, &me))
	{
		// Only a successful first entry means the enumeration actually ran;
		// an opened handle that yields nothing is the same "don't know" case
		// as a handle that never opened, not a real empty process.
		g_rawModuleListValid = true;
		do
		{
			++seen;
			if (rawCount >= kRawModuleCap)
				continue;
			ModuleBaseEntry& e = raw[rawCount];
			size_t i = 0;
			for (; me.szModule[i] && i + 1 < sizeof(e.name); ++i)
				e.name[i] = me.szModule[i];
			e.name[i] = '\0';
			e.base = (unsigned __int64)(uintptr_t)me.modBaseAddr;
			e.size = (unsigned __int64)me.modBaseSize;
			++rawCount;
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	g_rawModuleCount = rawCount;

	g_moduleBaseCount = SelectModuleBases(raw, rawCount, priority, priorityCount,
		g_moduleBases, kMaxModuleBases);

	if (seen > kMaxModuleBases)
	{
		char line[96];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"Module bases: snapshot saw %d modules, kept %d (kMaxModuleBases)",
			seen, kMaxModuleBases);
		LogMsg(line);
	}
}

// The one write into the crash file. The first record truncates it, later ones
// append. Throws are recorded elsewhere, so this file means what its name says.
static void WriteCrashRecordFile(const char* text, size_t len)
{
	LONG index = InterlockedIncrement(&g_crashRecordsWritten);
	HANDLE hFile = CreateFileA(g_crashFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		(index == 1) ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;
	if (index != 1)
		SetFilePointer(hFile, 0, NULL, FILE_END);
	DWORD bytes = 0;
	WriteFile(hFile, text, (DWORD)len, &bytes, NULL);
	CloseHandle(hFile);
}

// Builds one record and writes it. `kind` names what died, so no two
// causes of death merge in the log. `pExInfo->ContextRecord` may be absent on
// the unhandled path; everything else read here is a register, an interlocked
// counter or a syscall that copies into a stack buffer.
static void EmitCrashRecord(const char* kind, LONG seq, PEXCEPTION_POINTERS pExInfo)
{
	uintptr_t addr = (uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress;
	DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
	CONTEXT* c = pExInfo->ContextRecord;

	// Budget: the register and counter block runs to roughly 1 KB, the module
	// table below is formatted into its own 2 KB buffer and copied in whole,
	// and the stitch-byte and section-key rings add their own blocks. FlbChar
	// drops what does not fit, and what it would drop is the tail -- the
	// module table, crashTid, afterStop -- so the headroom is kept ahead of the
	// fields, not measured after them.
	char storage[6144];
	CrashBuf o;
	o.b = storage;
	o.cap = sizeof(storage);
	o.n = 0;

	FlbStr(&o, "CRASH #");
	FlbDecU(&o, (unsigned __int64)(unsigned long)seq);
	FlbStr(&o, ": kind=");
	FlbStr(&o, kind);
	FlbStr(&o, " code=0x");
	FlbHexDigits(&o, (unsigned __int64)code, 8);
	FlbStr(&o, " addr=0x");
	FlbHexDigits(&o, (unsigned __int64)addr, 16);
	FlbStr(&o, " rva=0x");
	FlbHexDigits(&o, (unsigned __int64)(addr - gameBase), 8);
	{
		// rva= is meaningless once addr falls outside the game executable.
		// Resolved against the full startup enumeration, not the curated
		// table below, so this never depends on whether the owning module
		// made that table's cap; VirtualQuery (a syscall into a stack
		// struct, no loader lock) covers a module that loaded after the
		// snapshot too, at least by address if not by name.
		int fullCount = 0;
		bool fullValid = false;
		const ModuleBaseEntry* full = FullModuleBases(&fullCount, &fullValid);

		MEMORY_BASIC_INFORMATION mbi;
		unsigned __int64 addrImageBase =
			(VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type == MEM_IMAGE)
			? (unsigned __int64)(uintptr_t)mbi.AllocationBase : 0;

		char addrMod[48];
		FormatAddrMod(addrMod, sizeof(addrMod), full, fullCount, fullValid,
			(unsigned __int64)addr, addrImageBase);
		FlbStr(&o, " addrMod=");
		FlbStr(&o, addrMod);
		if (c && (unsigned __int64)c->Rip != (unsigned __int64)addr)
		{
			unsigned __int64 ripImageBase =
				(VirtualQuery((LPCVOID)c->Rip, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type == MEM_IMAGE)
				? (unsigned __int64)(uintptr_t)mbi.AllocationBase : 0;
			char ripMod[48];
			FormatAddrMod(ripMod, sizeof(ripMod), full, fullCount, fullValid,
				(unsigned __int64)c->Rip, ripImageBase);
			FlbStr(&o, " ripMod=");
			FlbStr(&o, ripMod);
		}
	}
	{
		// The address the instruction tried to touch, which `addr` does not
		// give: `addr` is where the code was, this is what it reached for.
		// NULL, a freed object and a wild pointer are the same `addr` and
		// three different faults.
		const EXCEPTION_RECORD* er = pExInfo->ExceptionRecord;
		char access[64];
		FatalFormatAccess((unsigned long)code, (unsigned long)er->NumberParameters,
			(unsigned __int64)er->ExceptionInformation[0],
			(unsigned __int64)er->ExceptionInformation[1],
			access, sizeof(access));
		FlbChar(&o, ' ');
		FlbStr(&o, access);
	}
	if (c)
	{
		FlbStr(&o, "\r\n  ");
		cbReg(&o, "RIP", c->Rip);
		FlbStr(&o, "\r\n  ");
		cbReg(&o, "RAX", c->Rax); cbReg(&o, "RBX", c->Rbx);
		cbReg(&o, "RCX", c->Rcx); cbReg(&o, "RDX", c->Rdx);
		FlbStr(&o, "\r\n  ");
		cbReg(&o, "R8 ", c->R8);  cbReg(&o, "R9 ", c->R9);
		cbReg(&o, "R10", c->R10); cbReg(&o, "R11", c->R11);
		FlbStr(&o, "\r\n  ");
		// R12-R15: callee-saved, so a value here reflects the faulting frame's
		// own state rather than whatever the last call clobbered -- the same
		// reason RSP/RBP were already captured.
		cbReg(&o, "R12", c->R12); cbReg(&o, "R13", c->R13);
		cbReg(&o, "R14", c->R14); cbReg(&o, "R15", c->R15);
		FlbStr(&o, "\r\n  ");
		cbReg(&o, "RSP", c->Rsp); cbReg(&o, "RBP", c->Rbp);
		cbReg(&o, "RSI", c->Rsi); cbReg(&o, "RDI", c->Rdi);
	}
	FlbStr(&o, "\r\n  ");
	FlbStr(&o, "busy=");
	FlbDecU(&o, (unsigned __int64)(unsigned long)InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0));
	FlbStr(&o, " slabHook=");
	FlbDecU(&o, (unsigned __int64)(unsigned long)InterlockedCompareExchange(&navmesh::g_nmCache.g_slabAllocHits, 0, 0));
	FlbChar(&o, '/');
	FlbDecU(&o, (unsigned __int64)(unsigned long)InterlockedCompareExchange(&navmesh::g_nmCache.g_slabAllocWorkerHits, 0, 0));
	FlbChar(&o, ' ');
	// The build mutex a narrow builder scope is holding, if any. A fault here
	// leaves it held for the rest of the process, and every later non-blocking
	// acquisition refuses, so this line is what says whether the mod owned it.
	{
		LONG owner = InterlockedCompareExchange(&g_buildLockOwnerTid, 0, 0);
		FlbStr(&o, "blOwner=");
		if (owner)
		{
			FlbDecU(&o, (unsigned __int64)(unsigned long)owner);
			FlbChar(&o, ':');
			FlbDecU(&o, (unsigned __int64)(unsigned long)
				InterlockedCompareExchange(&g_buildLockOwnerState, 0, 0));
		}
		else
			FlbChar(&o, '-');
		FlbChar(&o, ' ');
	}
	// Memory, live. Both readings go through entry points resolved at startup
	// and copy into the stack buffers below: no allocation, no CRT stream, no
	// loader work. A failed live read falls back to the periodic sample, and
	// says how old that sample is so the figure is never mistaken for a fresh
	// one. The system commit figures are here because an allocation failure
	// cannot be attributed to this process without them.
	{
		MemFigures mf;
		bool live = MemProbeRead(&mf);
		double age = 0.0;
		bool haveLast = false;
		if (!live)
			haveLast = MemProbeLastSample(&mf, &age, ElapsedSec());
		char figures[160];
		MemFormatLong(figures, sizeof(figures), mf);
		FlbStr(&o, figures);
		FlbStr(&o, " memSrc=");
		if (live)
			FlbStr(&o, "live");
		else if (haveLast)
		{
			FlbStr(&o, "last+");
			FlbDecU(&o, (unsigned __int64)(age > 0.0 ? (long)age : 0));
			FlbChar(&o, 's');
		}
		else
			FlbStr(&o, "none");
		FlbStr(&o, "\r\n  ");
	}
	{
		char cppEx[160];
		CppExceptionToken(cppEx, sizeof(cppEx));
		FlbStr(&o, cppEx);
		FlbChar(&o, ' ');
	}
	FlbStr(&o, "crashTid=");
	FlbDecU(&o, (unsigned __int64)GetCurrentThreadId());
	FlbStr(&o, " bgTid=");
	FlbDecU(&o, (unsigned __int64)g_navMeshBgThreadId);
	// g_navMeshStopSeen is set by the NavMesh::stop hook when the game
	// begins tearing the navmesh system down; tag records written after that
	// point instead of suppressing them (they are still real crashes, just in
	// a context where the navmesh system is already going away).
	if (InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0)
		FlbStr(&o, " afterStop=1");
	// The faulting thread's navmesh worker phase, when it is a
	// worker (core.h). This thread's own TLS pointer, one volatile read.
	{
		volatile LONG* wp = t_navMeshWorkerPhase;
		if (wp)
		{
			FlbStr(&o, " wphase=w");
			FlbDecU(&o, (unsigned __int64)(unsigned int)t_navMeshWorkerId);
			FlbChar(&o, ':');
			FlbStr(&o, NavMeshWorkerPhaseName(*wp));
		}
	}
	FlbStr(&o, "\r\n  ");
	{
		// The table `rva=` cannot give once `addr` falls outside the game
		// executable -- Havok, PhysX, Ogre, D3D and RE_Kenshi.dll all live
		// here. Formatted into its own buffer first: FormatModuleBases
		// NUL-terminates, and FlbStr's copy stops at that NUL rather than
		// carrying a stray '\0' byte into the record.
		char modLine[2048];
		int modTrunc = 0;
		FormatModuleBases(modLine, sizeof(modLine), g_moduleBases, g_moduleBaseCount, &modTrunc);
		FlbStr(&o, modLine);
		if (modTrunc > 0)
		{
			FlbStr(&o, " modTrunc=");
			FlbDecU(&o, (unsigned __int64)(unsigned long)modTrunc);
		}
	}
	FlbStr(&o, "\r\n");

	{
		// The interior stitch stores just before this fault, and what each hit
		// or would have hit. Written whether or not any happened, so an empty
		// block reads as "none", not as a missing instrument.
		char sbyte[768];
		size_t sbyteLen = StitchByteGuardCrashFormat(sbyte, sizeof(sbyte), 4);
		if (sbyteLen > 0)
			FlbStr(&o, sbyte);
	}

#ifdef KEO_DEBUG
	{
		// The section-table lookup the navmesh step was about to make. Written
		// for every fault, not only the ones that already look like that
		// family: a block that appears only when something looked wrong cannot
		// say that nothing was wrong. Its own buffer, so a long module table
		// cannot push it out.
		char ring[768];
		size_t ringLen = SectionKeyRingFormat(ring, sizeof(ring), 1);
		if (ringLen > 0)
			FlbStr(&o, ring);
	}
#endif
	WriteCrashRecordFile(o.b, o.n);
}

// A stack overflow is recorded here instead: the filter runs on the stack that
// overflowed, so the record carries no register block, no memory reading and
// no borrowed buffers — a second fault would cost RE_Kenshi its minidump. The
// module-base table is left off for the same reason: formatting it needs a
// second local buffer on a stack that has none to spare, so this path has no
// module attribution and its `rva=` stays exe-relative only.
static void EmitMinimalRecord(const char* kind, LONG seq, PEXCEPTION_POINTERS pExInfo)
{
	uintptr_t addr = (uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress;
	DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

	char storage[192];
	CrashBuf o;
	o.b = storage;
	o.cap = sizeof(storage);
	o.n = 0;

	FlbStr(&o, "CRASH #");
	FlbDecU(&o, (unsigned __int64)(unsigned long)seq);
	FlbStr(&o, ": kind=");
	FlbStr(&o, kind);
	FlbStr(&o, " code=0x");
	FlbHexDigits(&o, (unsigned __int64)code, 8);
	FlbStr(&o, " addr=0x");
	FlbHexDigits(&o, (unsigned __int64)addr, 16);
	FlbStr(&o, " rva=0x");
	FlbHexDigits(&o, (unsigned __int64)(addr - gameBase), 8);
	FlbStr(&o, " crashTid=");
	FlbDecU(&o, (unsigned __int64)GetCurrentThreadId());
	if (InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0)
		FlbStr(&o, " afterStop=1");
	FlbStr(&o, "\r\n");

	WriteCrashRecordFile(o.b, o.n);
}

// The throw ring's one exit. Opens the file once, writes what the ring holds
// and closes it; a second call with nothing pending opens nothing, so the two
// death paths can both call it for the same death.
namespace crash_record_detail {
struct ThrowFlushCtx
{
	HANDLE h;
	long   written;
};
} // namespace crash_record_detail
using namespace crash_record_detail;

static void ThrowFlushSink(void* ctx, const char* text, size_t len)
{
	ThrowFlushCtx* c = (ThrowFlushCtx*)ctx;
	DWORD bytes = 0;
	WriteFile(c->h, text, (DWORD)len, &bytes, NULL);
	++c->written;
}

static void FlushThrowRing()
{
	if (!g_cppExFilePath[0] || ThrowRingPending() <= 0)
		return;

	LONG index = InterlockedIncrement(&g_cppExRecordsWritten);
	HANDLE hFile = CreateFileA(g_cppExFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		(index == 1) ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;
	if (index != 1)
		SetFilePointer(hFile, 0, NULL, FILE_END);

	// The header says how many throws were captured against how many survived,
	// so a ring that overran cannot read as a session that threw sixteen times.
	char head[160];
	CrashBuf o;
	o.b = head;
	o.cap = sizeof(head);
	o.n = 0;
	FlbStr(&o, "THROWS: captured=");
	FlbDecU(&o, (unsigned __int64)(unsigned long)ThrowRingPushed());
	FlbStr(&o, " lost=");
	FlbDecU(&o, (unsigned __int64)(unsigned long)ThrowRingLost());
	FlbStr(&o, " total=");
	FlbDecU(&o, (unsigned __int64)(unsigned long)CppExceptionCount());
	FlbStr(&o, "\r\n");
	DWORD bytes = 0;
	WriteFile(hFile, o.b, (DWORD)o.n, &bytes, NULL);

	ThrowFlushCtx ctx;
	ctx.h = hFile;
	ctx.written = 0;
	ThrowRingDrain(ThrowFlushSink, &ctx);
	CloseHandle(hFile);
}

LONG WINAPI NavMeshCrashHandler(PEXCEPTION_POINTERS pExInfo)
{
	// Our own guarded faults are not crashes.
	if (InOurGuard())
		return EXCEPTION_CONTINUE_SEARCH;

	if (!pExInfo || !pExInfo->ExceptionRecord || !pExInfo->ContextRecord || !gameBase)
		return EXCEPTION_CONTINUE_SEARCH;

	DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
	int kind = FatalKindForCode(code);

	// A C++ throw names its own type only here, first-chance, and nothing
	// downstream can recover it. Most throws are caught and harmless, so the
	// note is always taken while the recorded ones are rationed, and they go
	// to a file of their own: a caught throw is not a crash, and writing it
	// into crash_dump.txt would cost that file its meaning.
	//
	// The record goes into memory, not onto disk. This runs on whatever thread
	// threw, holding whatever that thread holds, and the game throws routinely
	// inside passes that hold locks every navigability query needs; the file is
	// written from the death paths below, where nothing is waiting any more.
	if (kind == FATAL_KIND_CPPEX)
	{
		CppExceptionNote(pExInfo->ExceptionRecord);
		if (!g_cppExFilePath[0])
			return EXCEPTION_CONTINUE_SEARCH;
		double now = ElapsedSec();
		LONG slot = CppExceptionClaimRecordSlot(now);
		if (slot > 0)
		{
			char type[128];
			CppExceptionLastType(type, sizeof(type));

			ThrowRecordFields f;
			f.seq       = slot;
			f.code      = code;
			f.addr      = (unsigned __int64)(uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress;
			f.rva       = (unsigned __int64)((uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress - gameBase);
			f.tid       = GetCurrentThreadId();
			f.atSec     = (long)now;
			f.afterStop = InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0;
			f.kind      = FatalKindToken(kind);
			f.type      = type;

			char line[THROW_RING_CHARS];
			ThrowRecordFormat(line, sizeof(line), f);
			ThrowRingPush(line);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (code != EXCEPTION_ACCESS_VIOLATION
	    && code != EXCEPTION_INT_DIVIDE_BY_ZERO
	    && code != EXCEPTION_STACK_OVERFLOW
#ifdef KEO_DEBUG
	    // Widened here in DEV so a genuinely *dispatched* heap corruption,
	    // fail-fast or invalid-parameter exception still gets a record; none
	    // of the three reaches any handler in a PROD build. The CRT's
	    // direct-call bypass (0xC0000417 handed straight to
	    // UnhandledExceptionFilter, no exception ever raised) never reaches
	    // this VEH at all -- the DEV-only UnhandledExceptionFilter/
	    // NtTerminateProcess detour (diag/exit_capture.cpp) is what catches
	    // that path.
	    && code != 0xC0000374L  // ntdll heap corruption
	    && code != 0xC0000409L  // fail-fast / stack buffer overrun
	    && code != 0xC0000417L  // CRT invalid-parameter exit code
#endif
	    )
		return EXCEPTION_CONTINUE_SEARCH;

#ifdef KEO_DEBUG
	// One of the three codes above can refault inside the record we are about
	// to write (CreateFileA/WriteFile can still touch the process heap under
	// the hood), and a corrupt heap is exactly what 0xC0000374 means. Without
	// this guard that refault re-enters here and recurses until the stack
	// gives out -- turning a diagnosable heap corruption into an undiagnosed
	// stack overflow. Never reset: at most one of these three is ever worth a
	// second attempt, and skipping a repeat is safe.
	if ((code == 0xC0000374L || code == 0xC0000409L || code == 0xC0000417L))
	{
		static volatile LONG s_inRiskyRecord = 0;
		if (InterlockedCompareExchange(&s_inRiskyRecord, 1, 0) != 0)
			return EXCEPTION_CONTINUE_SEARCH;
	}
#endif

	// The profiler's guarded reads fault inside its own image and handle it there.
	if (code == EXCEPTION_ACCESS_VIOLATION
	    && InProfilerImage((uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress))
	{
		InterlockedIncrement(&g_crashSkipProfiler);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Every early-out before the sequence increment, so a fault before the
	// path is known (or of a code we don't record) never consumes the PROD
	// build's one-record budget (g_crashSeq > 1 refuses every later fault,
	// this one included, for the rest of the process).
	if (!g_crashFilePath[0])
		return EXCEPTION_CONTINUE_SEARCH;

	LONG seq = InterlockedIncrement(&g_crashSeq);
#ifndef KEO_DEBUG
	// PROD: one record per process. RE_Kenshi writes its own dumps, and a fault
	// storm must not turn into a write storm from a faulting thread.
	if (seq > 1)
		return EXCEPTION_CONTINUE_SEARCH;
#endif

	if (code == EXCEPTION_STACK_OVERFLOW)
		EmitMinimalRecord(FatalKindToken(kind), seq, pExInfo);
	else
		EmitCrashRecord(FatalKindToken(kind), seq, pExInfo);
	FlushThrowRing();
	InterlockedExchange64(&g_lastRecordedAddr,
		(LONG64)(uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress);
	InterlockedExchange(&g_lastRecordedCode, (LONG)code);

	return EXCEPTION_CONTINUE_SEARCH;
}

// Unhandled-exception filter, chained. RE_Kenshi installs its own before any
// plugin loads, so ours sits in front of it and returns what it returns: its
// minidump, emergency save and report window behave exactly as before.
//
// This is the only place a death the vectored handler declines can still
// leave a record, and an allocation failure thrown as a C++ exception is the
// case it exists for. The one-record budget above deliberately does not apply
// here: that budget guards against a faulting thread looping, and this runs
// once, at the end.
LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = NULL;
static volatile LONG g_inUnhandledFilter = 0;

LONG WINAPI KEOUnhandledFilter(PEXCEPTION_POINTERS pExInfo)
{
	if (pExInfo && pExInfo->ExceptionRecord && g_crashFilePath[0]
	    && InterlockedCompareExchange(&g_inUnhandledFilter, 1, 0) == 0)
	{
		DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
		LONG64 addr = (LONG64)(uintptr_t)pExInfo->ExceptionRecord->ExceptionAddress;
		bool already =
			InterlockedCompareExchange64(&g_lastRecordedAddr, 0, 0) == addr &&
			InterlockedCompareExchange(&g_lastRecordedCode, 0, 0) == (LONG)code;
		if (!already)
		{
			if (FatalKindForCode(code) == FATAL_KIND_CPPEX)
				CppExceptionNote(pExInfo->ExceptionRecord);
			if (code == EXCEPTION_STACK_OVERFLOW)
				EmitMinimalRecord(FatalKindToken(FatalKindForCode(code)),
					InterlockedIncrement(&g_crashSeq), pExInfo);
			else
				EmitCrashRecord(FatalKindToken(FatalKindForCode(code)),
					InterlockedIncrement(&g_crashSeq), pExInfo);
		}
	}

	// Whatever the filter decided above, the throws captured on the way here
	// are written now: this is the last point that runs in this process.
	FlushThrowRing();

	if (g_prevUnhandledFilter)
		return g_prevUnhandledFilter(pExInfo);
	return EXCEPTION_CONTINUE_SEARCH;
}


// Claims a leftover crash_dump.txt before startPlugin sets g_crashFilePath,
// so any crash_dump.txt found on disk afterward can only be this process's
// own -- the collector no longer has to guess from mtimes that a live process
// keeps disturbing long after the fault (RE_Kenshi's own dialog can hold it
// open for minutes). Only the immediately preceding leftover survives, under
// crash_dump.prev.txt; an older one was already retired the same way one
// startup ago. Never touches g_crashFilePath or the vectored handler, so it
// is safe to call before either exists. A locked file or read-only directory
// just fails the rename -- the caller logs it and starts normally.
CrashClaimOutcome ClaimPreviousCrashDump(const std::string& dllDir, unsigned long* outLastError)
{
	*outLastError = 0;
	std::string cur = CrashDumpCurrentPath(dllDir);
	if (GetFileAttributesA(cur.c_str()) == INVALID_FILE_ATTRIBUTES)
		return CRASH_CLAIM_NONE;

	std::string prev = CrashDumpPreviousPath(dllDir);
	if (MoveFileExA(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING))
		return CRASH_CLAIM_RENAMED;

	*outLastError = GetLastError();
	return CRASH_CLAIM_FAILED;
}
