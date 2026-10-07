#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/town_claim.h"
#include "fixes/world/town_claim_policy.h"
#include "fixes/near_page.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include "base/config.h"
#include <windows.h>
#include <string>

// 0 = not attempted, 1 = armed, -1 = refused.
static int s_state = 0;

// The data page holds the gate slot; the code page holds the stub at +0 and the vanilla thunk
// at +0x40. Never freed once the site is patched: the main thread can be inside the stub.
static unsigned char* s_page     = NULL;
static unsigned char* s_codePage = NULL;
static const size_t kDataPageSize     = 0x1000;
static const size_t kCodePageSize     = 0x1000;
static const size_t kVanillaGateOffset = 0x40;

// SEH-guarded copy. Standalone and POD-only: MSVC 2010 rejects __try in a function that also
// holds an object needing unwinding.
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

static bool ReadGroup(unsigned __int64 rva, unsigned char* out, int len)
{
	return SafeReadBytes((const void*)(uintptr_t)((unsigned __int64)gameBase + rva), out, (size_t)len);
}

// Main thread, startPlugin. The thirteen bytes cannot be written in one atomic store, so the
// patch is applied only while no world exists (pauseState.navmesh is NULL): nothing can be
// running createBuilding yet. Returns NULL when armed, else the reason.
static const char* TryArm()
{
	unsigned __int64 navmesh = 0;
	if (!SafeReadBytes(GameAddr(RVA_GLOBAL_SECTION_MGR), &navmesh, sizeof(navmesh)))
		return "pauseState.navmesh unreadable";
	if (navmesh != 0)
		return "the world already exists; the thirteen bytes cannot be written safely";

	unsigned char lead[kTownClaimLeadLen];
	unsigned char site[kTownClaimSiteLen];
	unsigned char resume[kTownClaimResumeLen];
	unsigned char keep[kTownClaimKeepLen];
	if (!ReadGroup(kTownClaimSiteRva, site, kTownClaimSiteLen)
	 || !ReadGroup(kTownClaimLeadRva, lead, kTownClaimLeadLen)
	 || !ReadGroup(kTownClaimResumeRva, resume, kTownClaimResumeLen)
	 || !ReadGroup(kTownClaimKeepRva, keep, kTownClaimKeepLen))
		return "the site or a neighbour is unreadable";
	if (!TownClaimBytesMatch(site, kTownClaimSiteBytes, kTownClaimSiteLen))
		return "site bytes differ";
	if (!TownClaimBytesMatch(lead, kTownClaimLeadBytes, kTownClaimLeadLen)
	 || !TownClaimBytesMatch(resume, kTownClaimResumeBytes, kTownClaimResumeLen)
	 || !TownClaimBytesMatch(keep, kTownClaimKeepBytes, kTownClaimKeepLen))
		return "the lead, resume or keep bytes differ";

	unsigned char* page = AllocateNearPages((unsigned __int64)gameBase, kDataPageSize + kCodePageSize);
	if (!page)
		return "no free page within rel32 reach of the exe";

	unsigned char* codePage = page + kDataPageSize;
	const unsigned __int64 gateSlotAddr = (unsigned __int64)(uintptr_t)page;
	const unsigned __int64 stubAddr     = (unsigned __int64)(uintptr_t)codePage;
	const unsigned __int64 siteAddr     = (unsigned __int64)gameBase + kTownClaimSiteRva;
	const unsigned __int64 keepAddr     = (unsigned __int64)gameBase + kTownClaimKeepRva;
	const unsigned __int64 resumeAddr   = (unsigned __int64)gameBase + kTownClaimResumeRva;

	unsigned char patch[kTownClaimSiteLen];
	if (!BuildTownClaimStub(codePage, kCodePageSize, stubAddr, gateSlotAddr, keepAddr, resumeAddr, NULL)
	 || !BuildTownClaimVanillaGate(codePage + kVanillaGateOffset, kCodePageSize - kVanillaGateOffset)
	 || !BuildTownClaimSitePatch(site, siteAddr, stubAddr, patch))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		return "the stub's displacements do not reach from the page found";
	}

	*(volatile unsigned __int64*)page = (unsigned __int64)(uintptr_t)&KEO_TownClaimGate;

	DWORD oldProtect = 0;
	if (!VirtualProtect(codePage, kCodePageSize, PAGE_EXECUTE_READ, &oldProtect))
	{
		VirtualFree(page, 0, MEM_RELEASE);
		return "the stub page could not be made executable";
	}
	FlushInstructionCache(GetCurrentProcess(), codePage, kCodePageSize);

	DWORD oldSite = 0;
	if (!VirtualProtect((void*)(uintptr_t)siteAddr, kTownClaimSiteLen, PAGE_EXECUTE_READWRITE, &oldSite))
		return "VirtualProtect on the site failed";   // the page stays; nothing points at it
	memcpy((void*)(uintptr_t)siteAddr, patch, kTownClaimSiteLen);
	DWORD ignore = 0;
	VirtualProtect((void*)(uintptr_t)siteAddr, kTownClaimSiteLen, oldSite, &ignore);
	FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)siteAddr, kTownClaimSiteLen);

	s_page = page;
	s_codePage = codePage;

	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "Town claim: patch armed at exe+"); FlbHexDigits(&o, kTownClaimSiteRva, 6);
	FlbStr(&o, " stub="); FlbHex(&o, stubAddr);
	LogMsg(FlbDone(&o));
	return NULL;
}

namespace fixes {

void InstallTownClaimPatch(bool gateOk)
{
	if (!g_fixesCfg.townClaimFixEnabled) return;
	const char* why = !gateOk ? "the build gate refused"
	                : !TownClaimRowsInstalled() ? "its hooks are not installed"
	                : TryArm();
	if (!why)
	{
		s_state = 1;
		return;
	}
	s_state = -1;
	LogMsg(std::string("Town claim: patch not armed (") + why
	       + "); a first-time zone still re-checks a placement's town");
}

int TownClaimPatchState()
{
	return s_state;
}

void NeutralizeTownClaimPatch()
{
	if (!s_page || !s_codePage)
		return;
	InterlockedExchange64((volatile LONGLONG*)s_page,
		(LONGLONG)(unsigned __int64)(uintptr_t)(s_codePage + kVanillaGateOffset));
	s_page = NULL;
}

} // namespace fixes
