// The light-level cache's pure rules: the modes, the ambient bypass, the keys
// (exact and the two coarse ones), the table and its age limit, the
// loaded-cell fingerprint and the epoch, when a value may be served and
// stored, the code-byte decode against this build's instructions, the shadow
// comparison and the heartbeat line.

#include "fixes/world/light_cache_policy.h"
#include <limits>
#include <string.h>
#include <string>

#include "check.h"

using namespace fixes;

static const char* const SUITE_NAME = "light_cache_units";

static LightCacheTable s_table;

static LightCacheKey Key(int kind, float x, float y, float z, int floor, bool outdoors, float ambient, uint32_t epoch)
{
	LightCacheKey k;
	memset(&k, 0xCD, sizeof(k));
	float p[3] = { x, y, z };
	Check(LightCacheMakeKey(kind, p, floor, outdoors, ambient, epoch, &k), "key: a finite position keys");
	return k;
}

static bool Same(const LightCacheKey& a, const LightCacheKey& b)
{
	return memcmp(&a, &b, sizeof(a)) == 0;
}

static float NextUp(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	++u;
	memcpy(&f, &u, sizeof(f));
	return f;
}

static LightCacheKey Base(uint32_t epoch)
{
	return Key(LIGHT_KEY_EXACT, -78550.25f, 1200.5f, -35457.75f, 2, true, 0.0f, epoch);
}

static void CheckModes()
{
	Check(LightCacheModeOf(0) == LIGHT_CACHE_OFF && LightCacheModeOf(1) == LIGHT_CACHE_SHADOW
	      && LightCacheModeOf(2) == LIGHT_CACHE_ON, "mode: 0, 1 and 2 are off, shadow and on");
	Check(LightCacheModeOf(3) == LIGHT_CACHE_OFF && LightCacheModeOf(-1) == LIGHT_CACHE_OFF
	      && LightCacheModeOf(7) == LIGHT_CACHE_OFF, "mode: any other value is off");
	Check(strcmp(LightCacheModeName(0), "off") == 0 && strcmp(LightCacheModeName(1), "shadow") == 0
	      && strcmp(LightCacheModeName(2), "on") == 0 && strcmp(LightCacheModeName(9), "off") == 0, "mode: names");
}

static void CheckBypass()
{
	Check(!LightCacheAmbientBypass(0.0f), "bypass: full night (ambient 0) walks the lights");
	Check(!LightCacheAmbientBypass(0.5f), "bypass: an ambient of one half still walks the lights");
	Check(LightCacheAmbientBypass(NextUp(0.5f)) && LightCacheAmbientBypass(1.0f),
	      "bypass: above one half the original answers 1.0 at once");
	Check(LightCacheAmbientBypass(std::numeric_limits<float>::quiet_NaN()),
	      "bypass: an ambient that is not a number is never keyed");
}

