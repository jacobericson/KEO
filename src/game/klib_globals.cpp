#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "game/klib_bindings.h"
#include "base/klib_include.h"
#include <core/Functions.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/KingOfRenderThread.h>
#include <kenshi/SharedKing.h>
// WeatherRegion is independently defined by PhysicsCollection.h, so keep
// Weather.h here, away from Character/PhysicsActual header dependencies.
#include <kenshi/Weather.h>
#include "base/klib_include_end.h"
static_assert(offsetof(KingOfRenderThread, render) == 8, "render storage");
static_assert(sizeof(((KingOfRenderThread*)0)->render) == 8, "render pointer width");
static_assert(offsetof(SharedKing, townList) == 8, "townList storage");
static_assert(sizeof(((SharedKing*)0)->townList) == 8, "townList pointer width");
// These are storage addresses. The game-owned pointers inside them are read
// by callers each time, preserving pointer-vs-object and reload semantics.
uintptr_t KlibGlobalAddress(uintptr_t rva)
{
	switch (rva)
	{
	case 0x21330B0: return (uintptr_t)ou;
	case 0x21330C8: return ou ? (uintptr_t)&ou->physics : 0;
	case 0x2133560: return ou ? (uintptr_t)&ou->navmesh : 0;
	case 0x2133630: return ou ? (uintptr_t)&ou->player : 0;
	case 0x2132440: return (uintptr_t)options;
	case 0x21322B0: return (uintptr_t)au;
	case 0x21322B8: return au ? (uintptr_t)&au->render : 0;
	case 0x2133098: return (uintptr_t)shou;
	case 0x21330A0: return shou ? (uintptr_t)&shou->townList : 0;
	default: return 0;
	}
}
uintptr_t KlibWeatherAddress(bool mainThread)
{
	return mainThread ? (uintptr_t)KlibRealAddress(&WeatherSystem::updateMT)
	                  : (uintptr_t)KlibRealAddress(&WeatherSystem::updateBT);
}
