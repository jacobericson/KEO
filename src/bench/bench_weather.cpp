#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bench/bench_game.h"
#include "bench/bench_game_internal.h"
#include "bench/bench_game_math.h"
#include "bench/bench_pin.h"
#include "game/game.h"
#include "base/core.h"
#include <float.h>
#include <stddef.h>
#include <stdio.h>
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
// The weather pin's members. The library's classes are not standard-layout, so
// clang flags offsetof on them; the offsets pinned here are the MSVC layout the
// game was built with, which is the one that matters.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(WeatherRegion, currentSeason) == 0x38, "WeatherRegion::currentSeason");
static_assert(offsetof(WeatherRegion, currentSeasonEndDay) == 0x44, "WeatherRegion::currentSeasonEndDay");
static_assert(offsetof(WeatherRegion, requestUpdateEffects) == 0x69, "WeatherRegion::requestUpdateEffects");
static_assert(offsetof(WeatherRegion, weatherUpdated) == 0xB1, "WeatherRegion::weatherUpdated");
static_assert(offsetof(Season, weathers) == 0x8 && offsetof(Season, hasConditionalWeather) == 0x20, "Season::weathers");
static_assert(offsetof(Weather, windSpeedMin) == 0x30, "Weather::windSpeedMin");
static_assert(offsetof(Weather, windSpeedMax) == 0x34, "Weather::windSpeedMax");
static_assert(offsetof(Weather, conditionStart) == 0x7C, "Weather::conditionStart");
static_assert(offsetof(Weather, conditionEnd) == 0x80, "Weather::conditionEnd");
static_assert(offsetof(WeatherInstance, windBuildUpSpeedStart) == 0x34, "WeatherInstance::windBuildUpSpeedStart");
static_assert(offsetof(WeatherInstance, windBuildUpSpeedEnd) == 0x38, "WeatherInstance::windBuildUpSpeedEnd");
static_assert(offsetof(WeatherInstance, endTimeMinutes) == 0x48, "WeatherInstance::endTimeMinutes");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static WeatherSystem* const* s_weatherSystem = NULL;   // NULL until the game creates it
static bool                  s_setupReady    = false;  // setupWeather is where the game calls it

static const unsigned MAX_SEASON_WEATHERS = 256;
static const int      KEEP_END_AHEAD_MIN  = 120;   // game minutes the held weather's end stays ahead
static const int      KEEP_SEASON_DAYS    = 2;     // days the held season's end stays ahead

bool BenchWeatherInstall()
{
	static const unsigned char MOV_RAX_RIP[] = { 0x48, 0x8B, 0x05 };
	s_weatherSystem = NULL;
	s_setupReady = false;
	if (!BenchSameAddress((const void*)KlibRealAddress(&WeatherSystem::getInstance),
	                      RVA_WEATHER_GET_INSTANCE, "WeatherSystem::getInstance") ||
	    !BenchCheckAnchor(RVA_WEATHER_GET_INSTANCE, 0x47, MOV_RAX_RIP, sizeof(MOV_RAX_RIP),
	                      RVA_WEATHER_INSTANCE, "WeatherSystem instance"))
		return false;
	s_weatherSystem = (WeatherSystem* const*)(gameBase + RVA_WEATHER_INSTANCE);
	s_setupReady = BenchSameAddress((const void*)KlibRealAddress(&WeatherInstance::setupWeather), RVA_WEATHER_SETUP,
	                                "WeatherInstance::setupWeather");
	return true;
}

// The camera biome's region and its instance; NULL when either is not plausible.
static WeatherRegion* ActiveRegion(WeatherInstance** instance)
{
	if (!s_weatherSystem)
		return NULL;
	WeatherSystem* ws = *s_weatherSystem;
	if (!BenchPlausible(ws) || !BenchPlausible(ws->ActiveRegionWeather))
		return NULL;
	WeatherRegion* region = ws->ActiveRegionWeather;
	if (!BenchPlausible(region->weatherInstance))
		return NULL;
	*instance = region->weatherInstance;
	return region;
}

static std::string WeatherName(const Weather* w)
{
	std::string name;
	if (BenchPlausible(w) && BenchPlausible(w->weatherData) && w->weatherData->name.size() < 128)
		name = w->weatherData->name;
	for (size_t i = 0; i < name.size(); ++i)
	{
		if (name[i] == ' ')
			name[i] = '_';
	}
	return name;
}

