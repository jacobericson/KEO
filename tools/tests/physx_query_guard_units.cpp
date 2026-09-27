#include <cstdio>
#include <cstring>
#include "fixes/physx/physx_query_guard_policy.h"

#include "check.h"

// Stand-in module layout: PhysXCore64 occupies [PHYS, PHYS_END), _purecall
// sits inside it, and EXE is somewhere else entirely.
static const unsigned __int64 PHYS     = 0x00000000F2A50000ULL;
static const unsigned __int64 PHYS_END = PHYS + 0x00500000ULL;
static const unsigned __int64 PURE     = PHYS + 0x47624ULL;
static const unsigned __int64 EXE      = 0x00007FF66D7B0000ULL;

static PhysQueryClass Classify(unsigned __int64 shape, bool vptrRead, unsigned __int64 vptr,
                               bool slotRead, unsigned __int64 slot1,
                               unsigned __int64 purecall = PURE)
{
	return ClassifyPhysQueryEntry(shape, vptrRead, vptr, slotRead, slot1,
	                              PHYS, PHYS_END, purecall);
}

int main()
{
	// --- the arithmetic screen, polarity pinned both ways ---
	Check(PhysQueryAddressPlausible(0x155950860ULL), "an aligned heap pointer is plausible");
	Check(!PhysQueryAddressPlausible(0), "NULL is not");
	Check(!PhysQueryAddressPlausible(0x155950864ULL), "a misaligned pointer is not");
	Check(!PhysQueryAddressPlausible(0xFFFF800000000000ULL), "a non-canonical pointer is not");

	// --- one test per rejection arm, and the accept ---
	Check(Classify(0, true, PHYS + 0x3B40D0, true, PHYS + 0x100) == PHYSQ_REJ_ADDR,
	      "a NULL entry is addr, whatever the reads say");
	Check(Classify(0x155950864ULL, true, PHYS + 0x3B40D0, true, PHYS + 0x100) == PHYSQ_REJ_ADDR,
	      "a misaligned entry is addr");
	Check(Classify(0x155950860ULL, false, 0, false, 0) == PHYSQ_REJ_UNREAD,
	      "an unreadable vptr is unread");
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B3F90, false, 0) == PHYSQ_REJ_UNREAD,
	      "an unreadable slot 1 is unread");
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B3F90, true, PURE) == PHYSQ_REJ_PURE,
	      "the abstract vtable's _purecall slot 1 is pure");
	Check(Classify(0x155950860ULL, true, EXE + 0x1000, true, PHYS + 0x100) == PHYSQ_REJ_FOREIGN_VPTR,
	      "a vptr outside PhysX is fvptr");
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B40D0, true, EXE + 0x1000) == PHYSQ_REJ_FOREIGN_SLOT,
	      "a slot 1 outside PhysX is fslot");
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B40D0, true, PHYS + 0x16D770) == PHYSQ_OK,
	      "a concrete vtable with a real slot 1 is accepted");

	// The pure test is ordered before the slot range test: _purecall is
	// itself inside PhysX, so a range-first order would accept this crash.
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B3F90, true, PURE) != PHYSQ_OK,
	      "the residue case is never accepted");
	// With _purecall unconfirmed the same entry falls through to the range
	// tests, which cannot see it -- the documented coverage loss, not a
	// silent one.
	Check(Classify(0x155950860ULL, true, PHYS + 0x3B3F90, true, PURE, 0) == PHYSQ_OK,
	      "purecall=0 disables the pure test rather than misclassifying");

	// Only the two foreign arms can have rejected a live shape.
	Check(PhysQueryClassIsForeign(PHYSQ_REJ_FOREIGN_VPTR), "fvptr is foreign");
	Check(PhysQueryClassIsForeign(PHYSQ_REJ_FOREIGN_SLOT), "fslot is foreign");
	Check(!PhysQueryClassIsForeign(PHYSQ_REJ_PURE), "pure is not foreign");
	Check(!PhysQueryClassIsForeign(PHYSQ_REJ_UNREAD), "unread is not foreign");
	Check(!PhysQueryClassIsForeign(PHYSQ_REJ_ADDR), "addr is not foreign");
	Check(!PhysQueryClassIsForeign(PHYSQ_OK), "ok is not foreign");

	// --- site bytes: exact compare, no detour allowance ---
	{
		unsigned char patched[8];
		memcpy(patched, kPhysQuerySiteBlock, 8);
		patched[2] = 0xE9;   // what an E9-head detour would leave at the site
		Check(!PhysQueryBytesMatch(patched, kPhysQuerySiteBlock, kPhysQuerySiteBlockLen),
		      "an E9 head at the site is a mismatch, not a shared site");
		Check(PhysQueryBytesMatch(kPhysQuerySiteBlock, kPhysQuerySiteBlock, kPhysQuerySiteBlockLen),
		      "the expected block matches itself");
	}
	Check(kPhysQuerySiteBlock[2] == 0x48 && kPhysQuerySiteBlock[3] == 0x8B
	      && kPhysQuerySiteBlock[4] == 0x01 && kPhysQuerySiteBlock[5] == 0xFF
	      && kPhysQuerySiteBlock[6] == 0x50 && kPhysQuerySiteBlock[7] == 0x08,
	      "the block's last six bytes are the site's own 48 8B 01 FF 50 08");

	// --- rel32 reach ---
	Check(PhysQueryRel32Reaches(0x140000000ULL, 0x140800000ULL), "a nearby target reaches");
	Check(!PhysQueryRel32Reaches(0x00007FF66D7B0000ULL, 0x00007FFEA23F0000ULL),
	      "exe to mod DLL does not reach, which is why the stub needs its own page");

	// --- the stub ---
	{
		const unsigned __int64 stub = 0x140000000ULL + 0x10000000ULL;
		const unsigned __int64 slot = stub - 0x1000ULL;
		const unsigned __int64 resume = 0x1407859F8ULL;
		const unsigned __int64 skip   = 0x140785A40ULL;
		unsigned char buf[0x40];
		size_t len = 0;
		Check(BuildPhysQueryStub(buf, sizeof(buf), stub, slot, resume, skip, &len)
		      && len == kPhysQueryStubLen, "the stub assembles");

		// The guard call goes through the slot, so the slot can be repointed
		// without touching executable memory.
		Check(buf[0x09] == 0xFF && buf[0x0A] == 0x15, "the gate call is indirect");
		int d = 0; memcpy(&d, buf + 0x0B, 4);
		Check((unsigned __int64)((__int64)(stub + 0x0F) + d) == slot,
		      "the gate call's displacement resolves to the slot");

		// Both return targets, which is the check a diff cannot make.
		Check(buf[0x22] == 0xE9, "the accept path ends in a jmp");
		memcpy(&d, buf + 0x23, 4);
		Check((unsigned __int64)((__int64)(stub + 0x27) + d) == resume,
		      "the accept path resolves to the instruction after the call");
		Check(buf[0x2B] == 0xE9, "the skip path ends in a jmp");
		memcpy(&d, buf + 0x2C, 4);
		Check((unsigned __int64)((__int64)(stub + 0x30) + d) == skip,
		      "the skip path resolves to the loop's next-entry label");

		// The original two instructions are re-issued verbatim, after rsp is
		// restored to exactly its vanilla value.
		Check(buf[0x18] == 0x48 && buf[0x19] == 0x83 && buf[0x1A] == 0xC4 && buf[0x1B] == 0x30,
		      "the accept path restores rsp before the call");
		Check(buf[0x1C] == 0x48 && buf[0x1D] == 0x8B && buf[0x1E] == 0x01
		      && buf[0x1F] == 0xFF && buf[0x20] == 0x50 && buf[0x21] == 0x08,
		      "the accept path re-issues the original mov/call");
		Check(buf[0x27] == 0x48 && buf[0x28] == 0x83 && buf[0x29] == 0xC4 && buf[0x2A] == 0x30,
		      "the skip path restores rsp too");

		// The conditional branch lands on the skip path, not past it.
		Check(buf[0x16] == 0x74 && (0x18 + (int)(signed char)buf[0x17]) == 0x27,
		      "the reject branch targets the skip path");

		Check(!BuildPhysQueryStub(buf, 4, stub, slot, resume, skip, &len),
		      "a buffer too small is refused");
		Check(!BuildPhysQueryStub(buf, sizeof(buf), stub, slot,
		                          stub + 0x100000000ULL, skip, &len),
		      "an unreachable return target is refused, not truncated");
	}

	{
		unsigned char acc[8];
		Check(BuildPhysQueryAcceptAll(acc, sizeof(acc)), "the accept-all thunk assembles");
		Check(acc[0] == 0xB8 && acc[1] == 1 && acc[5] == 0xC3, "it is mov eax,1 / ret");
	}

	// --- the site patch, as one aligned qword ---
	{
		const unsigned __int64 site = 0x1407859F2ULL;
		const unsigned __int64 stub = 0x150000000ULL;
		unsigned __int64 q = 0;
		Check(BuildPhysQuerySiteQword(kPhysQuerySiteBlock, site, stub, &q),
		      "the site qword builds");
		unsigned char b[8];
		memcpy(b, &q, 8);
		Check(b[0] == kPhysQuerySiteBlock[0] && b[1] == kPhysQuerySiteBlock[1],
		      "the two bytes ahead of the site are preserved");
		Check(b[2] == 0xE9 && b[7] == 0x90, "the site becomes E9 rel32 + nop");
		int d = 0; memcpy(&d, b + 3, 4);
		Check((unsigned __int64)((__int64)(site + 5) + d) == stub,
		      "the site jump resolves to the stub");

		unsigned char wrong[8];
		memcpy(wrong, kPhysQuerySiteBlock, 8);
		wrong[7] = 0x09;
		Check(!BuildPhysQuerySiteQword(wrong, site, stub, &q),
		      "a block that does not match is refused");
		Check(!BuildPhysQuerySiteQword(kPhysQuerySiteBlock, site, 0x00007FFEA23F0000ULL, &q),
		      "a stub out of rel32 reach is refused");
	}

	// --- deferred-arm retry ---
	Check(PhysQueryRetryDue(0.0, -1.0), "the first attempt is always due");
	Check(!PhysQueryRetryDue(10.0, 9.5), "a retry within the interval is not due");
	Check(PhysQueryRetryDue(10.0, 9.0), "a retry past the interval is due");
	Check(!PhysQueryRetryExpired(10.0, 5.0), "the window has not expired at 5s");
	Check(PhysQueryRetryExpired(70.0, 5.0), "the window has expired at 65s");
	Check(!PhysQueryRetryExpired(70.0, -1.0), "no first attempt means not expired");

	return CheckExit("physx_query_guard_units");
}
