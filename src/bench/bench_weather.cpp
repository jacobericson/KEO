#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bench/bench_game.h"
#include "bench/bench_game_internal.h"
#include "bench/bench_game_math.h"
#include "game/game.h"
#include <float.h>
#include <stddef.h>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <kenshi/Weather.h>
#include <kenshi/GameData.h>
#include "base/klib_include_end.h"

// Separate from bench_game.cpp: Weather.h's WeatherRegion clashes with the
// one PhysicsCollection.h (pulled in by the character headers) declares.

static_assert(offsetof(WeatherSystem, ActiveRegionWeather) == 0x0, "WeatherSystem::ActiveRegionWeather");
static_assert(offsetof(WeatherRegion, weatherInstance) == 0x30, "WeatherRegion::weatherInstance");
static_assert(offsetof(WeatherInstance, weather) == 0x8, "WeatherInstance::weather");
static_assert(offsetof(WeatherInstance, strength) == 0x14, "WeatherInstance::strength");
static_assert(offsetof(WeatherInstance, windSpeed) == 0x18, "WeatherInstance::windSpeed");
static_assert(offsetof(Weather, weatherData) == 0x8, "Weather::weatherData");
static_assert(offsetof(GameData, name) == 0x28, "GameData::name");

static WeatherSystem* const* s_weatherSystem = NULL;   // NULL until the game creates it

bool BenchWeatherInstall()
{
	static const unsigned char MOV_RAX_RIP[] = { 0x48, 0x8B, 0x05 };
	s_weatherSystem = NULL;
	if (!BenchSameAddress((const void*)KlibRealAddress(&WeatherSystem::getInstance),
	                      RVA_WEATHER_GET_INSTANCE, "WeatherSystem::getInstance") ||
	    !BenchCheckAnchor(RVA_WEATHER_GET_INSTANCE, 0x47, MOV_RAX_RIP, sizeof(MOV_RAX_RIP),
	                      RVA_WEATHER_INSTANCE, "WeatherSystem instance"))
		return false;
	s_weatherSystem = (WeatherSystem* const*)(gameBase + RVA_WEATHER_INSTANCE);
	return true;
}

// The camera biome's weather: the region WeatherSystem marks active.
std::string BenchWeatherText()
{
	if (!s_weatherSystem)
		return "unknown";
	WeatherSystem* ws = *s_weatherSystem;
	if (!BenchPlausible(ws) || !BenchPlausible(ws->ActiveRegionWeather))
		return "unknown";
	WeatherInstance* wi = ws->ActiveRegionWeather->weatherInstance;
	if (!BenchPlausible(wi))
		return "unknown";
	std::string name;
	Weather* w = wi->weather;
	if (BenchPlausible(w) && BenchPlausible(w->weatherData) && w->weatherData->name.size() < 128)
		name = w->weatherData->name;
	float strength = _finite(wi->strength) ? wi->strength : 0.0f;
	float wind = _finite(wi->windSpeed) ? wi->windSpeed : 0.0f;
	return FormatBenchWeather(name, strength, wind);
}
