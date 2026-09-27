#include "fixes/streaming/navmesh_update_guard.h"
#include "fixes/streaming/navmesh_guard_policy.h"
#include "fixes/streaming/section_key_ring.h"
#include "base/core.h"
#include "base/fixed_log_buf.h"

#include <windows.h>

// changeMutex's offset inside the SectionManager. Held here rather than taken
// from game.h so this file stays buildable on its own, like the other recorders
// in this folder.
static const size_t kChangeMutexOffset = 0x200;

// An exclusively held, uncontended boost::shared_mutex reads exactly this:
// the lock path sets bit 22 and clears nothing else.
static const LONG kExclusiveBit = 0x00400000;

// The navmesh generator's zone lock: a process global, taken exclusively by a
// non-blocking acquire. NavMesh::update holds it across part of its add loop,
// so a fault there orphans it as well.
static const unsigned kGeneratorZoneMutexRva = 0x212DEB8;

// The two regions of NavMesh::update in which each lock is held, as exe RVAs,
// half-open. Each begins at the instruction the acquiring call returns to and
// ends at the instruction after the releasing call, so a frame anywhere inside
// one means this thread took that lock and has not yet released it.
//
// The zone region starts after the branch that tests the acquire's result, so
// the path where the non-blocking acquire failed is outside it: no code in
// either region releases the lock it belongs to, and no code outside it holds
// that lock. The zone region lies wholly inside the change region, so it adds
// nothing to the ownership question and is recorded only as a fact.
static const unsigned kChangeCsLo = 0x3AE670, kChangeCsHi = 0x3AE99F;
static const unsigned kZoneCsLo   = 0x3AE6DB, kZoneCsHi   = 0x3AE8A8;

static const int kMaxFrames = 64;
static const int kMaxChain  = 12;

// The "unwind only, run no handler" flag for RtlVirtualUnwind. Not declared by
// this toolchain's headers.
#ifndef UNW_FLAG_NHANDLER
#define UNW_FLAG_NHANDLER 0
#endif

static bool             g_enabled   = false;
static char             g_recordPath[MAX_PATH] = { 0 };
static unsigned __int64 g_gameBase  = 0;
static unsigned __int64 g_gameSize  = 0;
static LARGE_INTEGER    g_qpf;

static volatile LONG g_latched   = 0;
static volatile LONG g_recordSeq = 0;

static const LONG kMaxRecords = 8;


// --- SEH-guarded reads. Standalone and POD-only: MSVC 2010 rejects __try in a
// function that also holds an object needing unwinding. ---

