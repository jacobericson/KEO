#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "diag/physx_pool_probe.h"

#ifdef KEO_DEBUG

#include "diag/physx_pool_sets.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

namespace physx_pool_probe_detail
{

typedef __int64 (__fastcall *loadPhysXResource_t)(const void*, unsigned int);
static loadPhysXResource_t orig_loadPhysXResource = NULL;

// VS2010 x64 std::string: _Bx +0 (buffer or pointer), _Mysize +16, _Myres +24.
const size_t OFF_STR_SIZE = 16;
const size_t OFF_STR_RES  = 24;
const size_t STR_SSO_CAP  = 16;

// PhysFileParams, the first argument at the loadPhysXFile call site: the
// filename string sits at +0 (which is why that caller can pass the struct
// where a std::string is expected) and the instance scale at +116. Only read
// when the return address proves that caller, since the other three pass a
// bare std::string with nothing behind it.
const size_t OFF_PARAMS_SCALE = 116;

// A name long enough to be a path and then some; anything past this is hashed
// by its first bytes plus its length, which cannot merge two shorter names.
const size_t MAX_NAME_BYTES = 256;

// Resolved once at install, read-only afterwards. The classifier compares
// against these instead of calling the loader from the pass-through.
static unsigned __int64 s_exeBase = 0;
static unsigned __int64 s_exeSize = 0;

// Any loadPhysXResource thread publishes counters and role masks with
// individual Interlocked updates; main PhysXPoolTick reads them separately.
// No coherent set is copied, so mixed diagnostic epochs are tolerated.
// Initialized before hook install and cumulative for the session.
static volatile LONG s_calls[PXP_ROLE_COUNT]    = { 0 };
static volatile LONG s_inflight[PXP_ROLE_COUNT] = { 0 };
static volatile LONG s_tid[PXP_ROLE_COUNT]      = { 0 };   // first thread seen in that role
static volatile LONG s_tidVaried[PXP_ROLE_COUNT]= { 0 };   // a second thread appeared in it

static volatile LONG s_depth        = 0;
static volatile LONG s_maxDepth     = 0;
static volatile LONG s_concurrent   = 0;   // entries that found someone else already inside
static volatile LONG s_pairMask     = 0;   // roles seen inside together, accumulated
static volatile LONG s_maxDepthMask = 0;   // roles inside when the deepest nesting was reached

// The accumulated mask above merges every overlap in the session, so it cannot
// by itself say that one particular pair was ever inside together. These two
// can: per role, how often an entry found company, and how often it found
// company of its own role. Two navmesh-side entrants -- the reading that would
// mean the generator's own serialisation is not what the source says -- shows
// up here as a non-zero same-role count and nowhere else.
static volatile LONG s_concurrentByRole[PXP_ROLE_COUNT] = { 0 };
static volatile LONG s_sameRoleOverlap[PXP_ROLE_COUNT]  = { 0 };

static volatile LONG s_scaleUnit    = 0;
static volatile LONG s_scaleNonUnit = 0;

static PhysXPoolSet s_resources;
static PhysXPoolSet s_scales;
static PhysXPoolSet s_pairs;

static bool   s_installed  = false;
static double s_nextBeat   = 0.0;
static LONG   s_lastPrinted = -1;
const double kBeatSeconds = 60.0;


// A successful max CAS is followed by a separate role-mask exchange. The
// mask is diagnostic and not a coherent pair with the maximum: an earlier
// winner can resume and publish its mask after a deeper winner. Below depth
// two no mask is written.
static void RaiseMaxDepth(LONG depth, LONG mask)
{
	for (;;)
	{
		LONG seen = s_maxDepth;
		if (depth <= seen)
			return;
		if (InterlockedCompareExchange(&s_maxDepth, depth, seen) == seen)
		{
			if (depth > 1)
				InterlockedExchange(&s_maxDepthMask, mask);
			return;
		}
	}
}

// Which roles currently have a call in flight. Read without a lock, so it is
// a snapshot of counters that are moving: it names the roles that overlapped,
// which is the question, and makes no claim about the instant it was taken.
static LONG InFlightMask()
{
	LONG mask = 0;
	for (int i = 0; i < PXP_ROLE_COUNT; ++i)
	{
		if (s_inflight[i] > 0)
			mask |= (1L << i);
	}
	return mask;
}

// Reads the argument as a std::string without a fault guard. All four call
// sites pass one by reference -- the physics pair pass a PhysFileParams whose
// first member is that string -- so a bad pointer here would already have
// faulted in the original a few instructions later. DEV builds only.
static unsigned __int64 HashName(const void* stringObj, unsigned int type)
{
	const unsigned char* s = (const unsigned char*)stringObj;
	unsigned __int64 res  = *(const unsigned __int64*)(s + OFF_STR_RES);
	unsigned __int64 size = *(const unsigned __int64*)(s + OFF_STR_SIZE);
	const unsigned char* text = (res >= STR_SSO_CAP)
		? *(const unsigned char* const*)s
		: s;
	if (!text)
		return PhysXPoolHashU32(type, PXP_HASH_SEED);

	size_t take = (size_t)(size < MAX_NAME_BYTES ? size : MAX_NAME_BYTES);
	unsigned __int64 h = PhysXPoolHashU32(type, PXP_HASH_SEED);
	h = PhysXPoolHashU32((unsigned int)size, h);
	return PhysXPoolHashBytes(text, take, h);
}


// The depth and in-flight counters must come back down on every exit,
// including a C++ exception unwinding out of the original -- loadPhysXResource
// has exception states, and a leaked in-flight count would make every later
// call look concurrent. No __try anywhere in the detour, so a destructor is
// allowed here.
struct EntryScope
{
	int role;
	explicit EntryScope(int r) : role(r) {}
	~EntryScope()
	{
		InterlockedDecrement(&s_inflight[role]);
		InterlockedDecrement(&s_depth);
	}
private:
	EntryScope(const EntryScope&);
	EntryScope& operator=(const EntryScope&);
};


static __int64 __fastcall hook_loadPhysXResource(const void* nameString, unsigned int type)
{
	unsigned __int64 ret = (unsigned __int64)_ReturnAddress();
	bool insideExe = (s_exeSize != 0) && (ret >= s_exeBase) && (ret < s_exeBase + s_exeSize);
	int role = PhysXPoolClassifyCaller(insideExe ? ret - s_exeBase : 0, insideExe);

	InterlockedIncrement(&s_calls[role]);

	LONG tid = (LONG)GetCurrentThreadId();
	LONG priorTid = InterlockedCompareExchange(&s_tid[role], tid, 0);
	if (priorTid != 0 && priorTid != tid)
		InterlockedIncrement(&s_tidVaried[role]);

	LONG sameRole = InterlockedIncrement(&s_inflight[role]);
	LONG depth = InterlockedIncrement(&s_depth);
	EntryScope scope(role);

	if (sameRole > 1)
		InterlockedIncrement(&s_sameRoleOverlap[role]);

	LONG mask = 0;
	if (depth > 1)
	{
		mask = InFlightMask();
		InterlockedIncrement(&s_concurrent);
		InterlockedIncrement(&s_concurrentByRole[role]);
		for (;;)
		{
			LONG seen = s_pairMask;
			if ((seen | mask) == seen)
				break;
			if (InterlockedCompareExchange(&s_pairMask, seen | mask, seen) == seen)
				break;
		}
	}
	RaiseMaxDepth(depth, mask);

	if (nameString)
	{
		unsigned __int64 nameHash = HashName(nameString, type);
		PhysXPoolSetInsert(&s_resources, nameHash);

		if (role == PXP_ROLE_PHYS)
		{
			const float* scale = (const float*)((const unsigned char*)nameString + OFF_PARAMS_SCALE);
			float x = scale[0], y = scale[1], z = scale[2];
			if (x == 1.0f && y == 1.0f && z == 1.0f)
				InterlockedIncrement(&s_scaleUnit);
			else
				InterlockedIncrement(&s_scaleNonUnit);
			PhysXPoolSetInsert(&s_scales, PhysXPoolHashScale(x, y, z, PXP_HASH_SEED));
			PhysXPoolSetInsert(&s_pairs,  PhysXPoolHashScale(x, y, z, nameHash));
		}
	}

	return orig_loadPhysXResource(nameString, type);
}


static bool ResolveExeExtent()
{
	s_exeBase = (unsigned __int64)gameBase;
	s_exeSize = 0;
	if (!s_exeBase)
		return false;

	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)s_exeBase;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt =
		(const IMAGE_NT_HEADERS64*)((uintptr_t)s_exeBase + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	s_exeSize = nt->OptionalHeader.SizeOfImage;
	return s_exeSize != 0;
}

} // namespace
using namespace physx_pool_probe_detail;