static void CheckKeys()
{
	LightCacheKey a = Base(7);
	Check(Same(a, Base(7)), "key: the same inputs give the same key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, NextUp(-78550.25f), 1200.5f, -35457.75f, 2, true, 0.0f, 7)), "key: one bit of x changes the key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, -78550.25f, NextUp(1200.5f), -35457.75f, 2, true, 0.0f, 7)), "key: one bit of y changes the key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, -78550.25f, 1200.5f, NextUp(-35457.75f), 2, true, 0.0f, 7)), "key: one bit of z changes the key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, -78550.25f, 1200.5f, -35457.75f, 3, true, 0.0f, 7)), "key: the floor changes the key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, -78550.25f, 1200.5f, -35457.75f, 2, false, 0.0f, 7)), "key: the outdoors flag changes the key");
	Check(!Same(a, Key(LIGHT_KEY_EXACT, -78550.25f, 1200.5f, -35457.75f, 2, true, 0.25f, 7)), "key: the ambient changes the key");
	Check(!Same(a, Base(8)), "key: the epoch changes the key");

	float nan[3] = { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f };
	float inf[3] = { 0.0f, std::numeric_limits<float>::infinity(), 0.0f };
	float distant[3] = { 0.0f, 0.0f, 2.0e8f };
	float ok[3]  = { 1.0f, 2.0f, 3.0f };
	LightCacheKey k = a;
	Check(!LightCacheMakeKey(LIGHT_KEY_EXACT, nan, 0, true, 0.0f, 1, &k) && Same(k, a)
	      && !LightCacheMakeKey(LIGHT_KEY_Q2, inf, 0, true, 0.0f, 1, &k)
	      && !LightCacheMakeKey(LIGHT_KEY_Q05, distant, 0, true, 0.0f, 1, &k),
	      "key: a position that is not finite, or beyond the bound, is not keyed");
	Check(!LightCacheMakeKey(LIGHT_KEY_COUNT, ok, 0, true, 0.0f, 1, &k), "key: an unknown kind is not keyed");

	Check(Same(Key(LIGHT_KEY_Q05, 10.1f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_Q05, 10.4f, 0, 0, 0, true, 0, 1)),
	      "key: 10.1 and 10.4 share a half-unit cell");
	Check(!Same(Key(LIGHT_KEY_Q05, 10.4f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_Q05, 10.6f, 0, 0, 0, true, 0, 1)),
	      "key: 10.4 and 10.6 do not");
	Check(!Same(Key(LIGHT_KEY_Q05, -0.1f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_Q05, 0.1f, 0, 0, 0, true, 0, 1)),
	      "key: a half-unit cell does not straddle zero");
	Check(Same(Key(LIGHT_KEY_Q2, 10.1f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_Q2, 11.9f, 0, 0, 0, true, 0, 1)),
	      "key: 10.1 and 11.9 share a two-unit cell");
	Check(!Same(Key(LIGHT_KEY_Q2, 11.9f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_Q2, 12.1f, 0, 0, 0, true, 0, 1)),
	      "key: 11.9 and 12.1 do not");
	Check(!Same(Key(LIGHT_KEY_EXACT, 10.1f, 0, 0, 0, true, 0, 1), Key(LIGHT_KEY_EXACT, 10.4f, 0, 0, 0, true, 0, 1)),
	      "key: the exact key separates what a cell joins");
	Check(LightCacheSlot(a) < (uint32_t)LIGHT_CACHE_SLOTS && LightCacheSlot(a) == LightCacheSlot(Base(7)),
	      "key: the slot is in range and repeatable");
}

static void CheckTable()
{
	memset(&s_table, 0, sizeof(s_table));
	LightCacheKey k = Base(1);
	float v = -1.0f;
	Check(!LightCacheFind(s_table, k, 1000, &v), "table: an empty table finds nothing");
	LightCacheStore(&s_table, k, 0.4375f, 1000);
	Check(LightCacheFind(s_table, k, 1000, &v) && v == 0.4375f, "table: a stored value comes back exact");
	Check(LightCacheFind(s_table, k, 1000 + LIGHT_CACHE_TTL_MS - 1, &v), "table: an entry 9.999 s old is served");
	Check(!LightCacheFind(s_table, k, 1000 + LIGHT_CACHE_TTL_MS, &v), "table: an entry 10 s old has expired");
	Check(!LightCacheFind(s_table, Base(2), 1000, &v), "table: another epoch misses");

	LightCacheKey clash = k;
	for (uint32_t e = 2; e < 1000000 && Same(clash, k); ++e)
	{
		LightCacheKey c = Base(e);
		if (LightCacheSlot(c) == LightCacheSlot(k))
			clash = c;
	}
	Check(!Same(clash, k), "table: a second key for the same slot exists");
	LightCacheStore(&s_table, clash, 0.75f, 1100);
	Check(LightCacheFind(s_table, clash, 1100, &v) && v == 0.75f && !LightCacheFind(s_table, k, 1100, &v),
	      "table: a store overwrites the slot's other key");

	LightCacheStore(&s_table, k, 0.25f, 0xFFFFF000u);
	Check(LightCacheFind(s_table, k, 0x00000100u, &v) && v == 0.25f, "table: the age survives the clock wrapping");
	float negZero = -0.0f;
	LightCacheStore(&s_table, k, negZero, 2000);
	uint32_t bits = 0;
	Check(LightCacheFind(s_table, k, 2000, &v) && (memcpy(&bits, &v, sizeof(bits)), bits == 0x80000000u),
	      "table: the value's bits come back unchanged");
}