static bool SafeReadLong(const void* addr, LONG* out)
{
	if (!addr)
		return false;
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(volatile const LONG*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool ResolveImageSize(unsigned __int64 base, unsigned __int64* outSize)
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


// --- record formatter: base/fixed_log_buf.h's builder over GBuf, a 1024-byte
// buffer; no CRT, no allocation. ---

typedef FixedLogBufN<1024> GBuf;


// What the frame walk established about the faulting thread.
struct UnwindFacts
{
	bool ok;          // the walk ran to completion without faulting
	bool inChangeCs;  // a live frame sits inside update's changeMutex region
	bool inZoneCs;    // a live frame sits inside its generator zone-lock region
	int  frames;
	int  chainCount;
	unsigned __int64 chain[kMaxChain]; // exe RVAs, innermost first
	bool stackScanUsed; // chain came from StackScanFallback, not RtlVirtualUnwind
};

// Fixed span above RSP that StackScanFallback reads, in qwords. A page is
// ample for the frame depths this guard has actually seen and keeps the scan
// itself bounded regardless of what RSP points at.
static const int kStackScanWords = 512;

// RtlVirtualUnwind needs unwind metadata for the current RIP to take a single
// step; it has none when RIP itself is not a valid code address (a call
// through a corrupt function pointer, the family this guard was extended
// for), so the walk above ends after frame 0 with an empty chain. The return
// addresses that would have named the caller are still sitting on the stack
// just below the corrupted call, in the ordinary x64 calling convention, so a
// bounded scan of the words above RSP recovers them without needing RIP to
// be valid at all. This is an approximation, not a proof: a stale qword left
// over from an earlier, already-returned call reads identically to a live
// return address, so a caller must not treat this chain as a verified stack.
//
// Only ever called with the fault's own, unmodified RSP -- never a value
// RtlVirtualUnwind has already advanced -- and only when the classic walk
// found nothing, so this never overwrites a chain the unwinder produced.
static void StackScanFallback(unsigned __int64 rsp, UnwindFacts* out)
{
	if (!rsp || !g_gameBase || out->chainCount != 0)
		return;

	bool any = false;
	GuardEnter();
	__try
	{
		volatile unsigned __int64* p = (volatile unsigned __int64*)rsp;
		for (int i = 0; i < kStackScanWords && out->chainCount < kMaxChain; ++i)
		{
			unsigned __int64 v = p[i];
			if (v >= g_gameBase && (!g_gameSize || v - g_gameBase < g_gameSize))
			{
				out->chain[out->chainCount++] = v - g_gameBase;
				any = true;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// Whatever was found before the bad page is kept; the scan just stops.
	}
	GuardLeave();
	out->stackScanUsed = any;
}

// Walks the faulting thread's real frame chain, from a copy of the faulting
// context, using the exe's own unwind metadata.
//
// This is what makes a release safe. The mutex word records that the lock is
// held, never by whom, and three other functions take the same mutex
// exclusively on other threads -- so "held" on its own would let a fault
// outside update's critical section release a stranger's lock. A live frame
// whose RIP lies inside the region between the acquire and the release is
// proof that this thread is the owner.
//
// Nothing here may fault: it runs inside an exception filter, where a second
// fault would take the process down and destroy the minidump of whatever the
// guard was about to decline. Hence the guarded walk, the frame cap, and the
// refusal to guess a frame when unwind data is missing.
static void UnwindWalk(const CONTEXT* faultCtx, UnwindFacts* out)
{
	out->ok = false;
	out->inChangeCs = false;
	out->inZoneCs = false;
	out->frames = 0;
	out->chainCount = 0;
	out->stackScanUsed = false;

	if (!faultCtx || !g_gameBase)
		return;

	bool ok = true;
	GuardEnter();
	__try
	{
		CONTEXT ctx;
		memcpy(&ctx, faultCtx, sizeof(CONTEXT));

		for (int depth = 0; depth < kMaxFrames; ++depth)
		{
			unsigned __int64 rip = (unsigned __int64)ctx.Rip;
			if (!rip)
				break;
			++out->frames;

			if (rip >= g_gameBase && (!g_gameSize || rip - g_gameBase < g_gameSize))
			{
				unsigned __int64 rva = rip - g_gameBase;
				if (rva >= kChangeCsLo && rva < kChangeCsHi)
					out->inChangeCs = true;
				if (rva >= kZoneCsLo && rva < kZoneCsHi)
					out->inZoneCs = true;
				if (out->chainCount < kMaxChain)
					out->chain[out->chainCount++] = rva;
			}

			ULONG64 imageBase = 0;
			PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, NULL);
			if (!fn)
				break;   // no unwind data: stop rather than guess a return address

			PVOID   handlerData = NULL;
			ULONG64 establisher = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn,
			                 &ctx, &handlerData, &establisher, NULL);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	out->ok = ok;

	// The fault's own RSP, never one RtlVirtualUnwind has stepped: only runs
	// when the walk above left the chain empty.
	StackScanFallback((unsigned __int64)faultCtx->Rsp, out);
}


// Runs on the faulting thread, inside the exception filter, so the faulting
// frame is still intact. No lock, no CRT stream, no allocation.
//
// The mod's vectored handler has already written crash_dump.txt for this same
// fault (it runs before any frame handler and returns CONTINUE_SEARCH), so the
// registers are recorded elsewhere. What only this record can carry is the
// state the decision turned on: both mutex words at the instant of the fault,
// the action taken, what the frame walk concluded, and the unwound call chain.
static void WriteRecord(const char* action, DWORD code, unsigned __int64 addr,
                        LONG changeMutex, bool changeMutexRead,
                        LONG genMutex, bool genMutexRead,
                        const UnwindFacts& uw)
{
	LONG seq = InterlockedIncrement(&g_recordSeq);
	if (seq > kMaxRecords || !g_recordPath[0])
		return;

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	GBuf o;
	o.n = 0;
	FlbStr(&o, "NMGUARD #");     FlbDecU(&o, (unsigned __int64)(unsigned long)seq);
	FlbStr(&o, ": action=");     FlbStr(&o, action);
	FlbStr(&o, " code=0x");      FlbHexDigits(&o, (unsigned __int64)code, 8);
	FlbStr(&o, " addr=0x");      FlbHexDigits(&o, addr, 16);
	if (g_gameBase && addr >= g_gameBase && addr - g_gameBase < g_gameSize)
	{
		FlbStr(&o, " rva=0x");
		FlbHexDigits(&o, addr - g_gameBase, 8);
	}
	else
	{
		FlbStr(&o, " rva=outside-exe");
	}
	FlbStr(&o, " tid=");         FlbDecU(&o, (unsigned __int64)GetCurrentThreadId());

	FlbStr(&o, " changeMutex=");
	if (changeMutexRead) { FlbStr(&o, "0x"); FlbHexDigits(&o, (unsigned __int64)(unsigned long)changeMutex, 8); }
	else                 { FlbStr(&o, "unreadable"); }

	FlbStr(&o, " genMutex=");
	if (genMutexRead) { FlbStr(&o, "0x"); FlbHexDigits(&o, (unsigned __int64)(unsigned long)genMutex, 8); }
	else              { FlbStr(&o, "unreadable"); }

	FlbStr(&o, " qpc=0x");       FlbHexDigits(&o, (unsigned __int64)now.QuadPart, 16);
	if (g_qpf.QuadPart > 0)
	{
		FlbStr(&o, " ms=");
		FlbDecU(&o, (unsigned __int64)((now.QuadPart * 1000) / g_qpf.QuadPart));
	}

	FlbStr(&o, "\r\n  unwind=");
	FlbStr(&o, uw.ok ? "ok" : "failed");
	FlbStr(&o, " frames=");   FlbDecU(&o, (unsigned __int64)(unsigned int)uw.frames);
	FlbStr(&o, " inChangeCs="); FlbDecU(&o, uw.inChangeCs ? 1 : 0);
	FlbStr(&o, " inZoneCs=");   FlbDecU(&o, uw.inZoneCs ? 1 : 0);

	// Partial when the walk faulted: the frames below the one that faulted were
	// never reached, so a chain under unwind=failed is a prefix, not a stack.
	// A stack-scanned chain carries its own label -- it is stack words that
	// looked like exe addresses, not a walk RtlVirtualUnwind verified, and a
	// reader must not treat the two as equally certain.
	if (uw.stackScanUsed)
		FlbStr(&o, "\r\n  chain(stack scan, unverified, closest to RSP first)=");
	else
		FlbStr(&o, uw.ok ? "\r\n  chain(exe frames, innermost first)="
		                : "\r\n  chain(PARTIAL, walk faulted)=");
	for (int i = 0; i < uw.chainCount; ++i)
	{
		if (i) FlbChar(&o, ',');
		FlbStr(&o, "0x");
		FlbHexDigits(&o, uw.chain[i], 8);
	}
	if (uw.chainCount == 0)
		FlbStr(&o, "none");
	FlbStr(&o, "\r\n");

	// The last record the file can hold says so. Without this a truncated file
	// is indistinguishable from a session that faulted exactly eight times.
	if (seq == kMaxRecords)
		FlbStr(&o, "  NOTE: record cap reached; any later fault is not recorded here.\r\n");

	HANDLE hFile = CreateFileA(g_recordPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		(seq == 1) ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		if (seq != 1)
			SetFilePointer(hFile, 0, NULL, FILE_END);
		DWORD written = 0;
		WriteFile(hFile, o.b, (DWORD)o.n, &written, NULL);
#ifdef ZONEOPT_DEBUG
		// The section-table lookups this thread was about to make, in a second
		// write with its own buffer: the record above fills a fixed 1 KiB that
		// a long chain can already come close to, and a ring squeezed into it
		// would be the part that silently disappears.
		{
			char ring[1536];
			size_t ringLen = SectionKeyRingFormat(ring, sizeof(ring), 2);
			if (ringLen > 0)
				WriteFile(hFile, ring, (DWORD)ringLen, &written, NULL);
		}
#endif
		CloseHandle(hFile);
	}
}


static bool IsRecordableCode(DWORD code)
{
	// Only hardware faults, and never a C++ throw (0xE06D7363): the game throws
	// routinely inside this pass, and recording those would spend the record
	// budget before a real fault ever arrives. A stack overflow is excluded too,
	// because the filter runs on the overflowed stack and the record buffer
	// below would be the second fault.
	return code == EXCEPTION_ACCESS_VIOLATION
	    || code == EXCEPTION_ILLEGAL_INSTRUCTION
	    || code == EXCEPTION_PRIV_INSTRUCTION
	    || code == EXCEPTION_DATATYPE_MISALIGNMENT
	    || code == EXCEPTION_INT_DIVIDE_BY_ZERO;
}


// The exception filter, and the whole of the guard's behaviour: it classifies
// the fault, writes the record and always returns CONTINUE_SEARCH, which leaves
// the fault to RE_Kenshi's unhandled-exception filter exactly as it would land
// with no guard here at all. Nothing is released and the thread is not resumed.
//
// A fault taken inside the change critical section means a reader has just
// found damage in the streaming collection while this thread held changeMutex
// exclusively. That lock is the only thing keeping every other reader out of
// the same damage, so it is left held on purpose: the session can no longer
// answer a navigability query, but it stops there instead of dying again in a
// shared-mode reader a second later.
static LONG GuardFilter(EXCEPTION_POINTERS* ep, void* sectionMgr)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
		return EXCEPTION_CONTINUE_SEARCH;

	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (!IsRecordableCode(code))
		return EXCEPTION_CONTINUE_SEARCH;

	unsigned __int64 addr = (unsigned __int64)ep->ExceptionRecord->ExceptionAddress;

	UnwindFacts uw;
	UnwindWalk(ep->ContextRecord, &uw);

	LONG changeMutex = 0;
	bool changeMutexRead = sectionMgr
		&& SafeReadLong((const char*)sectionMgr + kChangeMutexOffset, &changeMutex);

	LONG genMutex = 0;
	bool genMutexRead = g_gameBase
		&& SafeReadLong((const void*)(g_gameBase + kGeneratorZoneMutexRva), &genMutex);

	NavMeshGuardFacts facts;
	facts.unwindOk        = uw.ok;
	facts.inChangeCs      = uw.inChangeCs;
	facts.changeMutexRead = changeMutexRead;
	facts.changeMutexHeld = changeMutexRead && (changeMutex & kExclusiveBit) != 0;

	NavMeshGuardOutcome outcome = NavMeshGuardDecide(facts);

	WriteRecord(NavMeshGuardOutcomeName(outcome), code, addr,
	            changeMutex, changeMutexRead, genMutex, genMutexRead, uw);

	// Every latch is recorded, but only the first says so in the log: a second
	// would repeat a message the player has already acted on, and logging from
	// a thread in this state is the riskiest work left in this filter.
	if (outcome == NMGUARD_LATCH && InterlockedExchange(&g_latched, 1) == 0)
	{
		LogMsgDeferrable("NavMeshGuard: fault inside NavMesh::update's change section -- "
		                 "the navmesh change lock is left held deliberately, so no character "
		                 "can be ordered to move from here on. Answer Yes to the crash "
		                 "handler's save offer: it writes emergency_save_N and repoints "
		                 "continue= at it. The process ends either way.");
	}

	return EXCEPTION_CONTINUE_SEARCH;
}


char NavMeshUpdateGuardCall(void* sectionMgr, NavMeshUpdateFn orig)
{
	if (!orig)
		return 0;
	if (!g_enabled)
		return orig(sectionMgr);

	char served = 0;
	__try
	{
		served = orig(sectionMgr);
	}
	__except (GuardFilter(GetExceptionInformation(), sectionMgr))
	{
		// GuardFilter never returns EXECUTE_HANDLER, so this body cannot run.
		// The language requires it to exist for the filter to be attached.
		served = 0;
	}
	return served;
}


void NavMeshUpdateGuardInit(const std::string& dllDir, unsigned __int64 gameBase, bool enabled)
{
	g_enabled = false;

	std::string path = dllDir + "navmesh_guard.txt";

	// Retire a leftover before the enabled check, not after it. A record from
	// an earlier session is indistinguishable from this one's, so a run with
	// the guard switched off must not leave one behind for a collector to
	// pick up: the file's presence is what proves this run wrote it.
	if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		std::string prev = dllDir + "navmesh_guard.prev.txt";
		MoveFileExA(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
	}

	if (!enabled || !gameBase)
		return;

	g_gameBase = gameBase;
	if (!ResolveImageSize(gameBase, &g_gameSize))
		g_gameSize = 0;

	QueryPerformanceFrequency(&g_qpf);

	if (path.size() < sizeof(g_recordPath))
		memcpy(g_recordPath, path.c_str(), path.size() + 1);

	g_enabled = true;
}


bool NavMeshUpdateGuardLatched()
{
	return InterlockedCompareExchange(&g_latched, 0, 0) != 0;
}
