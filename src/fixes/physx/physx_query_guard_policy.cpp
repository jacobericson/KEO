#include "fixes/physx/physx_query_guard_policy.h"
#include <string.h>

// Read from the IDB (kenshi_x64.exe, Steam 1.0.65), from a full
// disassembly of GameWorld::getObjectsWithinBox (0x7857A0..0x785A87).
const unsigned __int64 kPhysQuerySiteRva   = 0x7859F2;
const unsigned __int64 kPhysQueryResumeRva = 0x7859F8;
const unsigned __int64 kPhysQuerySkipRva   = 0x785A40;

// 0x7859F0..0x7859F7: the tail of `mov rcx, [rcx+rsi*8]`, then the six bytes
// the stub replaces (`mov rax,[rcx]` / `call qword ptr [rax+8]`).
const unsigned char kPhysQuerySiteBlock[8] =
	{ 0x0C, 0xF1, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08 };

const int kPhysQueryResumeLen = 4;
const unsigned char kPhysQueryResumeBytes[4] = { 0x48, 0x8B, 0x48, 0x08 };

const int kPhysQuerySkipLen = 6;
const unsigned char kPhysQuerySkipBytes[6] = { 0x48, 0xFF, 0xC6, 0x49, 0x3B, 0xF5 };

bool PhysQueryBytesMatch(const unsigned char* actual, const unsigned char* expect, int len)
{
	if (!actual || !expect || len <= 0)
		return false;
	return memcmp(actual, expect, (size_t)len) == 0;
}


bool PhysQueryClassIsForeign(PhysQueryClass cls)
{
	return cls == PHYSQ_REJ_FOREIGN_VPTR || cls == PHYSQ_REJ_FOREIGN_SLOT;
}

bool PhysQueryAddressPlausible(unsigned __int64 p)
{
	if (p == 0)
		return false;
	if ((p & 7) != 0)
		return false;
	return p < 0x0000800000000000ULL;
}

PhysQueryClass ClassifyPhysQueryEntry(unsigned __int64 shape,
                                      bool vptrRead, unsigned __int64 vptr,
                                      bool slot1Read, unsigned __int64 slot1,
                                      unsigned __int64 physBase, unsigned __int64 physEnd,
                                      unsigned __int64 purecall)
{
	if (!PhysQueryAddressPlausible(shape))
		return PHYSQ_REJ_ADDR;
	if (!vptrRead)
		return PHYSQ_REJ_UNREAD;
	if (vptr < physBase || vptr >= physEnd)
		return PHYSQ_REJ_FOREIGN_VPTR;
	if (!slot1Read)
		return PHYSQ_REJ_UNREAD;

	// Before the range test, not after: _purecall lives inside PhysXCore64,
	// so the range test would accept the abstract vtable this crash leaves
	// behind.
	if (purecall != 0 && slot1 == purecall)
		return PHYSQ_REJ_PURE;

	if (slot1 < physBase || slot1 >= physEnd)
		return PHYSQ_REJ_FOREIGN_SLOT;
	return PHYSQ_OK;
}


bool PhysQueryRel32Reaches(unsigned __int64 from, unsigned __int64 to)
{
	__int64 delta = (__int64)to - (__int64)from;
	return delta >= (__int64)-0x80000000LL && delta <= (__int64)0x7FFFFFFFLL;
}

static void PutRel32(unsigned char* at, unsigned __int64 nextIp, unsigned __int64 target)
{
	int rel = (int)((__int64)target - (__int64)nextIp);
	memcpy(at, &rel, 4);
}

bool BuildPhysQueryStub(unsigned char* out, size_t cap,
                        unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                        unsigned __int64 resumeAddr, unsigned __int64 skipAddr,
                        size_t* outLen)
{
	if (!out || cap < kPhysQueryStubLen)
		return false;
	if (!PhysQueryRel32Reaches(stubAddr + 0x0F, gateSlotAddr)
	 || !PhysQueryRel32Reaches(stubAddr + 0x27, resumeAddr)
	 || !PhysQueryRel32Reaches(stubAddr + 0x30, skipAddr))
		return false;

	static const unsigned char kTemplate[kPhysQueryStubLen] =
	{
		0x48, 0x83, 0xEC, 0x30,                   // 0x00 sub  rsp, 30h
		0x48, 0x89, 0x4C, 0x24, 0x20,             // 0x04 mov  [rsp+20h], rcx
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,       // 0x09 call qword ptr [rip+d]
		0x48, 0x8B, 0x4C, 0x24, 0x20,             // 0x0F mov  rcx, [rsp+20h]
		0x85, 0xC0,                               // 0x14 test eax, eax
		0x74, 0x0F,                               // 0x16 jz   .skip (0x27)
		0x48, 0x83, 0xC4, 0x30,                   // 0x18 add  rsp, 30h
		0x48, 0x8B, 0x01,                         // 0x1C mov  rax, [rcx]
		0xFF, 0x50, 0x08,                         // 0x1F call qword ptr [rax+8]
		0xE9, 0x00, 0x00, 0x00, 0x00,             // 0x22 jmp  resume
		0x48, 0x83, 0xC4, 0x30,                   // 0x27 add  rsp, 30h
		0xE9, 0x00, 0x00, 0x00, 0x00              // 0x2B jmp  skip
	};
	memcpy(out, kTemplate, kPhysQueryStubLen);

	PutRel32(out + 0x0B, stubAddr + 0x0F, gateSlotAddr);
	PutRel32(out + 0x23, stubAddr + 0x27, resumeAddr);
	PutRel32(out + 0x2C, stubAddr + 0x30, skipAddr);

	if (outLen)
		*outLen = kPhysQueryStubLen;
	return true;
}

bool BuildPhysQueryAcceptAll(unsigned char* out, size_t cap)
{
	if (!out || cap < kPhysQueryAcceptAllLen)
		return false;
	static const unsigned char kBytes[kPhysQueryAcceptAllLen] =
		{ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };   // mov eax,1 / ret
	memcpy(out, kBytes, kPhysQueryAcceptAllLen);
	return true;
}

bool BuildPhysQuerySiteQword(const unsigned char* block,
                             unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                             unsigned __int64* outQword)
{
	if (!block || !outQword)
		return false;
	if (!PhysQueryBytesMatch(block, kPhysQuerySiteBlock, kPhysQuerySiteBlockLen))
		return false;
	if (!PhysQueryRel32Reaches(siteAddr + 5, stubAddr))
		return false;

	unsigned char patched[8];
	patched[0] = block[0];
	patched[1] = block[1];
	patched[2] = 0xE9;
	PutRel32(patched + 3, siteAddr + 5, stubAddr);
	patched[7] = 0x90;
	memcpy(outQword, patched, 8);
	return true;
}


const double kPhysQueryRetryIntervalSec = 1.0;
const double kPhysQueryRetryWindowSec   = 60.0;

bool PhysQueryRetryDue(double now, double lastAttempt)
{
	if (lastAttempt < 0.0)
		return true;
	return (now - lastAttempt) >= kPhysQueryRetryIntervalSec;
}

bool PhysQueryRetryExpired(double now, double firstAttempt)
{
	if (firstAttempt < 0.0)
		return false;
	return (now - firstAttempt) >= kPhysQueryRetryWindowSec;
}
