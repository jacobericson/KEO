// Host tests for the stitch-byte guard: the classifier, the skip decision,
// the site patch, and the stub -- assembled and then executed through a
// harness that enters it with the site's register state.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/stitch/stitch_byte_guard_policy.h"

#include "check.h"

static int Rel32At(const unsigned char* p) { int d = 0; memcpy(&d, p, 4); return d; }

// The gate the executed stub calls through its slot.
static const void* g_seenOutput = NULL;
static int         g_seenType   = -1;
static int         g_gateResult = 1;
static int         g_gateCalls  = 0;

static int __fastcall TestGate(const void* output, int type)
{
	g_seenOutput = output;
	g_seenType = type;
	++g_gateCalls;
	return g_gateResult;
}

// Enters the stub as the game does: rsi = output, r15d = type, r8d and r9d
// live, rsp 16-aligned. The stub's resume target stores r8/r9 into the object
// so their survival can be checked, and returns rax (= [rsi+8]).
//
//   00 push rsi / push r15 / push rbx       ; rsp 16-aligned after these
//   04 mov  rsi, rcx
//   07 mov  r15d, edx
//   0A mov  r8d, 5A5A1234h
//   10 mov  r9d, 6B6B5678h
//   16 jmp  stub
// resume (1B):
//   1B mov  [rsi+58h], r8
//   1F mov  [rsi+60h], r9
//   23 pop  rbx / pop r15 / pop rsi / ret
static const size_t kHarnessResume = 0x1B;
static size_t BuildHarness(unsigned char* out, unsigned __int64 at, unsigned __int64 stub)
{
	static const unsigned char k[] =
	{
		0x56, 0x41, 0x57, 0x53,
		0x48, 0x8B, 0xF1,
		0x44, 0x8B, 0xFA,
		0x41, 0xB8, 0x34, 0x12, 0x5A, 0x5A,
		0x41, 0xB9, 0x78, 0x56, 0x6B, 0x6B,
		0xE9, 0x00, 0x00, 0x00, 0x00,
		0x4C, 0x89, 0x46, 0x58,
		0x4C, 0x89, 0x4E, 0x60,
		0x5B, 0x41, 0x5F, 0x5E, 0xC3
	};
	memcpy(out, k, sizeof(k));
	int rel = (int)((__int64)stub - (__int64)(at + 0x1B));
	memcpy(out + 0x17, &rel, 4);
	return sizeof(k);
}

typedef unsigned __int64 (*Harness_t)(void* output, int type);

