#include "fixes/world/town_claim_policy.h"
#include <string.h>

bool TownClaimFlagged(const TownClaimInputs& in)
{
	return in.depthSet && in.ownerIsPlayer && !in.isFoliage && !in.hasFurnitureOf
	    && !in.hasDoorOf && !in.hasIndoorsOf && !in.hasSaveState;
}

bool TownClaimNeedsTown(const TownClaimInputs& in)
{
	return TownClaimFlagged(in)
	    && (in.townIsNull
	        || (!in.townIsPlayerTown && in.hasSnapTarget && !in.snapTargetIsPlayerOwned));
}

TownClaimAction TownClaimDecide(const TownClaimInputs& in)
{
	if (!TownClaimNeedsTown(in))
		return TC_PASS;
	return in.haveContainingPlayerTown ? TC_USE_CONTAINING : TC_USE_NULL_TOWN;
}

int TownClaimPickContaining(const TownClaimCandidate* c, int n, float px, float pz)
{
	if (!c || n <= 0)
		return -1;
	int best = -1;
	float bestD2 = 0.0f;
	for (int i = 0; i < n; ++i)
	{
		const float r = c[i].radius;
		if (!(r > 0.0f))
			continue;
		const float dx = c[i].x - px;
		const float dz = c[i].z - pz;
		const float d2 = dx * dx + dz * dz;
		if (d2 > r * r)
			continue;
		if (best < 0 || d2 < bestD2)
		{
			best = i;
			bestD2 = d2;
		}
	}
	return best;
}


// Read from the IDB (kenshi_x64.exe, Steam 1.0.65), RootObjectFactory::createBuilding
// (0x57C1E0).
const unsigned __int64 kTownClaimLeadRva   = 0x57C9AC;
const unsigned __int64 kTownClaimSiteRva   = 0x57C9B1;
const unsigned __int64 kTownClaimResumeRva = 0x57C9BE;
const unsigned __int64 kTownClaimKeepRva   = 0x57CC7E;

const unsigned char kTownClaimLeadBytes[5] =
	{ 0x48, 0x8B, 0x7C, 0x24, 0x78 };
const unsigned char kTownClaimSiteBytes[13] =
	{ 0x80, 0xBF, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x84, 0xC0, 0x02, 0x00, 0x00 };
const unsigned char kTownClaimResumeBytes[7] =
	{ 0x80, 0xBD, 0x78, 0x04, 0x00, 0x00, 0x00 };
const unsigned char kTownClaimKeepBytes[11] =
	{ 0x48, 0x85, 0xDB, 0x0F, 0x85, 0x48, 0x02, 0x00, 0x00, 0xEB, 0x05 };

bool TownClaimBytesMatch(const unsigned char* actual, const unsigned char* expect, int len)
{
	if (!actual || !expect || len <= 0)
		return false;
	return memcmp(actual, expect, (size_t)len) == 0;
}

bool TownClaimRel32Reaches(unsigned __int64 from, unsigned __int64 to)
{
	__int64 delta = (__int64)to - (__int64)from;
	return delta >= (__int64)-0x80000000LL && delta <= (__int64)0x7FFFFFFFLL;
}

static void PutRel32(unsigned char* at, unsigned __int64 nextIp, unsigned __int64 target)
{
	int rel = (int)((__int64)target - (__int64)nextIp);
	memcpy(at, &rel, 4);
}

bool BuildTownClaimStub(unsigned char* out, size_t cap,
                        unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                        unsigned __int64 keepAddr, unsigned __int64 resumeAddr, size_t* outLen)
{
	if (!out || cap < kTownClaimStubLen)
		return false;
	if (!TownClaimRel32Reaches(stubAddr + 0x0D, keepAddr)
	 || !TownClaimRel32Reaches(stubAddr + 0x22, gateSlotAddr)
	 || !TownClaimRel32Reaches(stubAddr + 0x39, keepAddr)
	 || !TownClaimRel32Reaches(stubAddr + 0x3E, resumeAddr))
		return false;

	static const unsigned char kTemplate[kTownClaimStubLen] =
	{
		0x80, 0xBF, 0xA8, 0x00, 0x00, 0x00, 0x00, // 0x00 cmp  byte ptr [rdi+0A8h], 0
		0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,       // 0x07 je   keep           (rel32 at 0x09)
		0x50,                                     // 0x0D push rax
		0x51,                                     // 0x0E push rcx
		0x52,                                     // 0x0F push rdx
		0x41, 0x50,                               // 0x10 push r8
		0x41, 0x51,                               // 0x12 push r9
		0x41, 0x52,                               // 0x14 push r10
		0x41, 0x53,                               // 0x16 push r11
		0x48, 0x83, 0xEC, 0x28,                   // 0x18 sub  rsp, 28h
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,       // 0x1C call qword ptr [gate]  (rel32 at 0x1E)
		0x48, 0x83, 0xC4, 0x28,                   // 0x22 add  rsp, 28h
		0x85, 0xC0,                               // 0x26 test eax, eax
		0x41, 0x5B,                               // 0x28 pop  r11
		0x41, 0x5A,                               // 0x2A pop  r10
		0x41, 0x59,                               // 0x2C pop  r9
		0x41, 0x58,                               // 0x2E pop  r8
		0x5A,                                     // 0x30 pop  rdx
		0x59,                                     // 0x31 pop  rcx
		0x58,                                     // 0x32 pop  rax
		0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,       // 0x33 jnz  keep           (rel32 at 0x35)
		0xE9, 0x00, 0x00, 0x00, 0x00              // 0x39 jmp  resume         (rel32 at 0x3A)
	};
	memcpy(out, kTemplate, kTownClaimStubLen);

	PutRel32(out + 0x09, stubAddr + 0x0D, keepAddr);
	PutRel32(out + 0x1E, stubAddr + 0x22, gateSlotAddr);
	PutRel32(out + 0x35, stubAddr + 0x39, keepAddr);
	PutRel32(out + 0x3A, stubAddr + 0x3E, resumeAddr);

	if (outLen)
		*outLen = kTownClaimStubLen;
	return true;
}

bool BuildTownClaimVanillaGate(unsigned char* out, size_t cap)
{
	if (!out || cap < kTownClaimVanillaGateLen)
		return false;
	static const unsigned char kBytes[kTownClaimVanillaGateLen] =
		{ 0x33, 0xC0, 0xC3 };   // xor eax,eax / ret
	memcpy(out, kBytes, kTownClaimVanillaGateLen);
	return true;
}

bool BuildTownClaimSitePatch(const unsigned char* current,
                             unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                             unsigned char* out13)
{
	if (!current || !out13)
		return false;
	if (!TownClaimBytesMatch(current, kTownClaimSiteBytes, kTownClaimSiteLen))
		return false;
	if (!TownClaimRel32Reaches(siteAddr + 5, stubAddr))
		return false;

	out13[0] = 0xE9;
	PutRel32(out13 + 1, siteAddr + 5, stubAddr);
	for (int i = 5; i < kTownClaimSiteLen; ++i)
		out13[i] = 0x90;
	return true;
}
