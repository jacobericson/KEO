#ifndef KEO_FIXES_LIGHT_CACHE_POLICY_H
#define KEO_FIXES_LIGHT_CACHE_POLICY_H

// The light-level cache's pure rules (light_cache.h): its modes, its keys and
// table, the loaded-cell fingerprint behind its epoch, when it may serve and
// store, the code-byte decode its install checks, and its heartbeat line. No
// game or Windows header, so the host suite links it.

#include <stddef.h>
#include <stdint.h>

namespace fixes {

enum LightCacheMode
{
	LIGHT_CACHE_OFF    = 0,  // the detour calls the original and counts nothing
	LIGHT_CACHE_SHADOW = 1,  // every call runs the original; the tables only count would-be hits
	LIGHT_CACHE_ON     = 2   // an exact-key hit is served while no character carries a light
};

// The mode a config value names; any other value is off.
int LightCacheModeOf(int cfgValue);
const char* LightCacheModeName(int mode);

// The original returns 1.0 without walking any light while the sky's ambient
// day factor is above one half; such a call is never keyed. NaN counts as
// above.
bool LightCacheAmbientBypass(float ambient);

// An entry lives 10 s: a building light's power can change with no input of
// the key changing, so a served value is at most this old.
const uint32_t LIGHT_CACHE_TTL_MS = 10000;
const int      LIGHT_CACHE_SLOTS  = 4096;    // per table, a power of two
// A coordinate beyond this is never keyed, so a quantized cell fits an int32.
const float    LIGHT_CACHE_MAX_COORD = 1.0e8f;

// GameWorld::getLightLevel's own instructions the install checks, as offsets
// from its entry: the sky object's load, the ambient day factor's call and
// the test of charactersWithLights' size; and that set's offset in GameWorld.
const size_t LIGHT_CACHE_SKY_LOAD     = 0x3D;   // mov rcx,[rip+disp32]
const size_t LIGHT_CACHE_AMBIENT_CALL = 0x44;   // call rel32
const size_t LIGHT_CACHE_LIT_TEST     = 0x2CA;  // cmp qword ptr [rip+disp32],0
const size_t LIGHT_CACHE_LIT_SET      = 0x588;  // GameWorld::charactersWithLights

// The exact key, and two coarser ones the shadow mode measures: the position
// as its float bits, or as its 0.5- or 2-unit cell.
enum LightCacheKeyKind { LIGHT_KEY_EXACT, LIGHT_KEY_Q05, LIGHT_KEY_Q2, LIGHT_KEY_COUNT };

// Every input of the original's result: seven 32-bit words and no padding, so
// keys compare and hash as bytes.
struct LightCacheKey
{
	uint32_t x, y, z;    // float bits (exact) or a signed cell index (quantized)
	uint32_t floor;
	uint32_t outdoors;   // 1 or 0, the original's fourth argument
	uint32_t ambient;    // the ambient day factor's float bits
	uint32_t epoch;
};
static_assert(sizeof(LightCacheKey) == 28, "LightCacheKey is seven words with no padding");

// False, with nothing written, for an unknown kind or a coordinate that is
// not finite or beyond LIGHT_CACHE_MAX_COORD.
bool LightCacheMakeKey(int kind, const float pos[3], int floor, bool outdoors, float ambient, uint32_t epoch,
                       LightCacheKey* out);

// The key's slot: FNV-1a of its bytes, masked to the table.
uint32_t LightCacheSlot(const LightCacheKey& k);

struct LightCacheEntry
{
	LightCacheKey key;
	uint32_t      valueBits;
	uint32_t      stampMs;
	uint32_t      used;
};

// Direct-mapped: a store overwrites whatever held its slot.
struct LightCacheTable
{
	LightCacheEntry slot[LIGHT_CACHE_SLOTS];
};

// True, with *value, when the key's slot holds this key stored less than
// LIGHT_CACHE_TTL_MS before nowMs (unsigned, so a wrapped clock still ages).
bool LightCacheFind(const LightCacheTable& t, const LightCacheKey& k, uint32_t nowMs, float* value);
void LightCacheStore(LightCacheTable* t, const LightCacheKey& k, float value, uint32_t nowMs);

// The loaded cells the original walks, folded independent of list order: a
// cell joining, leaving or replaced changes the value.
struct LightCacheZones
{
	uint64_t sum;
	uint32_t count;
};
void     LightCacheZonesAdd(LightCacheZones* z, uint64_t zone);
uint64_t LightCacheZonesValue(const LightCacheZones& z);

// Advances *epoch and records fp when fp differs from *lastFp; true when it
// advanced.
bool LightCacheEpochStep(uint64_t fp, uint64_t* lastFp, uint32_t* epoch);

// A value is served only in on mode while no character carries a light: the
// original adds each lit character's lights after the building lights, and
// the key holds none of them.
bool LightCacheMayServe(int mode, bool litEmpty);
// A result is stored only when no character was lit before and after the
// original ran, in shadow or on.
bool LightCacheMayStore(int mode, bool litEmptyBefore, bool litEmptyAfter);

// The absolute target of a RIP-relative operand: insn holds the
// instruction, its first opLen bytes must equal opcode and a disp32 follows
// them; insnLen is the whole instruction's length (an immediate may follow
// the displacement). False when the opcode differs or insnLen is too short.
bool LightCacheRipTarget(const unsigned char* insn, const unsigned char* opcode, size_t opLen, size_t insnLen,
                         uintptr_t insnAddr, uintptr_t* target);

// Main-thread counters, cumulative for the session.
struct LightCacheStats
{
	long     calls;      // calls past the mode test (shadow or on) on the main thread
	long     bypass;     // ambient above one half, or a position that cannot be keyed
	long     offMain;    // calls on another thread, passed through
	long     skipC;      // a character carried a light: neither served nor stored
	long     skipZ;      // the loaded cells could not be read: neither served nor stored
	long     hit;        // served from the table
	long     miss;       // the original ran after a lookup
	long     store;
	long     bumpZone;
	long     bumpLoad;
	long     bumpMode;
	uint32_t epoch;
	long     would[LIGHT_KEY_COUNT];     // shadow: lookups that found an entry
	long     mismatch[LIGHT_KEY_COUNT];  // ... whose value differed in any bit from the original's
	float    maxDiff[LIGHT_KEY_COUNT];   // ... the largest |stored - original|
};

// One shadow lookup that found an entry: counts it, its bit mismatch and its
// difference. An unknown kind counts nothing.
void LightCacheShadowCompare(LightCacheStats* s, int kind, float stored, float real);

// The heartbeat line, NUL-terminated within cap; its length, or -1 when it
// was truncated.
int LightCacheFormatLine(char* buf, size_t cap, int mode, const LightCacheStats& s);

} // namespace fixes

#endif // KEO_FIXES_LIGHT_CACHE_POLICY_H
