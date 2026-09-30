#include "diag/exit_capture.h"

#ifdef ZONEOPT_DEBUG

#include "diag/exit_capture_policy.h"
#include "base/core.h"
#include "diag/module_bases.h"
#include "game/prologue_policy.h"
#include <dbghelp.h>   // MINIDUMP_TYPE / MINIDUMP_EXCEPTION_INFORMATION only -- the
                       // function itself is resolved dynamically below, so this
                       // does not require linking dbghelp.lib.
#include <cstdio>
#include "plugin/hook_manifest.h"

// Implemented in plugin/crash_record.cpp, next to crash_dump.txt's own record counter.
bool ExitCaptureAnyCrashRecorded();

namespace exit_capture_detail
{

typedef LONG (WINAPI *UefFn)(EXCEPTION_POINTERS*);
typedef LONG (NTAPI *NtTerminateProcessFn)(HANDLE, LONG);
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
	PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
	PMINIDUMP_CALLBACK_INFORMATION);

const int kMaxFrames = 32;

UefFn                g_origUef = NULL;
NtTerminateProcessFn g_origTerminate = NULL;
MiniDumpWriteDumpFn  g_miniDumpWriteDump = NULL;

// Set once at install, read by ExitCaptureNoteStopHookOutcome and never
// written again: what actually came up, independent of whether the game ever
// gets far enough for it to matter.
bool g_uefInstalled = false;
bool g_terminateInstalled = false;

HANDLE g_dumpThread   = NULL;
HANDLE g_requestEvent = NULL;
HANDLE g_doneEvent    = NULL;
// Both pre-opened at install (main thread), never from the dying or dump
// thread: CreateFileA converts its path argument through the process heap,
// and a corrupt heap (0xC0000374) is exactly the case this exists for. A
// 0-byte file at session end means the instrument armed but never fired --
// the same convention as the profiler's *_hullcrash.txt.
HANDLE g_textFile = INVALID_HANDLE_VALUE;
HANDLE g_dumpFile = INVALID_HANDLE_VALUE;

volatile LONG g_fired = 0; // one-shot across both hooks: only the first real capture writes

// The dying thread's own findings, handed to the dump thread. g_fired is
// one-shot and set before either hook body returns, so this needs no lock:
// SetEvent/WaitForSingleObject already give the dump thread's read a
// happens-after relationship with the dying thread's writes.
struct CaptureState
{
	int    source;
	unsigned long code;
	unsigned long tid;
	unsigned __int64 frames[kMaxFrames];
	unsigned __int64 imageBases[kMaxFrames];
	int    frameCount;
	EXCEPTION_POINTERS* exPtrs; // set only on the UEF path; NULL from NtTerminateProcess
};
CaptureState g_req;

// RtlExitUserProcess's first NtTerminateProcess call passes NULL for "every
// thread in this process"; GetCurrentProcess() is the -1 pseudo-handle most
// other callers (including the CRT's direct bypass) use. Both mean "this
// process", never another one. A real handle from OpenProcess is resolved by
// PID, which needs PROCESS_QUERY_(LIMITED_)INFORMATION on that handle -- a
// handle that carries only PROCESS_TERMINATE fails this check and is treated
// as "not us"; no such caller is known in this codebase.
bool IsCurrentProcess(HANDLE h)
{
	if (h == NULL || h == GetCurrentProcess())
		return true;
	DWORD pid = GetProcessId(h);
	return pid != 0 && pid == GetCurrentProcessId();
}

// Whether this death is the one the instrument exists for -- the boolean
// logic itself is host-tested (exit_capture_policy.cpp); this just reads the
// three live inputs it needs: the stop hook's row state and two globals.
bool ShouldArm()
{
	return ExitCaptureShouldArm(
		HookRowInstalled(HOOK_NAVMESH_STOP),
		InterlockedCompareExchange(&g_navMeshStopSeen, 0, 0) != 0,
		ExitCaptureAnyCrashRecorded());
}

