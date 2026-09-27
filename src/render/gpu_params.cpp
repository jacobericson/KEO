#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/module_hooks.h"
#include "render/gpu_param_cache.h"
#include "game/klib_member_contract.h"
#include "base/core.h"

// Ogre::GpuProgramParameters::getConstantDefinition, a string-keyed map
// lookup the D3D11 render system calls once per bound constant while setting
// up a pass. Patched through the D3D11 DLL's own import slot for the OgreMain
// export, rather than a code hook, since the call sites are in the D3D11 DLL
// (three `call [rip+X]` through the IAT slot) and only the callee itself is
// in OgreMain.
//
// The cache maps (params' GpuNamedConstants, name object) to the definition
// the original returned. Each name is a string owned by the D3D11 program's
// constant-buffer description, so its address is stable while the program
// lives; a definition is a map node's value, stable while its
// GpuNamedConstants lives. Every GpuNamedConstants is deleted through one
// OgreMain function, hooked below to drop its entries first.

#ifdef ZONEOPT_DEBUG
static const bool DEV_BUILD = true;
#else
static const bool DEV_BUILD = false;
#endif

typedef const void* (*GetConstDef_t)(const void* params, const void* name);
typedef void        (*SetNamedConstants_t)(void* params, const void* namedConstantsPtr);
typedef void*       (*DeleteNamedConstants_t)(void* namedConstants, int flags);

static GetConstDef_t          s_origGetConstDef = NULL;
static SetNamedConstants_t    s_origSetNamed    = NULL;
static DeleteNamedConstants_t s_origDelete      = NULL;
static int                    s_patchState      = -1;    // -1 untried, 0 refused, 1 patched
static int                    s_watchState      = -1;    // the two invalidation hooks: -1 untried, 0 refused, 1 live
static bool                   s_cacheHooked     = false; // invalidation hooks and patch both live

// Counts GpuNamedConstants deletions, raised before the object is freed.
static volatile LONG s_namedConstantsDeleted = 0;

// Main thread only, apart from s_flushPending: another thread that deletes or
// replaces a GpuNamedConstants cannot touch the table, so it asks for a full
// clear before the next lookup instead.
static GpuParamCache s_cache;
static volatile LONG s_flushPending = 0;

static const char* const D3D11_MODULE = "RenderSystem_Direct3D11_x64.dll";
static const char* const OGRE_MODULE  = "OgreMain_x64.dll";
static const char* const GET_CONST_DEF_SYMBOL =
	"?getConstantDefinition@GpuProgramParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z";

static const ModuleSite s_setNamedSite =
{
	"GpuProgramParameters::_setNamedConstants", "OgreMain_x64.dll",
	"?_setNamedConstants@GpuProgramParameters@Ogre@@QEAAXAEBV?$SharedPtr@UGpuNamedConstants@Ogre@@@2@@Z",
	0xE89F0,
	{ 0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x81,0xC1 }
};

// GpuNamedConstants' deleting destructor (this, flags), not exported. Both
// deleters a GpuNamedConstantsPtr is created with end here.
static const ModuleSite s_deleteSite =
{
	"GpuNamedConstants deleting destructor", "OgreMain_x64.dll", NULL,
	0xDAC30,
	{ 0x40,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,0x48 }
};

static const void* NamedConstantsOf(const void* params)
{
	return *(const void* const*)((const char*)params + KLIB_OFF_GpuProgramParameters_namedConstants);
}

// Any thread.
static void InvalidateNamedConstants(const void* namedConstants)
{
	if (!namedConstants)
		return;
	if (IsMainThread())
		s_cache.DropMap(namedConstants);
	else
		InterlockedExchange(&s_flushPending, 1);
}

// Main thread: no logging, allocation or lock. The original throws when
// the name is missing; nothing is stored then, and this frame holds nothing
// to unwind. The hit and miss counts are touched only on this thread.
static const void* CachedLookup(const void* params, const void* name)
{
	if (!IsMainThread())
	{
		InterlockedIncrement(&g_renderStats.lookupOffThread);
		return s_origGetConstDef(params, name);
	}
	// The map is read before the flush check: a map freed and reallocated at
	// the same address was freed before params could point at it, so its
	// flush request is already visible here.
	const void* map = NamedConstantsOf(params);
	if (s_flushPending)
	{
		InterlockedExchange(&s_flushPending, 0);
		s_cache.Clear();
	}
	if (!map)
		return s_origGetConstDef(params, name);
	const MsvcString* key = (const MsvcString*)name;
	const void* def = s_cache.Find(map, key);
	if (def)
	{
		++g_renderStats.lookupHits;
		return def;
	}
	def = s_origGetConstDef(params, name);
	++g_renderStats.lookupMisses;
	if (def)
		s_cache.Store(map, key, def);
	return def;
}

static const void* Lookup(const void* params, const void* name)
{
	if (s_cacheHooked && g_renderCfg.gpuParamCache)
		return CachedLookup(params, name);
	return s_origGetConstDef(params, name);
}