void InstallPhysXPoolProbe(int* installed, int*)
{
	PhysXPoolSetReset(&s_resources);
	PhysXPoolSetReset(&s_scales);
	PhysXPoolSetReset(&s_pairs);

	const char* why = NULL;
	if (!ResolveExeExtent())
		why = "image extent";
	else
		why = HookInstall(HOOK_LOAD_PHYSX_RESOURCE, hook_loadPhysXResource,
				&orig_loadPhysXResource, installed, true);

	if (!why)
	{
		s_installed = true;
		LogMsg("PhysXPool probe: installed (pass-through; the readout's order= "
		       "field says whether ours is the outermost detour on the site)");
	}
	else
	{
		orig_loadPhysXResource = NULL;
		ErrorLog(std::string("PhysXPool probe: not installed (") + why + ")");
	}
}


void PhysXPoolTick(double now)
{
	if (!s_installed)
		return;
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;

	LONG total = 0;
	for (int i = 0; i < PXP_ROLE_COUNT; ++i)
		total += s_calls[i];

	// Nothing new since the last line and nothing ever seen: say so once and
	// then stay quiet, rather than repeating an empty row every minute.
	if (total == s_lastPrinted)
		return;
	s_lastPrinted = total;

	char resBuf[32], scaleBuf[32], pairBuf[32], depthBuf[32], maskBuf[64], maxMaskBuf[64];
	PhysXPoolFormatSet(resBuf, sizeof(resBuf), &s_resources);
	PhysXPoolFormatSet(scaleBuf, sizeof(scaleBuf), &s_scales);
	PhysXPoolFormatSet(pairBuf, sizeof(pairBuf), &s_pairs);
	PhysXPoolFormatCount(depthBuf, sizeof(depthBuf), s_maxDepth, total > 0);
	PhysXPoolFormatRoleMask(maskBuf, sizeof(maskBuf), (unsigned long)s_pairMask);
	PhysXPoolFormatRoleMask(maxMaskBuf, sizeof(maxMaskBuf), (unsigned long)s_maxDepthMask);

	// Hook order, read from where the calls came from rather than from the
	// install: a call entered from outside the executable can only have come
	// through another module's detour, which would then be answering the cache
	// hits itself and keeping them out of these counts.
	const char* order = (total == 0)
		? "?"
		: (s_calls[PXP_ROLE_FOREIGN] > 0 ? "inner" : "outer");

	// A thread id of 0 means the role never ran, which is a reading of its own
	// and must not be printed as a thread whose id happens to be zero.
	char tidPhys[32], tidNav[32];
	PhysXPoolFormatCount(tidPhys, sizeof(tidPhys), s_tid[PXP_ROLE_PHYS], s_tid[PXP_ROLE_PHYS] != 0);
	PhysXPoolFormatCount(tidNav,  sizeof(tidNav),  s_tid[PXP_ROLE_NAV],  s_tid[PXP_ROLE_NAV]  != 0);

	char line[640];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"PhysXPool: calls=%ld phys=%ld trig=%ld nav=%ld xml=%ld other=%ld foreign=%ld"
		" maxDepth=%s conc=%ld pair=%s deepest=%s"
		" concPhys=%ld concNav=%ld samePhys=%ld sameNav=%ld"
		" res=%s scale=%s resScale=%s unit=%ld nonUnit=%ld order=%s"
		" tid=phys:%s/nav:%s varied=%ld/%ld",
		(long)total,
		(long)s_calls[PXP_ROLE_PHYS], (long)s_calls[PXP_ROLE_TRIG],
		(long)s_calls[PXP_ROLE_NAV], (long)s_calls[PXP_ROLE_XML],
		(long)s_calls[PXP_ROLE_EXE_OTHER], (long)s_calls[PXP_ROLE_FOREIGN],
		depthBuf, (long)s_concurrent, maskBuf, maxMaskBuf,
		(long)s_concurrentByRole[PXP_ROLE_PHYS], (long)s_concurrentByRole[PXP_ROLE_NAV],
		(long)s_sameRoleOverlap[PXP_ROLE_PHYS], (long)s_sameRoleOverlap[PXP_ROLE_NAV],
		resBuf, scaleBuf, pairBuf,
		(long)s_scaleUnit, (long)s_scaleNonUnit, order,
		tidPhys, tidNav,
		(long)s_tidVaried[PXP_ROLE_PHYS], (long)s_tidVaried[PXP_ROLE_NAV]);
	LogMsg(line);
}

#else  // !KEO_DEBUG

void InstallPhysXPoolProbe(int* installed, int*) { (void)installed; }
void PhysXPoolTick(double now) { (void)now; }

#endif // KEO_DEBUG