// Runs once, on the dedicated dump thread, never on the dying thread:
// MiniDumpWriteDump can itself allocate or hang walking a corrupt heap, and
// the whole point of a separate thread is that the dying thread's wait below
// is bounded regardless of what this one does.
DWORD WINAPI DumpThreadProc(LPVOID)
{
	for (;;)
	{
		if (WaitForSingleObject(g_requestEvent, INFINITE) != WAIT_OBJECT_0)
			continue;

		if (g_textFile != INVALID_HANDLE_VALUE)
		{
			int fullCount = 0;
			bool fullValid = false;
			const ModuleBaseEntry* full = FullModuleBases(&fullCount, &fullValid);
			char line[4096];
			size_t n = ExitCaptureFormatRecord(line, sizeof(line), g_req.source,
				g_req.code, g_req.tid, g_req.frames, g_req.imageBases, g_req.frameCount,
				full, fullCount, fullValid);
			DWORD written = 0;
			WriteFile(g_textFile, line, (DWORD)n, &written, NULL);
			FlushFileBuffers(g_textFile);
		}

		if (g_miniDumpWriteDump && g_dumpFile != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_EXCEPTION_INFORMATION mei;
			MINIDUMP_EXCEPTION_INFORMATION* meiPtr = NULL;
			if (g_req.exPtrs)
			{
				mei.ThreadId = g_req.tid;
				mei.ExceptionPointers = g_req.exPtrs;
				mei.ClientPointers = FALSE;
				meiPtr = &mei;
			}
			// Deliberately small: MiniDumpWithFullMemory on this game runs
			// several GB, which would make the dying thread's bounded wait
			// pointless. IndirectlyReferencedMemory follows every pointer the
			// captured stacks and registers hold -- what a post-mortem stack
			// walk and "what did a local point at" read actually needs -- and
			// ThreadInfo keeps each thread's context and TEB.
			MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
				MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
			g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), g_dumpFile,
				type, meiPtr, NULL, NULL);
			FlushFileBuffers(g_dumpFile);
		}

		SetEvent(g_doneEvent);
	}
}

// Runs on the dying thread. Small and allocation-free by design: a fixed
// struct, one intrinsic stack walk, a handful of VirtualQuery syscalls (no
// heap touched -- MEMORY_BASIC_INFORMATION is a caller-owned stack struct),
// and two syscalls to hand off to the dump thread and wait, bounded.
void CaptureAndSignal(int source, unsigned long code, EXCEPTION_POINTERS* exPtrs)
{
	g_req.source = source;
	g_req.code = code;
	g_req.tid = GetCurrentThreadId();
	g_req.exPtrs = exPtrs;
	g_req.frameCount = (int)CaptureStackBackTrace(1, kMaxFrames, (PVOID*)g_req.frames, NULL);
	for (int i = 0; i < g_req.frameCount; ++i)
	{
		MEMORY_BASIC_INFORMATION mbi;
		g_req.imageBases[i] =
			(VirtualQuery((LPCVOID)(uintptr_t)g_req.frames[i], &mbi, sizeof(mbi)) == sizeof(mbi)
			 && mbi.Type == MEM_IMAGE)
			? (unsigned __int64)(uintptr_t)mbi.AllocationBase : 0;
	}

	if (g_requestEvent && g_doneEvent && g_dumpThread)
	{
		ResetEvent(g_doneEvent);
		SetEvent(g_requestEvent);
		// Bounded: the dump thread can itself hang inside MiniDumpWriteDump on
		// a corrupt heap. A timeout here means the record or the dump can come
		// out partial or absent, never that this thread hangs the shutdown.
		WaitForSingleObject(g_doneEvent, 4000);
	}
}

