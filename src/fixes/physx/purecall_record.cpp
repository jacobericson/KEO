#include "fixes/physx/purecall_record.h"
#include "fixes/physx/purecall_layout.h"
#include "base/core.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"

// _AddressOfReturnAddress: the handler needs its own return-address slot to
// walk one frame further up into _purecall's own caller (purecall_layout.h).
#include <intrin.h>
#pragma intrinsic(_AddressOfReturnAddress)

namespace purecall_record_detail
{

// Resolved once at install (main thread), read-only afterwards. The handler
// never calls the loader: every range it needs is a plain integer compare.
// Main install fills these immutable ranges and the path/previous-handler
// state before publishing the encoded handler slot. Any purecall handler
// reads them directly, without a copied set. Failed install clears its
// unpublished path/handler; an armed handler has no concurrent metadata
// rewrite or torn set. Teardown uninstalls the slot without resetting ranges.
static volatile uintptr_t g_gameBase  = 0;
static volatile uintptr_t g_gameSize  = 0;
static volatile uintptr_t g_physxBase = 0;
static volatile uintptr_t g_physxSize = 0;

typedef void (__cdecl *PurecallHandlerFn)(void);

// Non-NULL only if PhysXCore64 already carried a handler at install time.
// Chained rather than replaced: see InstallPurecallRecorder for why.
static PurecallHandlerFn g_priorHandler = NULL;

static char g_dumpPath[MAX_PATH] = { 0 };
static volatile LONG g_recordSeq = 0;

// Teardown state (main thread only, both at install and at DLL_PROCESS_DETACH
// -- see UninstallPurecallRecorder). g_handlerAddr is 0 until armed.
static volatile uintptr_t g_handlerAddr    = 0;
static volatile unsigned __int64 g_installedEncoded = 0; // the value we wrote
static volatile unsigned __int64 g_priorRaw = 0;         // the value it replaced (0 = none)

// Deferred-install state (main thread only: startPlugin and
// hook_updateCameraZone are the only two callers). PhysXCore64.dll is
// usually not loaded yet when startPlugin runs, so a single failed probe
// there is not a verdict -- it is retried from PurecallRecordTick until it
// either arms or the retry window (purecall_layout.h) expires.
enum PurecallState { kPurecallStateOff = 0, kPurecallStatePending, kPurecallStateArmed, kPurecallStateGaveUp };
static int    g_state           = kPurecallStateOff;
static int    g_attemptCount    = 0;
static double g_firstAttemptSec = -1.0;
static double g_lastAttemptSec  = -1.0;

static void __cdecl KEO_OnPurecall(void);

// --- SEH-guarded reads. Standalone and POD-only, same idiom as
// ReadGameBytes16 in prologue.cpp: MSVC 2010 rejects __try in a function that
// also holds an object needing unwinding. ---

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

static bool SafeReadQword(uintptr_t addr, unsigned __int64* out)
{
	if (!addr)
		return false;
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const unsigned __int64*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// Reads a module's own PE header to get its image size (ProfilerImageResolve
// in core.cpp does the same thing for KEOProfiler.dll; duplicated here
// rather than shared, since this file is meant to stand alone -- see the
// design note in the header).
static bool ResolveModuleRange(uintptr_t base, uintptr_t* outSize)
{
	if (!base)
		return false;
	bool ok = true;
	GuardEnter();
	__try
	{
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			ok = false;
		}
		else
		{
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
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


// --- record formatter: base/fixed_log_buf.h's builder over a 512-byte buffer,
// no CRT, no allocation. The record's state (its path and its one-record
// budget) stays its own, decoupled from the SEH crash handler's: that handler
// answers an exception on the mod's own faulting thread, this one a CRT abort
// from inside PhysXCore64 with no exception at all, so neither can starve the
// other's one-record budget. ---

typedef FixedLogBufN<512> RecBuf;

static const char* ModuleClassName(PurecallModuleClass cls)
{
	switch (cls)
	{
	case PURECALL_MOD_EXE:   return "exe";
	case PURECALL_MOD_PHYSX: return "physx";
	default:                 return "unknown";
	}
}

// Runs on the faulting thread, an instant before abort(). No lock, no CRT
// stream, no allocation: a fixed buffer and plain CreateFileA/WriteFile,
// exactly like NavMeshCrashHandler in plugin/crash_record.cpp.
static void WriteRecord(unsigned __int64 tid, unsigned __int64 qpc,
                  unsigned __int64 purecallRet, unsigned __int64 culprit,
                  PurecallModuleClass cls, unsigned __int64 culpritRva,
                  bool chained)
{
	LONG seq = InterlockedIncrement(&g_recordSeq);
#ifndef KEO_DEBUG
	// PROD: one record per process, same rule as crash_dump.txt (plugin/crash_record.cpp) --
	// a fault storm on a dying thread must not become a write storm.
	if (seq > 1)
		return;
#endif
	if (!g_dumpPath[0])
		return;

	RecBuf o;
	o.n = 0;
	FlbStr(&o, "PURECALL #"); FlbDecU(&o, (unsigned __int64)(unsigned long)seq);
	FlbStr(&o, ": tid=0x"); FlbHexDigits(&o, tid, 8);
	FlbStr(&o, " qpc=0x"); FlbHexDigits(&o, qpc, 16);
	FlbStr(&o, " purecallRet=0x"); FlbHexDigits(&o, purecallRet, 16);
	FlbStr(&o, " culprit=0x"); FlbHexDigits(&o, culprit, 16);
	FlbStr(&o, " culpritMod="); FlbStr(&o, ModuleClassName(cls));
	FlbStr(&o, " culpritRva=0x"); FlbHexDigits(&o, culpritRva, 8);
	FlbStr(&o, " chained="); FlbDecU(&o, chained ? 1 : 0);
	FlbStr(&o, "\r\n");

	HANDLE hFile = CreateFileA(g_dumpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		(seq == 1) ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		if (seq != 1)
			SetFilePointer(hFile, 0, NULL, FILE_END);
		DWORD written = 0;
		WriteFile(hFile, o.b, (DWORD)o.n, &written, NULL);
		CloseHandle(hFile);
	}
}

// The handler PhysXCore64's _purecall calls in place of its own default (no
// handler at all, which just falls through to abort()). Prototype fixed by
// the CRT: void (__cdecl *)(void), called with a plain `call rax` -- no
// arguments, no return value read afterwards.
static void __cdecl KEO_OnPurecall(void)
{
	// The address _purecall itself will resume at once this call returns.
	// Reading it is informational only (it always lands in PhysXCore64); the
	// address one frame further up, at kCulpritSlotOffset past this slot, is
	// _purecall's own caller -- the real faulting virtual call.
	void* retSlot = _AddressOfReturnAddress();
	unsigned __int64 purecallRet = 0;
	SafeReadQword((uintptr_t)retSlot, &purecallRet);

	unsigned __int64 culprit = 0;
	SafeReadQword(CulpritSlotAddress((uintptr_t)retSlot), &culprit);

	PurecallModuleClass cls = ClassifyPurecallCulprit(culprit,
		g_gameBase, g_gameSize, g_physxBase, g_physxSize);
	unsigned __int64 culpritRva = 0;
	if (cls == PURECALL_MOD_EXE)
		culpritRva = culprit - g_gameBase;
	else if (cls == PURECALL_MOD_PHYSX)
		culpritRva = culprit - g_physxBase;

	LARGE_INTEGER qpc;
	QueryPerformanceCounter(&qpc);

	bool willChain = (g_priorHandler != NULL);
	WriteRecord((unsigned __int64)GetCurrentThreadId(), (unsigned __int64)qpc.QuadPart,
		purecallRet, culprit, cls, culpritRva, willChain);

	// Chain: whatever PhysXCore64 had installed before us still runs. See
	// InstallPurecallRecorder for why this is chain-not-refuse.
	if (g_priorHandler)
		g_priorHandler();

	// Return. _purecall's own tail -- NMSG_WRITE(25), set_abort_behavior,
	// then the jmp into abort() -- runs exactly as it always has. Nothing
	// here changes whether or how the process dies: that popup, and the
	// wedged exit behind it, is how this crash class was ever caught at all.
}

// One attempt to find PhysXCore64.dll and arm the slot. Returns true for any
// terminal outcome (armed, or a reason that will never change on retry:
// wrong bytes, an unreadable slot, a path too long, a failed VirtualProtect)
// and logs that outcome itself. Returns false only for "the module is not
// loaded yet" -- the one outcome purecall_record.cpp retries, silently, so
// the once-a-second probe doesn't spam the log.
static bool TryArmPurecallRecorder()
{
	uintptr_t physxBase = (uintptr_t)GetModuleHandleA("PhysXCore64.dll");
	if (!physxBase)
		return false; // retry later; nothing logged here

	uintptr_t physxSize = 0;
	if (!ResolveModuleRange(physxBase, &physxSize))
	{
		LogMsg("PurecallRecord: off (PhysXCore64.dll image header unreadable)");
		return true;
	}

	unsigned char sig[16];
	uintptr_t purecallAddr = physxBase + (uintptr_t)kPurecallRva;
	if (!SafeReadBytes((const void*)purecallAddr, sig, sizeof(sig))
	    || !PurecallSignatureMatches(sig, kPurecallSignatureLen))
	{
		LogMsg("PurecallRecord: off (PhysXCore64.dll _purecall bytes do not "
		       "match this build's signature -- a different PhysX build is loaded)");
		return true;
	}

	uintptr_t handlerAddr = physxBase + (uintptr_t)kPurecallHandlerPtrRva;
	unsigned __int64 rawExisting = 0;
	if (!SafeReadQword(handlerAddr, &rawExisting))
	{
		LogMsg("PurecallRecord: off (handler slot unreadable)");
		return true;
	}

	// Someone else already owns the slot. Chain to it rather than silently
	// replacing it (or refusing): whatever it does still happens, after our
	// own record is written, and the cost is one extra call on a path that is
	// already on its way to abort().
	PurecallHandlerFn existing = NULL;
	if (rawExisting != 0)
		existing = (PurecallHandlerFn)DecodePointer((PVOID)(uintptr_t)rawExisting);

	// Every global the handler reads is set before the slot is armed, so a
	// concurrent _purecall on another thread never observes our handler
	// pointer with stale support state behind it.
	uintptr_t gameSize = 0;
	ResolveModuleRange(gameBase, &gameSize); // best-effort; failure just widens "unknown"

	std::string dumpPath = GetDLLDirectory() + "purecall_dump.txt";
	if (dumpPath.size() >= sizeof(g_dumpPath))
	{
		LogMsg("PurecallRecord: off (mod directory path too long)");
		return true;
	}
	memcpy(g_dumpPath, dumpPath.c_str(), dumpPath.size() + 1);

	g_gameBase  = gameBase;
	g_gameSize  = gameSize;
	g_physxBase = physxBase;
	g_physxSize = physxSize;
	g_priorHandler = existing;
	g_priorRaw = rawExisting;

	DWORD oldProtect = 0;
	if (!VirtualProtect((LPVOID)handlerAddr, sizeof(unsigned __int64), PAGE_READWRITE, &oldProtect))
	{
		LogMsg("PurecallRecord: off (VirtualProtect on the handler slot failed)");
		g_priorHandler = NULL;
		g_priorRaw = 0;
		g_dumpPath[0] = 0;
		return true;
	}

	unsigned __int64 encoded = (unsigned __int64)(uintptr_t)EncodePointer((PVOID)(uintptr_t)&KEO_OnPurecall);
	*(volatile unsigned __int64*)handlerAddr = encoded;

	DWORD ignore = 0;
	VirtualProtect((LPVOID)handlerAddr, sizeof(unsigned __int64), oldProtect, &ignore);

	// Recorded only once the write above has actually happened, so
	// UninstallPurecallRecorder never restores a slot we never armed.
	g_installedEncoded = encoded;
	g_handlerAddr = handlerAddr;
	g_state = kPurecallStateArmed;

	std::ostringstream msg;
	msg << "PurecallRecord: armed at PhysXCore64.dll+0x" << std::hex << kPurecallRva
	    << " (base=0x" << physxBase << ")" << std::dec
	    << (existing ? ", chaining to the existing handler" : ", no prior handler")
	    << ", attempt " << g_attemptCount;
	LogMsg(msg.str());
	return true;
}

} // namespace
using namespace purecall_record_detail;


void InstallPurecallRecorder(bool enabled)
{
	if (!enabled)
	{
		g_state = kPurecallStateOff;
		LogMsg("PurecallRecord: off (physPurecallRecord=false)");
		return;
	}

	g_state = kPurecallStatePending;
	g_firstAttemptSec = ElapsedSec();
	g_lastAttemptSec = g_firstAttemptSec;
	++g_attemptCount;
	if (TryArmPurecallRecorder())
	{
		if (g_state != kPurecallStateArmed)
			g_state = kPurecallStateGaveUp; // a terminal failure, not "not loaded"
		return;
	}

	// PhysXCore64.dll is not loaded yet. This is the normal case, not an edge
	// case: the exe loads it well after plugin init. PurecallRecordTick
	// retries on the main thread until it arms or the retry window expires.
	LogMsg("PurecallRecord: deferred (PhysXCore64.dll not loaded at plugin init; "
	       "retrying on the main thread)");
}

void PurecallRecordTick(double now)
{
	if (g_state != kPurecallStatePending)
		return;
	if (!PurecallRetryDue(now, g_lastAttemptSec))
		return;

	g_lastAttemptSec = now;
	++g_attemptCount;
	if (TryArmPurecallRecorder())
	{
		if (g_state != kPurecallStateArmed)
			g_state = kPurecallStateGaveUp; // a terminal failure, not "not loaded"
		return;
	}

	if (PurecallRetryExpired(now, g_firstAttemptSec))
	{
		g_state = kPurecallStateGaveUp;
		std::ostringstream msg;
		msg << "PurecallRecord: gave up after " << g_attemptCount
		    << " attempts over " << (now - g_firstAttemptSec)
		    << "s -- PhysXCore64.dll never loaded this session, no forensic net";
		LogMsg(msg.str());
	}
}

void UninstallPurecallRecorder()
{
	uintptr_t handlerAddr = g_handlerAddr;
	if (!handlerAddr)
		return; // never armed (off, or refused during install)

	// Compare the RAW qword, never the decoded pointer -- same reason as the
	// chain gate at install: DecodePointer's behaviour on an unexpected input
	// is not something to trust a safety decision to. If the slot no longer
	// holds exactly what we wrote, something else has taken it over since
	// (chained through us and then replaced, or otherwise); leave it alone,
	// since overwriting it here would be this same bug in the other
	// direction.
	unsigned __int64 current = 0;
	if (!SafeReadQword(handlerAddr, &current) || current != g_installedEncoded)
		return;

	DWORD oldProtect = 0;
	if (!VirtualProtect((LPVOID)handlerAddr, sizeof(unsigned __int64), PAGE_READWRITE, &oldProtect))
		return;

	*(volatile unsigned __int64*)handlerAddr = g_priorRaw;

	DWORD ignore = 0;
	VirtualProtect((LPVOID)handlerAddr, sizeof(unsigned __int64), oldProtect, &ignore);

	// Never re-run: a second call (there should not be one) must not restore
	// a slot some third party has taken since.
	g_handlerAddr = 0;
}
