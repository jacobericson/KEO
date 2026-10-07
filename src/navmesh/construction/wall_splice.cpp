// wall_splice.cpp - The wall progress detour that records a crossing, its install, and the
// call-site NOP that removes vanilla's early splice. The drain is wall_splice_drain.cpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "navmesh/construction/wall_splice.h"
#include "navmesh/construction/wall_splice_internal.h"
#include "navmesh/construction/splice_queue_policy.h"
#include "navmesh/navmesh_config.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <string.h>
#include <string>
#include "base/klib_include.h"
#include <Debug.h>
#include "base/klib_include_end.h"

namespace navmesh {

WallSpliceState g_wallSplice;

// Read from the IDB 2026-09-30 (WallBuilding::addConstructionProgress 0x5594A0):
// shareBuildStateOfAnother.type +0x370; _buildState progress +0x164, totalMats +0x188,
// pathThreshold +0x194; physical +0x228; othersSharingMyBuildState count +0x390, items +0x398
// (one hand per 0x20, type at +8, BUILDING = 0); getAABB vtable +0xF0.
static const size_t kShareType = 0x370, kProgress = 0x164, kTotalMats = 0x188, kThreshold = 0x194;
static const size_t kPhysical = 0x228, kSharingCount = 0x390, kSharingItems = 0x398;
static const size_t kHandStride = 0x20, kHandType = 0x8, kVtGetAabb = 0xF0;

typedef void        (__fastcall *wallAddProgress_t)(void* wall, float amount);
typedef const float* (__fastcall *getAabb_t)(void* object);
typedef void*       (__fastcall *zoneHandleGetObject_t)(void* list, const void* h);
static wallAddProgress_t     orig_wallAddProgress = NULL;
static zoneHandleGetObject_t fn_zoneHandleGetObject = NULL;
static void*                 s_zoneHandleList = NULL;

// The three callees' first bytes in this build: the row installs only when all three match.
static const unsigned char kQueuesAreClearHead[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x83,0xB9,0xB0,0x01,0x00,0x00,0x00,0x48,0x8B,0xD9 };
static const unsigned char kGenerateAabbHead[16] =
	{ 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24 };
static const unsigned char kZoneHandleGetObjectHead[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x01,0x48,0x8B,0xDA,0xFF,0x50,0x20,0x48 };

static const float* ObjectAabb(void* object)
{
	return ((getAabb_t)(*(void***)object)[kVtGetAabb / 8])(object);
}

// The box vanilla's splice would have covered: the wall's AABB merged with every sharing wall's,
// through the same calls vanilla makes in this function on this thread.
static void WallBox(void* wall, float box[6])
{
	const float* a = ObjectAabb(wall);
	for (int i = 0; i < 6; ++i) box[i] = a[i];
	const unsigned char* w = (const unsigned char*)wall;
	unsigned n = *(const unsigned*)(w + kSharingCount);
	const unsigned char* items = *(const unsigned char* const*)(w + kSharingItems);
	for (unsigned i = 0; items && i < n; ++i)
	{
		const unsigned char* h = items + i * kHandStride;
		if (*(const int*)(h + kHandType) != 0)
			continue;
		void* other = fn_zoneHandleGetObject(s_zoneHandleList, h);
		if (other)
			SpliceBoxMerge(box, ObjectAabb(other));
	}
}

// AI back thread or main thread. Vanilla's progress, then one ring record when vanilla's crossing
// block ran (its group-switch push is then already made, on this thread). No lock, no allocation,
// no logging.
static void __fastcall hook_wallAddProgress(void* wall, float amount)
{
	const unsigned char* w = (const unsigned char*)wall;
	const int shareType = wall ? *(const int*)(w + kShareType) : -1;
	const bool below = shareType == SPLICE_HAND_NULL_ITEM
		&& SpliceBelow(*(const float*)(w + kProgress), *(const float*)(w + kTotalMats),
		               *(const float*)(w + kThreshold));
	orig_wallAddProgress(wall, amount);
	if (!below || !InterlockedCompareExchange(&g_wallSplice.armed, 0, 0))
		return;
	if (!SpliceRecordDue(shareType, below, *(const float*)(w + kProgress), *(const float*)(w + kTotalMats),
	                     *(const float*)(w + kThreshold), amount,
	                     *(void* const*)(w + kPhysical) != NULL))
		return;
	float box[6];
	WallBox(wall, box);
	SpliceRingPush(&g_wallSplice.ring, box);
	InterlockedIncrement(&g_wallSplice.recorded);
}

void InstallWallSplice(int* installed, int*)
{
	if (!HookRowWanted(HOOK_WALL_ADD_PROGRESS)) return;

	const char* why = NULL;
	if (memcmp((const void*)GameAddr(RVA_ZONEMAP_HANDLE_GET_OBJECT), kZoneHandleGetObjectHead, 16) != 0)
		why = "getObject";
	else if (memcmp((const void*)GameAddr(RVA_QUEUES_ARE_CLEAR_MT), kQueuesAreClearHead, 16) != 0)
		why = "queuesAreClearMT";
	else if (memcmp((const void*)GameAddr(RVA_NAVMESH_GENERATE_AABB), kGenerateAabbHead, 16) != 0)
		why = "generate";
	else
	{
		// Before the install: the detour calls getObject as soon as it is in.
		SpliceRingInit(&g_wallSplice.ring);
		fn_zoneHandleGetObject = (zoneHandleGetObject_t)GameAddr(RVA_ZONEMAP_HANDLE_GET_OBJECT);
		s_zoneHandleList = GameAddr(RVA_ZONEMAP_HANDLE_LIST);
		g_wallSplice.fn_queuesAreClearMT = (queuesAreClearMT_t)GameAddr(RVA_QUEUES_ARE_CLEAR_MT);
		g_wallSplice.fn_navMeshGenerateAabb = (navMeshGenerateAabb_t)GameAddr(RVA_NAVMESH_GENERATE_AABB);
		why = HookInstall(HOOK_WALL_ADD_PROGRESS, hook_wallAddProgress, &orig_wallAddProgress, installed, true);
	}

	if (!why)
	{
		InterlockedExchange(&g_wallSplice.detourInstalled, 1);
		return;
	}
	// A head mismatch installs nothing, so the NOP is never written either.
	orig_wallAddProgress = NULL;
	ErrorLog(std::string("WallSplice: not installed (") + why + "); walls splice as vanilla");
}

// POD-only, for the __try: MSVC 2010 rejects one in a function holding an object to unwind.
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

// The five bytes at the call, written while no thread can be inside them: no world, so no wall
// progresses yet. Returns the refusal, or NULL once the NOP is in.
static const char* WriteCallSiteNop()
{
	if (*(void* const*)GameAddr(RVA_GLOBAL_SECTION_MGR) != NULL)
		return "a world already exists";
	unsigned char lead[kWallSpliceLeadLen];
	if (!SafeReadBytes(GameAddr((size_t)kWallSpliceLeadRva), lead, sizeof(lead)))
		return "the call site is unreadable";
	if (!WallSpliceLeadMatches(lead))
		return "the call site's bytes differ";

	void* call = GameAddr((size_t)kWallSpliceCallRva);
	DWORD oldProtect = 0;
	if (!VirtualProtect(call, sizeof(kWallSpliceNop), PAGE_EXECUTE_READWRITE, &oldProtect))
		return "VirtualProtect on the call site failed";
	memcpy(call, kWallSpliceNop, sizeof(kWallSpliceNop));
	DWORD ignore = 0;
	VirtualProtect(call, sizeof(kWallSpliceNop), oldProtect, &ignore);
	FlushInstructionCache(GetCurrentProcess(), call, sizeof(kWallSpliceNop));
	return NULL;
}

void InstallWallSpliceNop(bool gateOk)
{
	if (!g_navmeshCfg.wallSpliceFixEnabled) return;

	const char* why = NULL;
	if (!gateOk)
		why = "the build gate refused";
	else if (!InterlockedCompareExchange(&g_wallSplice.detourInstalled, 0, 0))
		why = "the progress detour is not installed";
	else
		why = WriteCallSiteNop();

	if (why)
	{
		LogMsg(std::string("WallSplice: not armed (") + why + "); walls splice as vanilla");
		return;
	}
	InterlockedExchange(&g_wallSplice.armed, SpliceArmed(true, true) ? 1 : 0);
	LogMsg("WallSplice: armed (progress detour and call-site NOP; a heartbeat line follows in any minute a counter moved)");
}

} // namespace navmesh
