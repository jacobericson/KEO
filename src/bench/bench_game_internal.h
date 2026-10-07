#pragma once
#include <stddef.h>
#include <stdint.h>

// Shared by the facade's translation units (bench_game.cpp, bench_weather.cpp).

inline bool BenchPlausible(const void* p)
{
	uintptr_t v = (uintptr_t)p;
	return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 7) == 0;
}

// KenshiLib's address for a covered function equals ours (game base + rva).
bool BenchSameAddress(const void* klibAddr, size_t rva, const char* name);

// The function at fnRva holds, at insnOff, opLen opcode bytes whose disp32
// operand addresses game base + expectRva.
bool BenchCheckAnchor(size_t fnRva, size_t insnOff, const unsigned char* opcode, size_t opLen,
                      size_t expectRva, const char* name);

// Weather readout (bench_weather.cpp). False leaves BenchWeatherText at
// "unknown"; the rest of the facade does not depend on it.
bool BenchWeatherInstall();

// The sky's total game hours (day * 24 + hour); negative when unknown.
double BenchGameHoursTotal();
