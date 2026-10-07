// light_cache_policy.cpp - the light-level cache's pure rules (light_cache_policy.h).
#include "fixes/world/light_cache_policy.h"
#include "base/hash.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t FloatBits(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

// The cell a coordinate falls in at 1/inv units, as a signed index's bits.
static uint32_t CellBits(float v, float inv)
{
	return (uint32_t)(int32_t)floorf(v * inv);
}

// Spreads every bit of a pointer over the whole word before it is summed.
static uint64_t Mix64(uint64_t v)
{
	v ^= v >> 33;
	v *= 0xFF51AFD7ED558CCDULL;
	v ^= v >> 33;
	v *= 0xC4CEB9FE1A85EC53ULL;
	v ^= v >> 33;
	return v;
}

namespace fixes {

int LightCacheModeOf(int cfgValue)
{
	return (cfgValue == LIGHT_CACHE_SHADOW || cfgValue == LIGHT_CACHE_ON) ? cfgValue : LIGHT_CACHE_OFF;
}

const char* LightCacheModeName(int mode)
{
	switch (LightCacheModeOf(mode))
	{
	case LIGHT_CACHE_SHADOW: return "shadow";
	case LIGHT_CACHE_ON:     return "on";
	default:                 return "off";
	}
}

bool LightCacheAmbientBypass(float ambient)
{
	return !(ambient <= 0.5f);
}

bool LightCacheMakeKey(int kind, const float pos[3], int floor, bool outdoors, float ambient, uint32_t epoch,
                       LightCacheKey* out)
{
	uint32_t c[3];
	for (int i = 0; i < 3; ++i)
	{
		if (!_finite(pos[i]) || fabsf(pos[i]) > LIGHT_CACHE_MAX_COORD)
			return false;
		if (kind == LIGHT_KEY_EXACT)
			c[i] = FloatBits(pos[i]);
		else if (kind == LIGHT_KEY_Q05)
			c[i] = CellBits(pos[i], 2.0f);
		else if (kind == LIGHT_KEY_Q2)
			c[i] = CellBits(pos[i], 0.5f);
		else
			return false;
	}
	out->x        = c[0];
	out->y        = c[1];
	out->z        = c[2];
	out->floor    = (uint32_t)floor;
	out->outdoors = outdoors ? 1u : 0u;
	out->ambient  = FloatBits(ambient);
	out->epoch    = epoch;
	return true;
}

uint32_t LightCacheSlot(const LightCacheKey& k)
{
	return Fnv1a32(&k, sizeof(k)) & (uint32_t)(LIGHT_CACHE_SLOTS - 1);
}

bool LightCacheFind(const LightCacheTable& t, const LightCacheKey& k, uint32_t nowMs, float* value)
{
	const LightCacheEntry& e = t.slot[LightCacheSlot(k)];
	if (!e.used || memcmp(&e.key, &k, sizeof(k)) != 0 || nowMs - e.stampMs >= LIGHT_CACHE_TTL_MS)
		return false;
	memcpy(value, &e.valueBits, sizeof(*value));
	return true;
}

void LightCacheStore(LightCacheTable* t, const LightCacheKey& k, float value, uint32_t nowMs)
{
	LightCacheEntry& e = t->slot[LightCacheSlot(k)];
	e.key = k;
	memcpy(&e.valueBits, &value, sizeof(e.valueBits));
	e.stampMs = nowMs;
	e.used = 1;
}

void LightCacheZonesAdd(LightCacheZones* z, uint64_t zone)
{
	z->sum += Mix64(zone);
	++z->count;
}

uint64_t LightCacheZonesValue(const LightCacheZones& z)
{
	return z.sum ^ Mix64(0x9E3779B97F4A7C15ULL + z.count);
}

bool LightCacheEpochStep(uint64_t fp, uint64_t* lastFp, uint32_t* epoch)
{
	if (fp == *lastFp)
		return false;
	*lastFp = fp;
	++*epoch;
	return true;
}

bool LightCacheMayServe(int mode, bool litEmpty)
{
	return mode == LIGHT_CACHE_ON && litEmpty;
}

bool LightCacheMayStore(int mode, bool litEmptyBefore, bool litEmptyAfter)
{
	return LightCacheModeOf(mode) != LIGHT_CACHE_OFF && litEmptyBefore && litEmptyAfter;
}

bool LightCacheRipTarget(const unsigned char* insn, const unsigned char* opcode, size_t opLen, size_t insnLen,
                         uintptr_t insnAddr, uintptr_t* target)
{
	if (!insn || !opcode || insnLen < opLen + 4 || memcmp(insn, opcode, opLen) != 0)
		return false;
	int32_t disp;
	memcpy(&disp, insn + opLen, sizeof(disp));
	*target = insnAddr + insnLen + (intptr_t)disp;
	return true;
}

void LightCacheShadowCompare(LightCacheStats* s, int kind, float stored, float real)
{
	if (kind < 0 || kind >= LIGHT_KEY_COUNT)
		return;
	++s->would[kind];
	if (FloatBits(stored) != FloatBits(real))
		++s->mismatch[kind];
	float d = fabsf(stored - real);
	if (!(d <= s->maxDiff[kind]))
		s->maxDiff[kind] = d;
}

int LightCacheFormatLine(char* buf, size_t cap, int mode, const LightCacheStats& s)
{
	return _snprintf_s(buf, cap, _TRUNCATE,
		"LightCache: mode=%s calls=%ld bypass=%ld offMain=%ld skipC=%ld skipZ=%ld hit=%ld miss=%ld store=%ld"
		" epoch=%u bumpZone=%ld bumpLoad=%ld bumpMode=%ld wouldExact=%ld wouldQ05=%ld wouldQ2=%ld"
		" mmExact=%ld mmQ05=%ld mmQ2=%ld diffExact=%.4g diffQ05=%.4g diffQ2=%.4g",
		LightCacheModeName(mode), s.calls, s.bypass, s.offMain, s.skipC, s.skipZ, s.hit, s.miss, s.store,
		(unsigned)s.epoch, s.bumpZone, s.bumpLoad, s.bumpMode,
		s.would[LIGHT_KEY_EXACT], s.would[LIGHT_KEY_Q05], s.would[LIGHT_KEY_Q2],
		s.mismatch[LIGHT_KEY_EXACT], s.mismatch[LIGHT_KEY_Q05], s.mismatch[LIGHT_KEY_Q2],
		(double)s.maxDiff[LIGHT_KEY_EXACT], (double)s.maxDiff[LIGHT_KEY_Q05], (double)s.maxDiff[LIGHT_KEY_Q2]);
}

} // namespace fixes