static uint64_t Zones(const uint64_t* p, int n)
{
	LightCacheZones z = { 0, 0 };
	for (int i = 0; i < n; ++i)
		LightCacheZonesAdd(&z, p[i]);
	return LightCacheZonesValue(z);
}

static void CheckZones()
{
	const uint64_t abc[] = { 0x00007FF600001000ULL, 0x00007FF600002000ULL, 0x00007FF600003000ULL };
	const uint64_t cab[] = { 0x00007FF600003000ULL, 0x00007FF600001000ULL, 0x00007FF600002000ULL };
	const uint64_t abd[] = { 0x00007FF600001000ULL, 0x00007FF600002000ULL, 0x00007FF600004000ULL };
	Check(Zones(abc, 3) == Zones(cab, 3), "zones: the fingerprint ignores Set B's list order");
	Check(Zones(abc, 3) != Zones(abc, 2), "zones: a cell leaving changes the fingerprint");
	Check(Zones(abc, 3) != Zones(abd, 3), "zones: a cell replaced by another changes the fingerprint");
	Check(Zones(abc, 0) == Zones(cab, 0) && Zones(abc, 0) != Zones(abc, 1), "zones: the empty set has one fingerprint");
}

static void CheckEpoch()
{
	uint64_t last = 0;
	uint32_t epoch = 1;
	Check(LightCacheEpochStep(5, &last, &epoch) && epoch == 2 && last == 5, "epoch: a new fingerprint advances the epoch");
	Check(!LightCacheEpochStep(5, &last, &epoch) && epoch == 2, "epoch: the same fingerprint keeps it");
	Check(LightCacheEpochStep(6, &last, &epoch) && epoch == 3 && last == 6, "epoch: every change advances it once");
}

static void CheckRules()
{
	Check(LightCacheMayServe(LIGHT_CACHE_ON, true), "serve: on serves while no character is lit");
	Check(!LightCacheMayServe(LIGHT_CACHE_ON, false), "serve: on never serves while a character is lit");
	Check(!LightCacheMayServe(LIGHT_CACHE_SHADOW, true) && !LightCacheMayServe(LIGHT_CACHE_OFF, true),
	      "serve: shadow and off never serve");
	Check(LightCacheMayStore(LIGHT_CACHE_ON, true, true) && LightCacheMayStore(LIGHT_CACHE_SHADOW, true, true),
	      "store: shadow and on store when no character was lit before and after");
	Check(!LightCacheMayStore(LIGHT_CACHE_ON, true, false) && !LightCacheMayStore(LIGHT_CACHE_ON, false, true)
	      && !LightCacheMayStore(LIGHT_CACHE_OFF, true, true),
	      "store: never with a character lit on either side, nor while off");
}