bool BenchWeatherSnapshot(BenchWeatherHold* out)
{
	WeatherInstance* wi = NULL;
	WeatherRegion* region = ActiveRegion(&wi);
	if (!region || !out)
		return false;
	out->region = region;
	out->weather = wi->weather;
	out->strength = wi->strength;
	return true;
}

bool BenchWeatherKeep(const BenchWeatherHold& h)
{
	WeatherInstance* wi = NULL;
	WeatherRegion* region = ActiveRegion(&wi);
	if (!IsMainThread() || !region || region != h.region || wi->weather != h.weather)
		return false;
	// The back thread rolls a new weather once the clock's minute passes the
	// end, or the sky's day reaches the season's end; it is joined while the
	// bench ticks, so these stores race nothing.
	double hours = BenchGameHoursTotal();
	if (hours >= 0.0)
	{
		int now = (int)(hours * 60.0);
		if (wi->endTimeMinutes < now + KEEP_END_AHEAD_MIN / 2)
			wi->endTimeMinutes = now + KEEP_END_AHEAD_MIN;
	}
	int day = BenchGetDay();
	if (day >= 0 && region->currentSeasonEndDay < day + KEEP_SEASON_DAYS)
		region->currentSeasonEndDay = day + KEEP_SEASON_DAYS;
	return true;
}

static bool HourInside(float a, float b, float h)
{
	if (a == b)
		return true;
	return a < b ? h >= a && h < b : h >= a || h < b;
}

static char s_forceWhy[128];

static const char* Refuse(const char* fmt, const char* name, float a = 0.0f, float b = 0.0f)
{
	_snprintf_s(s_forceWhy, sizeof(s_forceWhy), _TRUNCATE, fmt, name, a, b);
	return s_forceWhy;
}

const char* BenchForceWeather(const char* name, float strength, std::string* names)
{
	WeatherInstance* wi = NULL;
	WeatherRegion* region = ActiveRegion(&wi);
	if (!IsMainThread() || !s_setupReady || !region || !name)
		return "no weather";
	Season* season = region->currentSeason;
	if (!BenchPlausible(season))
		return "no weather";
	unsigned n = season->weathers.size();
	Weather** list = season->weathers.begin();
	if (n > MAX_SEASON_WEATHERS || (n && !BenchPlausible(list)))
		return "no weather";

	Weather* w = NULL;
	std::string all;
	for (unsigned i = 0; i < n; ++i)
	{
		std::string candidate = WeatherName(list[i]);
		if (candidate.empty())
			continue;
		all += (all.empty() ? "" : ", ") + candidate;
		if (!w && BenchWeatherNameMatches(name, candidate))
			w = list[i];
	}
	if (!w)
	{
		if (names)
			*names = all;
		return Refuse("weather '%s' not in this region's season", name);
	}
	float hour = BenchGetHour();
	if (hour >= 0.0f && !HourInside(w->conditionStart, w->conditionEnd, hour))
		return Refuse("weather '%s' needs hours %.0f-%.0f", name, w->conditionStart, w->conditionEnd);

	std::string was = BenchWeatherText();
	bool forced = wi->weather != w;
	if (forced)
	{
		// What the back thread's own roll does after its setup: the listeners
		// hear it from the main-thread update later this frame, and the effects
		// follow on the next back-thread pass.
		wi->setupWeather(w);
		region->requestUpdateEffects = true;
		region->weatherUpdated = true;
	}
	if (strength >= 0.0f && strength <= 1.0f)
	{
		// The setup's own wind formula, written to the segment's start and end
		// too so the per-frame interpolation keeps it.
		float wind = w->windSpeedMin + (w->windSpeedMax - w->windSpeedMin) * strength;
		wi->strength = strength;
		wi->windSpeed = wind;
		wi->windBuildUpSpeedStart = wind;
		wi->windBuildUpSpeedEnd = wind;
	}
	BenchWeatherHold h = { region, w, wi->strength };
	BenchWeatherKeep(h);

	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	char line[384];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "Bench: weather %s %s strength=%.2f (was %s) qpc=%lld",
	            forced ? "forced" : "kept", WeatherName(w).c_str(), wi->strength, was.c_str(), q.QuadPart);
	LogMsg(line);
	return NULL;
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