int main()
{
	// --- classification ---
	Check(ClassifyStitchByteWrite(0, true, 0x10000) == SBG_IN_OBJECT, "a generation task is in-object");
	Check(ClassifyStitchByteWrite(2, false, 0) == SBG_IN_OBJECT, "a non-stitch task never reads the uid");
	Check(ClassifyStitchByteWrite(4, true, 0x0123) == SBG_STITCH_SECTOR, "a small uid is a sector");
	Check(ClassifyStitchByteWrite(4, true, 0xFFFE) == SBG_STITCH_SECTOR, "0xFFFE is still a sector");
	Check(ClassifyStitchByteWrite(4, true, 0xFFFF) == SBG_STITCH_INTERIOR,
	      "0xFFFF is an interior, as addStitchJob's jnb has it");
	Check(ClassifyStitchByteWrite(4, true, 0x30000) == SBG_STITCH_INTERIOR, "(index+1)<<16 is an interior");
	Check(ClassifyStitchByteWrite(4, false, 0) == SBG_STITCH_UNKNOWN, "an unread uid is unknown");

	Check(StitchByteSkipsWrite(SBG_STITCH_INTERIOR, true), "guard mode skips an interior store");
	Check(!StitchByteSkipsWrite(SBG_STITCH_INTERIOR, false), "observe mode keeps it");
	Check(!StitchByteSkipsWrite(SBG_STITCH_SECTOR, true), "a sector store is always kept");
	Check(!StitchByteSkipsWrite(SBG_IN_OBJECT, true), "an in-object store is always kept");
	Check(!StitchByteSkipsWrite(SBG_STITCH_UNKNOWN, true), "an unknown store is kept (vanilla)");

	// --- the pinned bytes ---
	Check(kStitchByteSiteBytes[0] == 0xC6 && kStitchByteSiteBytes[2] == (unsigned char)kStitchByteWriteOffset,
	      "the site is the byte store at +0x50");
	{
		int d = Rel32At(kStitchByteBranchBytes + 6);
		Check(kStitchByteBranchRva + 10 + (unsigned __int64)(__int64)d == kStitchByteSiteRva,
		      "the pinned jnz targets the site");
		Check(kStitchByteBranchBytes[0] == 0x41 && kStitchByteBranchBytes[2] == 0xFF,
		      "the pinned compare reads r15d");
	}
	Check(kStitchByteSiteRva + kStitchByteSiteLen == kStitchByteResumeRva, "resume follows the site");

	// --- the site patch ---
	{
		const unsigned __int64 site = 0x1403C911EULL;
		const unsigned __int64 stub = 0x150000000ULL;
		unsigned char p[8];
		Check(BuildStitchByteSitePatch(kStitchByteSiteBytes, site, stub, p), "the site patch builds");
		Check(p[0] == 0xE9 && p[5] == 0x90 && p[6] == 0x90 && p[7] == 0x90,
		      "the site becomes E9 rel32 + three nops");
		Check(site + 5 + (unsigned __int64)(__int64)Rel32At(p + 1) == stub, "the site jump resolves to the stub");

		unsigned char wrong[8];
		memcpy(wrong, kStitchByteSiteBytes, 8);
		wrong[0] = 0xE9;
		Check(!BuildStitchByteSitePatch(wrong, site, stub, p), "an already-patched site is refused");
		Check(!BuildStitchByteSitePatch(kStitchByteSiteBytes, site, 0x00007FFEA23F0000ULL, p),
		      "a stub out of rel32 reach is refused");
	}

	// --- the stub, statically ---
	{
		const unsigned __int64 stub = 0x150000000ULL;
		const unsigned __int64 slot = stub - 0x1000ULL;
		const unsigned __int64 resume = 0x1403C9126ULL;
		unsigned char buf[0x40];
		size_t len = 0;
		Check(BuildStitchByteStub(buf, sizeof(buf), stub, slot, resume, &len) && len == kStitchByteStubLen,
		      "the stub assembles");
		Check(buf[0x15] == 0xFF && buf[0x16] == 0x15
		      && stub + 0x1B + (unsigned __int64)(__int64)Rel32At(buf + 0x17) == slot,
		      "the gate call is indirect through the slot");
		Check(buf[0x36] == 0xE9 && stub + 0x3B + (unsigned __int64)(__int64)Rel32At(buf + 0x37) == resume,
		      "the stub ends in a jmp to the resume target");
		Check(memcmp(buf + 0x2E, kStitchByteSiteBytes, 8) == 0,
		      "the two original instructions are carried verbatim");
		Check(buf[0x2C] == 0x74 && 0x2E + (int)(signed char)buf[0x2D] == 0x32,
		      "the skip branch lands on the second original instruction");
		Check(!BuildStitchByteStub(buf, 4, stub, slot, resume, &len), "a buffer too small is refused");
		Check(!BuildStitchByteStub(buf, sizeof(buf), stub, slot, stub + 0x100000000ULL, &len),
		      "an unreachable resume target is refused");
	}
	{
		unsigned char k[8];
		Check(BuildStitchByteKeepAll(k, sizeof(k)) && k[0] == 0xB8 && k[1] == 1 && k[5] == 0xC3,
		      "the keep-all thunk is mov eax,1 / ret");
	}

	// --- the stub, executed ---
	{
		unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		Check(page != NULL, "an executable page for the harness");
		if (page)
		{
			const unsigned __int64 slotAddr    = (unsigned __int64)(uintptr_t)page;
			const unsigned __int64 harnessAddr = slotAddr + 0x100;
			const unsigned __int64 stubAddr    = slotAddr + 0x200;
			*(unsigned __int64*)page = (unsigned __int64)(uintptr_t)&TestGate;
			BuildHarness(page + 0x100, harnessAddr, stubAddr);
			Check(BuildStitchByteStub(page + 0x200, 0x100, stubAddr, slotAddr,
			                          harnessAddr + kHarnessResume, NULL),
			      "the stub assembles in the harness page");
			FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
			Harness_t run = (Harness_t)(page + 0x100);

			__declspec(align(16)) unsigned char obj[0x80];

			// Keep: the byte is cleared and the flow continues as vanilla.
			memset(obj, 0xAB, sizeof(obj));
			*(unsigned __int64*)(obj + 8) = 0x1122334455667788ULL;
			g_gateResult = 1; g_gateCalls = 0;
			unsigned __int64 rax = run(obj, 4);
			Check(g_gateCalls == 1 && g_seenOutput == obj && g_seenType == 4,
			      "the gate sees output and type");
			Check(obj[0x50] == 0x00, "a kept store clears +0x50");
			Check(obj[0x4F] == 0xAB && obj[0x51] == 0xAB, "and only that byte");
			Check(rax == 0x1122334455667788ULL, "rax is [rsi+8] at the resume target");
			Check(*(unsigned __int64*)(obj + 0x58) == 0x5A5A1234ULL, "r8 survives the gate call");
			Check(*(unsigned __int64*)(obj + 0x60) == 0x6B6B5678ULL, "r9 survives the gate call");

			// Skip: the byte is untouched and everything else is the same.
			memset(obj, 0xAB, sizeof(obj));
			*(unsigned __int64*)(obj + 8) = 0x0102030405060708ULL;
			g_gateResult = 0; g_gateCalls = 0;
			rax = run(obj, 4);
			Check(g_gateCalls == 1, "the gate runs once on the skip path");
			Check(obj[0x50] == 0xAB, "a skipped store leaves +0x50 alone");
			Check(rax == 0x0102030405060708ULL, "rax is [rsi+8] on the skip path too");
			Check(*(unsigned __int64*)(obj + 0x58) == 0x5A5A1234ULL, "r8 survives the skip path");

			// Another type passes straight through the same way.
			memset(obj, 0xAB, sizeof(obj));
			g_gateResult = 1;
			run(obj, 1);
			Check(g_seenType == 1 && obj[0x50] == 0x00, "a type-1 task keeps its store");

			VirtualFree(page, 0, MEM_RELEASE);
		}
	}

	return CheckExit("stitch_byte_guard_units");
}