static void CheckAnchors()
{
	static const unsigned char MOV_RCX_RIP[] = { 0x48, 0x8B, 0x0D };
	static const unsigned char CALL_REL[]    = { 0xE8 };
	static const unsigned char CMP_RIP_0[]   = { 0x48, 0x83, 0x3D };
	// This build's three instructions inside GameWorld::getLightLevel.
	static const unsigned char skyLoad[] = { 0x48, 0x8B, 0x0D, 0x3C, 0x53, 0x72, 0x01 };
	static const unsigned char ambCall[] = { 0xE8, 0xC7, 0xA0, 0x60, 0xFF };
	static const unsigned char litTest[] = { 0x48, 0x83, 0x3D, 0x46, 0x93, 0x72, 0x01, 0x00 };
	const uintptr_t base = (uintptr_t)0x140000000ULL;
	const uintptr_t fn = base + 0xA0A040;
	uintptr_t t = 0;
	Check(LightCacheRipTarget(skyLoad, MOV_RCX_RIP, 3, 7, fn + LIGHT_CACHE_SKY_LOAD, &t) && t == base + 0x212F3C0,
	      "anchor: the sky load reads the sky object's global");
	Check(LightCacheRipTarget(ambCall, CALL_REL, 1, 5, fn + LIGHT_CACHE_AMBIENT_CALL, &t) && t == base + 0x14150,
	      "anchor: the first call is the ambient day factor");
	Check(LightCacheRipTarget(litTest, CMP_RIP_0, 3, 8, fn + LIGHT_CACHE_LIT_TEST, &t)
	      && t == base + 0x21330B0 + LIGHT_CACHE_LIT_SET + 0x20,
	      "anchor: the lit-character test reads charactersWithLights' size");
	Check(!LightCacheRipTarget(skyLoad, CMP_RIP_0, 3, 7, fn, &t), "anchor: another opcode is refused");
	Check(!LightCacheRipTarget(skyLoad, MOV_RCX_RIP, 3, 6, fn, &t), "anchor: a length too short for the displacement is refused");
}

static void CheckShadowAndLine()
{
	LightCacheStats s;
	memset(&s, 0, sizeof(s));
	LightCacheShadowCompare(&s, LIGHT_KEY_EXACT, 0.5f, 0.5f);
	Check(s.would[LIGHT_KEY_EXACT] == 1 && s.mismatch[LIGHT_KEY_EXACT] == 0 && s.maxDiff[LIGHT_KEY_EXACT] == 0.0f,
	      "shadow: an equal value is a would-hit and no mismatch");
	LightCacheShadowCompare(&s, LIGHT_KEY_Q2, 0.5f, 0.25f);
	LightCacheShadowCompare(&s, LIGHT_KEY_Q2, 0.5f, 0.375f);
	Check(s.would[LIGHT_KEY_Q2] == 2 && s.mismatch[LIGHT_KEY_Q2] == 2 && s.maxDiff[LIGHT_KEY_Q2] == 0.25f,
	      "shadow: the largest difference is kept");
	LightCacheShadowCompare(&s, LIGHT_KEY_COUNT, 1.0f, 0.0f);
	Check(s.would[0] + s.would[1] + s.would[2] == 3, "shadow: an unknown kind counts nothing");

	s.calls = 10; s.bypass = 1; s.offMain = 0; s.skipC = 2; s.skipZ = 0; s.hit = 0; s.miss = 7; s.store = 6;
	s.epoch = 4; s.bumpZone = 2; s.bumpLoad = 1; s.bumpMode = 1;
	char buf[512];
	int n = LightCacheFormatLine(buf, sizeof(buf), LIGHT_CACHE_SHADOW, s);
	Check(n > 0 && std::string(buf) ==
	      "LightCache: mode=shadow calls=10 bypass=1 offMain=0 skipC=2 skipZ=0 hit=0 miss=7 store=6 epoch=4"
	      " bumpZone=2 bumpLoad=1 bumpMode=1 wouldExact=1 wouldQ05=0 wouldQ2=2 mmExact=0 mmQ05=0 mmQ2=2"
	      " diffExact=0 diffQ05=0 diffQ2=0.25", "line: the heartbeat's tokens in order");
	char small[16];
	Check(LightCacheFormatLine(small, sizeof(small), LIGHT_CACHE_ON, s) < 0 && strlen(small) == sizeof(small) - 1,
	      "line: a short buffer truncates and says so");
}

int main()
{
	CheckModes();
	CheckBypass();
	CheckKeys();
	CheckTable();
	CheckZones();
	CheckEpoch();
	CheckRules();
	CheckAnchors();
	CheckShadowAndLine();
	return CheckExit(SUITE_NAME);
}
