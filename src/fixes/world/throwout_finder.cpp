// throwout_finder.cpp - The throw-out finder, AI::findKOIntruder_town, replaced by vanilla's loop
// with one more condition (the candidate is not held), or chained with a held result filtered.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/throwout_internal.h"
#include "fixes/world/throwout_hold.h"
#include "fixes/world/throwout_policy.h"
#include "game/game.h"
#include "game/hand_key.h"
#include "plugin/hook_manifest.h"
#include <windows.h>
#include <string.h>
#include "base/klib_include.h"
#include <kenshi/Character.h>
#include <kenshi/AI/AI.h>
#include "base/klib_include_end.h"

namespace throwout_finder_detail {
// SenseItr's layout (0x18 bytes, no destructor): the map node, the end node, the two masks.
struct SenseItrPod { void* node; void* end; unsigned flagsAny, flagsNot; };
} // namespace throwout_finder_detail
using namespace throwout_finder_detail;

namespace fixes {

typedef float (__fastcall *findKOIntruderTown_t)(void* ai, const void* subject, void* out, bool justAsking);
typedef void* (__fastcall *senseItrCtor_t)(SenseItrPod* it, unsigned fany, unsigned fnot, void* seen);
typedef void* (__fastcall *senseItrGetCharacter_t)(SenseItrPod* it);
typedef void  (__fastcall *senseItrIncrement_t)(SenseItrPod* it);
typedef void* (__fastcall *getOwnerships_t)(void* character);
typedef bool  (__fastcall *isMyTown_t)(void* ownerships, void* town);
static findKOIntruderTown_t   orig_findKOIntruderTown = NULL;
static senseItrCtor_t         fn_senseItrCtor         = NULL;
static senseItrGetCharacter_t fn_senseItrGetCharacter = NULL;
static senseItrIncrement_t    fn_senseItrIncrement    = NULL;
static getOwnerships_t        fn_getOwnerships        = NULL;
static isMyTown_t             fn_isMyTown             = NULL;
static volatile LONG s_chainOriginal = 0;

// The callees' first bytes, compared at install before the row goes in.
static const unsigned char kCtorBytes[16] =
	{ 0x44,0x8B,0xD2,0x33,0xD2,0x49,0x8B,0xC1,0x49,0x39,0x51,0x20,0x74,0x13,0x4D,0x8B };
static const unsigned char kGetCharacterBytes[16] =
	{ 0x40,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x39,0x8B,0x47,0x18,0x48,0x83,0xC7,0x10 };
static const unsigned char kIncrementBytes[16] =
	{ 0x4C,0x8B,0x41,0x08,0x4C,0x39,0x01,0x74,0x2B,0x0F,0x1F,0x80,0x00,0x00,0x00,0x00 };
static const unsigned char kGetOwnershipsBytes[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,0x48 };
static const unsigned char kIsMyTownBytes[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xD2,0x75,0x08,0x32,0xC0 };

static volatile LONG s_finderCalls, s_finderSkips, s_chainCalls, s_chainFiltered;
static GuardCounter s_finderRows[] =
{
	{ "finder",        GF_COUNT, &s_finderCalls,   0 },
	{ "skips",         GF_COUNT, &s_finderSkips,   0 },
	{ "chainCalls",    GF_COUNT, &s_chainCalls,    0 },
	{ "chainFiltered", GF_COUNT, &s_chainFiltered, 0 },
};

// The virtuals vanilla's loop calls, by vtable offset. The first three only read; amInsideTownWalls
// can recompute and store the character's walls flag, so the loop calls it only where vanilla does.
typedef void* (__fastcall *vPtr_t)(void* self);
typedef bool  (__fastcall *vBool_t)(void* self);
typedef int   (__fastcall *vInt_t)(void* self);
static void* VTownLocation(void* o)  { return (*(vPtr_t*)(*(char**)o + 0x78))(o); }      // getCurrentTownLocation
static bool  VHasGates(void* town)   { return (*(vBool_t*)(*(char**)town + 0x2F0))(town); } // TownBase::hasGates
static bool  VIsUnconcious(void* o)  { return (*(vBool_t*)(*(char**)o + 0x30))(o); }      // isUnconcious
static int   VInsideWalls(void* o)   { return (*(vInt_t*)(*(char**)o + 0x118))(o); }      // amInsideTownWalls

// The null hand, as the finder's own first lines write it: type NULL_ITEM, the rest 0; the
// vftable at +0 is the caller's and stays.
static void WriteNullHand(void* out)
{
	char* o = (char*)out;
	*(unsigned*)(o + KLIB_OFF_hand_type)            = game::HAND_KEY_NULL_TYPE;
	*(unsigned*)(o + KLIB_OFF_hand_container)       = 0;
	*(unsigned*)(o + KLIB_OFF_hand_containerSerial) = 0;
	*(unsigned*)(o + KLIB_OFF_hand_index)           = 0;
	*(unsigned*)(o + KLIB_OFF_hand_serial)          = 0;
}

static void WriteHand(void* out, const game::HandKey& k)
{
	char* o = (char*)out;
	*(unsigned*)(o + KLIB_OFF_hand_type)            = k.type;
	*(unsigned*)(o + KLIB_OFF_hand_container)       = k.container;
	*(unsigned*)(o + KLIB_OFF_hand_containerSerial) = k.containerSerial;
	*(unsigned*)(o + KLIB_OFF_hand_index)           = k.index;
	*(unsigned*)(o + KLIB_OFF_hand_serial)          = k.serial;
}

// The AI back thread. The original runs (another plugin's detour of it stays in the chain); a
// held result is answered as no candidate.
static float ChainOriginal(void* aiPtr, const void* subject, void* out, bool justAsking)
{
	InterlockedIncrement(&s_chainCalls);
	float r = orig_findKOIntruderTown(aiPtr, subject, out, justAsking);
	if (r > 0.0f && ThrowoutHoldIsHeld(game::HandKeyFromHand(out), ThrowoutNowHours()))
	{
		InterlockedIncrement(&s_chainFiltered);
		WriteNullHand(out);
		return 0.0f;
	}
	return r;
}

// The AI back thread. Vanilla's loop (findKOIntruder_town 0x99B380) with one more condition: a
// held body is skipped, so a later candidate is still found. In chain mode the original runs and
// a held result is filtered instead. No lock, no allocation, no logging.
static float __fastcall hook_findKOIntruderTown(void* aiPtr, const void* subject, void* out, bool justAsking)
{
	InterlockedIncrement(&s_finderCalls);
	if (InterlockedCompareExchange(&s_chainOriginal, 0, 0))
		return ChainOriginal(aiPtr, subject, out, justAsking);
	WriteNullHand(out);
	AI* ai = (AI*)aiPtr;
	Character* me = ai->me;
	void* town = VTownLocation(me);                       // vt+0x78
	if (!town || !VHasGates(town))                        // vt+0x2F0
		return 0.0f;
	if (!fn_isMyTown(fn_getOwnerships(me), town))
		return 0.0f;
	SenseItrPod it;
	fn_senseItrCtor(&it, 0x400E, 0x201, &ai->sensoryData.seen);
	const double now = ThrowoutNowHours();
	for (; it.node != it.end; fn_senseItrIncrement(&it))
	{
		Character* c = (Character*)fn_senseItrGetCharacter(&it);
		// Vanilla's order, short-circuited as it is: each query runs only past the tests before it.
		const bool carried = c != NULL && c->_isBeingCarried;
		const bool sameTown = c != NULL && !carried && VTownLocation(c) == town;
		const bool ko = sameTown && VIsUnconcious(c);
		const int inSomething = c != NULL ? (int)c->inSomething : 0;
		const int walls = (ko && inSomething != kThrowoutInPrison) ? VInsideWalls(c) : 0;
		const bool vanilla = ThrowoutCandidate(c != NULL, carried, sameTown, ko, inSomething, walls, false);
		if (!vanilla)
			continue;
		if (ThrowoutHoldIsHeld(game::HandKeyOfObject(c), now))
		{
			InterlockedIncrement(&s_finderSkips);
			continue;
		}
		WriteHand(out, game::HandKeyOfObject(c));
		return 1.0f;
	}
	return 0.0f;
}

const char* ThrowoutFinderInstall(int* installed)
{
	if (memcmp((const void*)GameAddr(RVA_SENSE_ITR_CTOR), kCtorBytes, sizeof(kCtorBytes)) != 0)
		return "bytes: SenseItr::ctor";
	if (memcmp((const void*)GameAddr(RVA_SENSE_ITR_GET_CHARACTER), kGetCharacterBytes, sizeof(kGetCharacterBytes)) != 0)
		return "bytes: SenseItr::getCharacter";
	if (memcmp((const void*)GameAddr(RVA_SENSE_ITR_INCREMENT), kIncrementBytes, sizeof(kIncrementBytes)) != 0)
		return "bytes: SenseItr::increment";
	if (memcmp((const void*)GameAddr(RVA_CHAR_GET_OWNERSHIPS), kGetOwnershipsBytes, sizeof(kGetOwnershipsBytes)) != 0)
		return "bytes: Character::getOwnerships";
	if (memcmp((const void*)GameAddr(RVA_OWNERSHIPS_IS_MY_TOWN), kIsMyTownBytes, sizeof(kIsMyTownBytes)) != 0)
		return "bytes: Ownerships::isMyTown";

	// Before the install: the detour calls them as soon as it is in.
	fn_senseItrCtor         = (senseItrCtor_t)GameAddr(RVA_SENSE_ITR_CTOR);
	fn_senseItrGetCharacter = (senseItrGetCharacter_t)GameAddr(RVA_SENSE_ITR_GET_CHARACTER);
	fn_senseItrIncrement    = (senseItrIncrement_t)GameAddr(RVA_SENSE_ITR_INCREMENT);
	fn_getOwnerships        = (getOwnerships_t)GameAddr(RVA_CHAR_GET_OWNERSHIPS);
	fn_isMyTown             = (isMyTown_t)GameAddr(RVA_OWNERSHIPS_IS_MY_TOWN);

	const char* why = HookInstall(HOOK_FIND_KO_INTRUDER_TOWN, hook_findKOIntruderTown,
	                              &orig_findKOIntruderTown, installed, true);
	if (why)
		orig_findKOIntruderTown = NULL;
	return why;
}

void ThrowoutFinderChainOriginal(bool on)
{
	InterlockedExchange(&s_chainOriginal, on ? 1 : 0);
}

const GuardCounter* ThrowoutFinderCounterRows(int* count)
{
	*count = 4;
	return s_finderRows;
}

} // namespace fixes
