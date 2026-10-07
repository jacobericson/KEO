#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/light_cache.h"

#ifdef KEO_DEBUG

#include "fixes/world/light_cache_policy.h"
#include "fixes/fixes_config.h"
#include "game/game.h"
#include "base/clock.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

typedef float (*GetLightLevel_t)(void* world, const float* position, int floor, bool outdoors);
typedef float (*AmbientDayFactor_t)(void* sky);

static GetLightLevel_t    orig_getLightLevel = NULL;
static AmbientDayFactor_t s_ambientDayFactor = NULL;
static bool               s_installed = false;

// The mode the detour reads; the tick publishes it.
static volatile LONG s_mode = fixes::LIGHT_CACHE_OFF;
// Calls on another thread: the one counter written off the main thread.
static volatile LONG s_offMain = 0;

// Main thread only: the detour and the tick both run there.
static fixes::LightCacheTable s_tables[fixes::LIGHT_KEY_COUNT];
static fixes::LightCacheStats s_stats;
static uint64_t s_lastZones = 0;
static uint32_t s_epoch = 1;
static int      s_lastMode = fixes::LIGHT_CACHE_OFF;
static bool     s_lastSaveLoading = false;
static double   s_lastBeat = 0.0;

static const double LIGHT_CACHE_BEAT_SECONDS = 60.0;

static uint32_t NowMs()
{
	return (uint32_t)(unsigned long long)QpcToMs(QpcNow() - pluginStartTime.QuadPart);
}

static bool LitSetEmpty()
{
	const char* world = (const char*)GameAddr(RVA_GLOBAL_GAMEWORLD);
	return *(const unsigned long long*)(world + fixes::LIGHT_CACHE_LIT_SET + OFF_SET_SIZE) == 0;
}

static void FoldZone(void* ctx, void* zone)
{
	fixes::LightCacheZonesAdd((fixes::LightCacheZones*)ctx, (uint64_t)(uintptr_t)zone);
}

// Moves the epoch when the loaded-cell set changed since the last call. False
// when the set could not be read.
static bool ZonesCurrent()
{
	void* zoneMgr = *(void* const*)KLIB_MEMBER(5, GameAddr(RVA_GLOBAL_GAMEWORLD), GameWorld_zoneMgr, 0x8B0);
	fixes::LightCacheZones z = { 0, 0 };
	if (ZoneSetBVisit(zoneMgr, &FoldZone, &z) < 0)
		return false;
	if (fixes::LightCacheEpochStep(fixes::LightCacheZonesValue(z), &s_lastZones, &s_epoch))
		++s_stats.bumpZone;
	return true;
}

// shadow or on, main thread.
static float Cached(int mode, void* world, const float* pos, int floor, bool outdoors)
{
	++s_stats.calls;
	void* sky = *(void* const*)GameAddr(RVA_SKY_INSTANCE);
	float ambient = sky ? s_ambientDayFactor(sky) : 1.0f;
	if (!pos || fixes::LightCacheAmbientBypass(ambient))
	{
		++s_stats.bypass;
		return orig_getLightLevel(world, pos, floor, outdoors);
	}
	if (!ZonesCurrent())
	{
		++s_stats.skipZ;
		return orig_getLightLevel(world, pos, floor, outdoors);
	}
	int kinds = mode == fixes::LIGHT_CACHE_SHADOW ? (int)fixes::LIGHT_KEY_COUNT : 1;
	fixes::LightCacheKey key[fixes::LIGHT_KEY_COUNT];
	for (int i = 0; i < kinds; ++i)
	{
		if (!fixes::LightCacheMakeKey(i, pos, floor, outdoors, ambient, s_epoch, &key[i]))
		{
			++s_stats.bypass;
			return orig_getLightLevel(world, pos, floor, outdoors);
		}
	}
	bool litEmpty = LitSetEmpty();
	if (!litEmpty)
	{
		++s_stats.skipC;
		return orig_getLightLevel(world, pos, floor, outdoors);
	}
	uint32_t now = NowMs();
	float value = 0.0f;
	if (fixes::LightCacheMayServe(mode, litEmpty)
	    && fixes::LightCacheFind(s_tables[fixes::LIGHT_KEY_EXACT], key[fixes::LIGHT_KEY_EXACT], now, &value))
	{
		++s_stats.hit;
		return value;
	}
	float real = orig_getLightLevel(world, pos, floor, outdoors);
	++s_stats.miss;
	for (int i = 0; mode == fixes::LIGHT_CACHE_SHADOW && i < kinds; ++i)
	{
		if (fixes::LightCacheFind(s_tables[i], key[i], now, &value))
			fixes::LightCacheShadowCompare(&s_stats, i, value, real);
	}
	if (fixes::LightCacheMayStore(mode, litEmpty, LitSetEmpty()))
	{
		for (int i = 0; i < kinds; ++i)
			fixes::LightCacheStore(&s_tables[i], key[i], real, now);
		++s_stats.store;
	}
	return real;
}

