#include "fixes/stitch/stitch_byte_guard_policy.h"
#include <string.h>

// Read from the IDB (kenshi_x64.exe, Steam 1.0.65) 2026-09-22, from
// NavMeshGenerator::update (0x3C8B70..0x3C96D1).
const unsigned __int64 kStitchByteSiteRva   = 0x3C911E;
const unsigned __int64 kStitchByteResumeRva = 0x3C9126;
const unsigned __int64 kStitchByteBranchRva = 0x3C8D67;

const unsigned char kStitchByteSiteBytes[8] =
	{ 0xC6, 0x46, 0x50, 0x00, 0x48, 0x8B, 0x46, 0x08 };
const unsigned char kStitchByteResumeBytes[5] =
	{ 0x44, 0x89, 0x44, 0x24, 0x20 };
const unsigned char kStitchByteBranchBytes[10] =
	{ 0x41, 0x83, 0xFF, 0x03, 0x0F, 0x85, 0xAD, 0x03, 0x00, 0x00 };

bool StitchByteBytesMatch(const unsigned char* actual, const unsigned char* expect, int len)
{
	if (!actual || !expect || len <= 0)
		return false;
	return memcmp(actual, expect, (size_t)len) == 0;
}


StitchByteClass ClassifyStitchByteWrite(int type, bool uidRead, unsigned int uid)
{
	if (type != kStitchByteStitchType)
		return SBG_IN_OBJECT;
	if (!uidRead)
		return SBG_STITCH_UNKNOWN;
	return uid < kStitchByteSectorUidLimit ? SBG_STITCH_SECTOR : SBG_STITCH_INTERIOR;
}

bool StitchByteSkipsWrite(StitchByteClass cls, bool actMode)
{
	return actMode && cls == SBG_STITCH_INTERIOR;
}


bool StitchByteRel32Reaches(unsigned __int64 from, unsigned __int64 to)
{
	__int64 delta = (__int64)to - (__int64)from;
	return delta >= (__int64)-0x80000000LL && delta <= (__int64)0x7FFFFFFFLL;
}

static void PutRel32(unsigned char* at, unsigned __int64 nextIp, unsigned __int64 target)
{
	int rel = (int)((__int64)target - (__int64)nextIp);
	memcpy(at, &rel, 4);
}

bool BuildStitchByteStub(unsigned char* out, size_t cap,
                         unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                         unsigned __int64 resumeAddr, size_t* outLen)
{
	if (!out || cap < kStitchByteStubLen)
		return false;
	if (!StitchByteRel32Reaches(stubAddr + 0x1B, gateSlotAddr)
	 || !StitchByteRel32Reaches(stubAddr + 0x3B, resumeAddr))
		return false;

	static const unsigned char kTemplate[kStitchByteStubLen] =
	{
		0x50,                                     // 0x00 push rax
		0x51,                                     // 0x01 push rcx
		0x52,                                     // 0x02 push rdx
		0x41, 0x50,                               // 0x03 push r8
		0x41, 0x51,                               // 0x05 push r9
		0x41, 0x52,                               // 0x07 push r10
		0x41, 0x53,                               // 0x09 push r11
		0x48, 0x83, 0xEC, 0x28,                   // 0x0B sub  rsp, 28h
		0x48, 0x8B, 0xCE,                         // 0x0F mov  rcx, rsi
		0x41, 0x8B, 0xD7,                         // 0x12 mov  edx, r15d
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,       // 0x15 call qword ptr [rip+d]
		0x48, 0x83, 0xC4, 0x28,                   // 0x1B add  rsp, 28h
		0x85, 0xC0,                               // 0x1F test eax, eax
		0x41, 0x5B,                               // 0x21 pop  r11
		0x41, 0x5A,                               // 0x23 pop  r10
		0x41, 0x59,                               // 0x25 pop  r9
		0x41, 0x58,                               // 0x27 pop  r8
		0x5A,                                     // 0x29 pop  rdx
		0x59,                                     // 0x2A pop  rcx
		0x58,                                     // 0x2B pop  rax
		0x74, 0x04,                               // 0x2C jz   .skip (0x32)
		0xC6, 0x46, 0x50, 0x00,                   // 0x2E mov  byte ptr [rsi+50h], 0
		0x48, 0x8B, 0x46, 0x08,                   // 0x32 mov  rax, [rsi+8]
		0xE9, 0x00, 0x00, 0x00, 0x00              // 0x36 jmp  resume
	};
	memcpy(out, kTemplate, kStitchByteStubLen);

	PutRel32(out + 0x17, stubAddr + 0x1B, gateSlotAddr);
	PutRel32(out + 0x37, stubAddr + 0x3B, resumeAddr);

	if (outLen)
		*outLen = kStitchByteStubLen;
	return true;
}

bool BuildStitchByteKeepAll(unsigned char* out, size_t cap)
{
	if (!out || cap < kStitchByteKeepAllLen)
		return false;
	static const unsigned char kBytes[kStitchByteKeepAllLen] =
		{ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };   // mov eax,1 / ret
	memcpy(out, kBytes, kStitchByteKeepAllLen);
	return true;
}

bool BuildStitchByteSitePatch(const unsigned char* current,
                              unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                              unsigned char* out8)
{
	if (!current || !out8)
		return false;
	if (!StitchByteBytesMatch(current, kStitchByteSiteBytes, kStitchByteSiteLen))
		return false;
	if (!StitchByteRel32Reaches(siteAddr + 5, stubAddr))
		return false;

	out8[0] = 0xE9;
	PutRel32(out8 + 1, siteAddr + 5, stubAddr);
	out8[5] = 0x90;
	out8[6] = 0x90;
	out8[7] = 0x90;
	return true;
}