static const void* hook_GetConstantDefinition(const void* params, const void* name)
{
	if (!DEV_BUILD || !g_renderCfg.gpuParamLookupDiag)
		return Lookup(params, name);
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);
	const void* r = Lookup(params, name);
	QueryPerformanceCounter(&t1);
	InterlockedExchangeAdd64(&g_renderStats.lookupTicks, t1.QuadPart - t0.QuadPart);
	InterlockedIncrement(&g_renderStats.lookupCalls);
	return r;
}

// Whichever thread sets up the params: its current map is about to be
// replaced, so that map's entries go first. A map rewritten in place by
// GpuProgram::setManualNamedConstants keeps its address and is not
// invalidated.
static void hook_SetNamedConstants(void* params, const void* namedConstantsPtr)
{
	InvalidateNamedConstants(NamedConstantsOf(params));
	s_origSetNamed(params, namedConstantsPtr);
}

// Whichever thread drops the last reference: entries go before the nodes do.
static void* hook_DeleteNamedConstants(void* namedConstants, int flags)
{
	InterlockedIncrement(&s_namedConstantsDeleted);
	InvalidateNamedConstants(namedConstants);
	return s_origDelete(namedConstants, flags);
}

// Idempotent: the diagnostic and the cache share the one patch.
static bool PatchLookupSlot()
{
	if (s_patchState >= 0)
		return s_patchState == 1;
	s_patchState = 0;
	HMODULE d3d = GetModuleHandleA(D3D11_MODULE);
	HMODULE ogre = GetModuleHandleA(OGRE_MODULE);
	if (!d3d || !ogre)
	{
		LogMsg("Render: gpuParams: RenderSystem_Direct3D11_x64.dll or OgreMain_x64.dll not loaded");
		return false;
	}
	void* current = (void*)GetProcAddress(ogre, GET_CONST_DEF_SYMBOL);
	if (!current)
	{
		LogMsg("Render: gpuParams: OgreMain export missing: getConstantDefinition");
		return false;
	}
	// Set before the patch, not after: the CAS guarantees prev == current on
	// success, so the hook (which can fire the instant the slot is swapped)
	// never sees a NULL original.
	s_origGetConstDef = (GetConstDef_t)current;
	// PatchImportSlot's compare-and-swap refuses (returns NULL) when the slot
	// isn't found, or when it no longer holds the OgreMain export -- another
	// plugin (ReShade, say) patched it first. Either way this never chains
	// onto an unknown function.
	void* prev = PatchImportSlot(d3d, OGRE_MODULE, GET_CONST_DEF_SYMBOL,
	                              (void*)&hook_GetConstantDefinition, current);
	if (!prev)
	{
		s_origGetConstDef = NULL;
		LogMsg("Render: gpuParams: import slot missing or already patched, not installed");
		return false;
	}
	s_patchState = 1;
	return true;
}

bool InstallGpuParamsDiag()
{
	return PatchLookupSlot();
}

bool InstallGpuNamedConstantsWatch()
{
	if (s_watchState >= 0)
		return s_watchState == 1;
	s_watchState = 0;
	if (!InstallModuleHook(s_deleteSite, (void*)&hook_DeleteNamedConstants, (void**)&s_origDelete, NULL))
		return false;
	if (!InstallModuleHook(s_setNamedSite, (void*)&hook_SetNamedConstants, (void**)&s_origSetNamed, NULL))
		return false;
	s_watchState = 1;
	return true;
}

LONG GpuNamedConstantsDeleted()
{
	return s_namedConstantsDeleted;
}

// The invalidation hooks go in before the cache can store anything, so no
// entry outlives its map unseen.
bool InstallGpuParamCache()
{
	if (s_cacheHooked)
		return true;
	if (!InstallGpuNamedConstantsWatch())
		return false;
	if (!PatchLookupSlot())
		return false;
	s_cacheHooked = true;
	return true;
}

bool GpuParamCacheActive()
{
	return s_cacheHooked && g_renderCfg.gpuParamCache;
}

// Main thread: zeroes the diagnostic's counters when it is switched off, and
// empties the table when the cache is, so switching it back on starts clean.
void GpuParams_MainThreadTick()
{
	static int diagWasOn = -1;   // -1 before the first tick
	static int cacheWasOn = -1;
	if (s_patchState != 1)
		return;
	int diagOn = g_renderCfg.gpuParamLookupDiag ? 1 : 0;
	if (diagOn != diagWasOn)
	{
		if (diagWasOn == 1)
		{
			InterlockedExchange(&g_renderStats.lookupCalls, 0);
			InterlockedExchange64(&g_renderStats.lookupTicks, 0);
		}
		diagWasOn = diagOn;
	}
	int cacheOn = GpuParamCacheActive() ? 1 : 0;
	if (cacheOn != cacheWasOn)
	{
		if (cacheWasOn == 1)
			s_cache.Clear();
		cacheWasOn = cacheOn;
	}
}