LONG WINAPI Hook_UnhandledExceptionFilter(EXCEPTION_POINTERS* ep)
{
	// Only the CRT's fabricated-code bypass ever calls UnhandledExceptionFilter
	// without a real exception dispatch. Every other code reaching here is a
	// genuinely unhandled exception that the VEH and RE_Kenshi's own handler
	// (its own in-process minidump, emergency save and report window) already
	// see; arming for those would run a second in-process MiniDumpWriteDump
	// concurrently with RE_Kenshi's, and dbghelp is not reentrant across
	// threads.
	unsigned long code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
	if (ExitCaptureUefCodeArmable(code) && ShouldArm()
	    && InterlockedCompareExchange(&g_fired, 1, 0) == 0)
	{
		CaptureAndSignal(EXITCAP_SOURCE_UEF, code, ep);
	}
	return g_origUef ? g_origUef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

LONG NTAPI Hook_NtTerminateProcess(HANDLE hProcess, LONG exitStatus)
{
	if (IsCurrentProcess(hProcess) && ShouldArm()
	    && InterlockedCompareExchange(&g_fired, 1, 0) == 0)
	{
		CaptureAndSignal(EXITCAP_SOURCE_TERMINATE, (unsigned long)exitStatus, NULL);
	}
	return g_origTerminate ? g_origTerminate(hProcess, exitStatus) : 0;
}

// SEH-guarded 16-byte read, the same idiom as purecall_record.cpp's
// SafeReadBytes: MSVC 2010 rejects __try in a function that also holds an
// object needing unwinding, so this stays a standalone, POD-only read.
bool SafeRead16(const void* addr, unsigned char* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		memcpy(out, addr, 16);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

void AppendHex16(std::string& out, const unsigned char* b)
{
	static const char* kHex = "0123456789ABCDEF";
	for (int i = 0; i < 16; ++i)
	{
		out += kHex[(b[i] >> 4) & 0xF];
		out += kHex[b[i] & 0xF];
	}
}

// Prologue-checked with the same byte classification the build gate uses
// (prologue_policy.h), but called directly rather than through
// VerifyPrologueAt: these are OS DLL exports, never a game-exe site, so they
// never touch g_hookPrologueCount or the "gate=ok(<n> sites, <k> shared)"
// token, and a failure here is not a build-gate refusal -- the plugin still
// loads, this one forensic net just stays off. Logged with its own prefix so
// it never reads as a build-gate failure. A future Windows update changing
// either export's prologue (KERNELBASE's rarely does; ntdll's syscall stub
// carries a syscall number that occasionally moves between major Windows
// versions) shows up as this piece printing "?" rather than silently doing
// nothing.
bool InstallExportHook(const char* moduleName, const char* symbol,
                        const unsigned char* expectBytes, void* detour, void** origOut)
{
	HMODULE m = GetModuleHandleA(moduleName);
	if (!m)
		return false;
	void* target = (void*)GetProcAddress(m, symbol);
	if (!target)
		return false;

	unsigned char actual[16];
	bool readable = SafeRead16(target, actual);
	PrologueClass cls = ClassifyPrologue(actual, expectBytes, readable);

	if (cls == PROLOGUE_UNREADABLE)
	{
		LogMsg(std::string("ExitCapture: ") + moduleName + "!" + symbol + " prologue unreadable");
		return false;
	}
	if (cls == PROLOGUE_MISMATCH)
	{
		std::string line = std::string("ExitCapture: ") + moduleName + "!" + symbol
			+ " prologue mismatch, expected ";
		AppendHex16(line, expectBytes);
		line += ", found ";
		AppendHex16(line, actual);
		LogMsg(line);
		return false;
	}
	if (cls == PROLOGUE_SHARED)
		LogMsg(std::string("ExitCapture: ") + moduleName + "!" + symbol
			+ " already hooked by another module, tail matches, chaining");

	return KenshiLib::AddHook(target, detour, origOut) == KenshiLib::SUCCESS;
}

// Renames `curPath` to `prevPath` only if `curPath` exists and is non-empty:
// an empty leftover carries no evidence, so it is simply reclaimed by the
// CREATE_ALWAYS that follows rather than preserved as a ".prev" file. Same
// idiom as crash_claim.h's rotation, but decided by size here because this
// instrument's own file is pre-created (0 bytes) at every launch that arms,
// where crash_dump.txt is only ever created on an actual write.
//
// On EXITCAP_CLAIM_FAILED the caller must not open `curPath` this session:
// if it did, the CREATE_ALWAYS that follows would wipe the very record the
// rotation was supposed to preserve, which is exactly the loss rotation
// exists to prevent.
ExitCaptureClaimOutcome ClaimIfNonEmpty(const std::string& curPath, const std::string& prevPath,
                                         unsigned long* outLastError)
{
	*outLastError = 0;
	WIN32_FILE_ATTRIBUTE_DATA attr;
	if (!GetFileAttributesExA(curPath.c_str(), GetFileExInfoStandard, &attr))
		return EXITCAP_CLAIM_NONE;
	unsigned __int64 size = ((unsigned __int64)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
	if (size == 0)
		return EXITCAP_CLAIM_NONE;
	if (MoveFileExA(curPath.c_str(), prevPath.c_str(), MOVEFILE_REPLACE_EXISTING))
		return EXITCAP_CLAIM_ROTATED;
	*outLastError = GetLastError();
	return EXITCAP_CLAIM_FAILED;
}

} // namespace
using namespace exit_capture_detail;

void InstallExitCapture(const std::string& dllDir)
{
	std::string textPath = ExitCaptureTextPath(dllDir);
	unsigned long textClaimErr = 0;
	ExitCaptureClaimOutcome textClaim =
		ClaimIfNonEmpty(textPath, ExitCaptureTextPrevPath(dllDir), &textClaimErr);
	if (textClaim != EXITCAP_CLAIM_FAILED)
		g_textFile = CreateFileA(textPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	std::string dumpPath = ExitCaptureMinidumpPath(dllDir);
	unsigned long dumpClaimErr = 0;
	ExitCaptureClaimOutcome dumpClaim =
		ClaimIfNonEmpty(dumpPath, ExitCaptureMinidumpPrevPath(dllDir), &dumpClaimErr);
	if (dumpClaim != EXITCAP_CLAIM_FAILED)
		g_dumpFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	// Pre-loaded now, on the main thread: LoadLibrary takes the loader lock,
	// which the dump thread (or a thread mid-fault) must never do -- the same
	// rule SnapshotModuleBases documents for its own Toolhelp snapshot. Only
	// the function pointer is used later; nothing here requires linking
	// dbghelp.lib.
	HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
	if (dbghelp)
		g_miniDumpWriteDump = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");

	g_requestEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	g_doneEvent    = CreateEventA(NULL, FALSE, FALSE, NULL);
	bool dumpThreadOk = false;
	if (g_requestEvent && g_doneEvent)
	{
		DWORD tid = 0;
		g_dumpThread = CreateThread(NULL, 0, DumpThreadProc, NULL, 0, &tid);
		dumpThreadOk = g_dumpThread != NULL;
	}

	// Captured from this build machine's kernelbase.dll: the
	// exported function's own first 16 bytes.
	static const unsigned char kUefBytes[16] = {
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
		0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41
	};
	static const unsigned char kNtTerminateBytes[16] = {
		0x4C, 0x8B, 0xD1, 0xB8, 0x2C, 0x00, 0x00, 0x00,
		0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01
	};

	// kernelbase.dll carries the real implementation. kernel32.dll's own copy
	// of this export is a genuine, separately hookable stub on this build
	// (a `jmp qword ptr [rip+...]`, not a classic export-directory forward),
	// but every call through it still lands on kernelbase's code -- once that
	// is hooked, a caller that resolved the symbol from kernel32.dll instead
	// still hits the patched bytes after one extra jump, so hooking it too
	// would only recheck a stub that already funnels here.
	g_uefInstalled = InstallExportHook("kernelbase.dll", "UnhandledExceptionFilter",
		kUefBytes, (void*)Hook_UnhandledExceptionFilter, (void**)&g_origUef);

	// ntdll!NtTerminateProcess is the one choke point every termination path
	// funnels through -- kernel32's TerminateProcess and ExitProcess, RTL's
	// own RtlExitUserProcess, and the CRT's direct TerminateProcess(self,
	// 0xC0000417) bypass all end here. One hook here catches every path a
	// hook on kernel32!TerminateProcess plus kernel32!ExitProcess would need
	// two hooks (and a wrapper for RtlExitUserProcess) to cover.
	g_terminateInstalled = InstallExportHook("ntdll.dll", "NtTerminateProcess",
		kNtTerminateBytes, (void*)Hook_NtTerminateProcess, (void**)&g_origTerminate);

	LogMsg(ExitCaptureInstallLogLine(g_uefInstalled, g_terminateInstalled, dumpThreadOk,
		g_miniDumpWriteDump != NULL));

	if (textClaim == EXITCAP_CLAIM_FAILED)
		LogMsg("ExitCapture: text=" + ExitCaptureClaimFailedToken(textClaimErr));
	else if (g_textFile != INVALID_HANDLE_VALUE)
		LogMsg("ExitCapture: text=" + textPath);

	if (dumpClaim == EXITCAP_CLAIM_FAILED)
		LogMsg("ExitCapture: dump=" + ExitCaptureClaimFailedToken(dumpClaimErr));
	else if (g_dumpFile != INVALID_HANDLE_VALUE)
		LogMsg("ExitCapture: dump=" + dumpPath);
}

// Called from the NavMesh::stop hook's own install site, on the NavMesh
// background thread (the first hook_dispatchJob call), never the main
// thread: LogMsgDeferrable, not LogMsg, carries the line over. Building the
// std::string is safe on this thread; only the synchronous log write is not.
void ExitCaptureNoteStopHookOutcome(bool installed)
{
	std::string line = installed
		? ExitCaptureArmedLogLine(g_uefInstalled, g_terminateInstalled)
		: ExitCaptureDisarmedLogLine();
	LogMsgDeferrable(line.c_str());
}

#else // !ZONEOPT_DEBUG

void InstallExitCapture(const std::string&) {}
void ExitCaptureNoteStopHookOutcome(bool) {}

#endif