static float hook_getLightLevel(void* world, const float* pos, int floor, bool outdoors)
{
	int mode = (int)s_mode;
	if (mode == fixes::LIGHT_CACHE_OFF)
		return orig_getLightLevel(world, pos, floor, outdoors);
	if (!IsMainThread())
	{
		InterlockedIncrement(&s_offMain);
		return orig_getLightLevel(world, pos, floor, outdoors);
	}
	return Cached(mode, world, pos, floor, outdoors);
}

// The three instructions the cache relies on, decoded from this build's
// bytes before the detour goes in: the sky object the ambient factor reads,
// the ambient factor itself, and the lit-character set's size. NULL when all
// three match, else the one that does not.
static const char* CheckAnchors()
{
	static const unsigned char MOV_RCX_RIP[] = { 0x48, 0x8B, 0x0D };
	static const unsigned char CALL_REL[]    = { 0xE8 };
	static const unsigned char CMP_RIP_0[]   = { 0x48, 0x83, 0x3D };
	uintptr_t fn = (uintptr_t)GameAddr(RVA_GET_LIGHT_LEVEL);
	const unsigned char* code = (const unsigned char*)fn;
	uintptr_t t = 0;
	if (!fixes::LightCacheRipTarget(code + fixes::LIGHT_CACHE_SKY_LOAD, MOV_RCX_RIP, 3, 7,
	                                fn + fixes::LIGHT_CACHE_SKY_LOAD, &t)
	    || t != (uintptr_t)GameAddr(RVA_SKY_INSTANCE))
		return "skyLoad";
	if (!fixes::LightCacheRipTarget(code + fixes::LIGHT_CACHE_AMBIENT_CALL, CALL_REL, 1, 5,
	                                fn + fixes::LIGHT_CACHE_AMBIENT_CALL, &t)
	    || t != (uintptr_t)GameAddr(RVA_AMBIENT_DAY_FACTOR))
		return "ambientCall";
	if (!fixes::LightCacheRipTarget(code + fixes::LIGHT_CACHE_LIT_TEST, CMP_RIP_0, 3, 8,
	                                fn + fixes::LIGHT_CACHE_LIT_TEST, &t)
	    || code[fixes::LIGHT_CACHE_LIT_TEST + 7] != 0x00
	    || t != (uintptr_t)GameAddr(RVA_GLOBAL_GAMEWORLD) + fixes::LIGHT_CACHE_LIT_SET + OFF_SET_SIZE)
		return "litTest";
	return NULL;
}

void InstallLightCache(int* installed, int*)
{
	const char* why = CheckAnchors();
	if (!why)
	{
		s_ambientDayFactor = (AmbientDayFactor_t)GameAddr(RVA_AMBIENT_DAY_FACTOR);
		why = HookInstall(HOOK_GET_LIGHT_LEVEL, hook_getLightLevel, &orig_getLightLevel, installed, true);
	}
	if (!why)
	{
		s_installed = true;
		LogMsg("LightCache: install=ok");
	}
	else
	{
		orig_getLightLevel = NULL;
		ErrorLog(std::string("LightCache: install=refused(") + why + ")");
	}
}

static void Beat(double now, int mode)
{
	s_lastBeat = now;
	s_stats.offMain = (long)InterlockedCompareExchange(&s_offMain, 0, 0);
	s_stats.epoch = s_epoch;
	char line[512];
	fixes::LightCacheFormatLine(line, sizeof(line), mode, s_stats);
	LogMsg(line);
}

void LightCacheTick(double now, bool saveLoading)
{
	if (!s_installed)
		return;
	if (saveLoading && !s_lastSaveLoading)
	{
		++s_epoch;
		++s_stats.bumpLoad;
	}
	s_lastSaveLoading = saveLoading;
	int mode = fixes::LightCacheModeOf(fixes::g_fixesCfg.cfg_lightCache);
	if (mode != s_lastMode)
	{
		s_lastMode = mode;
		InterlockedExchange(&s_mode, (LONG)mode);
		++s_epoch;
		++s_stats.bumpMode;
		Beat(now, mode);
		return;
	}
	if (now - s_lastBeat >= LIGHT_CACHE_BEAT_SECONDS)
		Beat(now, mode);
}

#else  // !KEO_DEBUG

void InstallLightCache(int* installed, int*) { (void)installed; }
void LightCacheTick(double now, bool saveLoading) { (void)now; (void)saveLoading; }

#endif // KEO_DEBUG
