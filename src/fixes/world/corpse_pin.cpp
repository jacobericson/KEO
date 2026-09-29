#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/corpse_pin.h"
#include "fixes/world/corpse_pin_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include <string>
#include <sstream>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include <kenshi/Platoon.h>         // ActivePlatoon, its `things` list
#include <kenshi/Character.h>       // Character, carryingObject, _isBeingCarried
#include "base/klib_include_end.h"

// A squad's things list is small (formation membership caps at
// MAX_FORMATION_MEMBERS_LIMIT elsewhere in the mod); this is a generous
// ceiling against a corrupt count, not a realistic size.
static const int kMaxScanMembers = 128;

typedef Ogre::Vector3* (__fastcall *calculateCurrentPos_t)(ActivePlatoon* self, Ogre::Vector3* out);
static calculateCurrentPos_t orig_calculateCurrentPos = NULL;

// RootObjectBase::getPosition, vtable offset 0x40 (Character overrides it).
// Called through the object's own vtable rather than as a typed C++ call --
// see the comment at the call site.
typedef Ogre::Vector3* (__fastcall *GetPositionFn)(void* self, Ogre::Vector3* out);
static const size_t kGetPositionVtableOffset = 0x40;

static volatile LONG s_overrideCount  = 0;
static volatile LONG s_noCarrierCount = 0;
// Detour entries, counted whether or not the fix applies to that call --
// the denominator behind overrides= and noCarrier= on the periodic line.
static volatile LONG s_calls          = 0;

static bool   s_installed  = false;
static double s_nextBeat   = 0.0;
static const double kBeatSeconds = 60.0;

static Ogre::Vector3* __fastcall hook_calculateCurrentPos(ActivePlatoon* self, Ogre::Vector3* out)
{
	InterlockedIncrement(&s_calls);
	Ogre::Vector3* result = orig_calculateCurrentPos(self, out);
	if (!fixes::g_fixesCfg.corpsePinEnabled || !self || !self->things.stuff)
		return result;

	unsigned int count = self->things.count;
	if (count > (unsigned int)kMaxScanMembers)
		count = (unsigned int)kMaxScanMembers;

	CorpsePinMember members[kMaxScanMembers];
	for (unsigned int i = 0; i < count; ++i)
	{
		RootObject* member = self->things.stuff[i];
		bool isChar = member && member->getDataType() == CHARACTER;
		Character* c = isChar ? (Character*)member : NULL;
		members[i].isCharacter    = isChar;
		members[i].isDead         = isChar && c->isDead();
		members[i].isBeingCarried = isChar && c->_isBeingCarried;
	}

	CorpsePinDecision d = CorpsePinDecide(members, (int)count);
	if (d.action != CORPSEPIN_USE_CARRIER)
		return result;

	Character* corpse  = (Character*)self->things.stuff[d.carriedIndex];
	Character* carrier = corpse->carryingObject.getCharacter();
	if (!carrier)
	{
		// carryingObject named a carrier that no longer resolves (already
		// dropped, or itself gone): nothing to redirect to, leave the
		// original's frozen position standing.
		InterlockedIncrement(&s_noCarrierCount);
		return result;
	}

	// getPosition() is virtual (RootObjectBase vtable+0x40, overridden by
	// Character) and returns Ogre::Vector3 by value through a hidden pointer,
	// same ABI as this detour's own out parameter -- so it is called through
	// the object's own vtable, writing straight into *out. A typed C++ call
	// would round-trip through Ogre::Vector3::operator=, which OgreMain
	// exports but this DLL does not link against.
	GetPositionFn getPos = *(GetPositionFn*)(*(char**)carrier + kGetPositionVtableOffset);
	getPos(carrier, out);
	InterlockedIncrement(&s_overrideCount);
	return out;
}

void InstallCorpsePin(int* installed, int*)
{
	if (!HookRowWanted(HOOK_ACTIVEPLATOON_CALC_POS))
		return;

	const char* why = NULL;
	if ((const void*)KlibRealAddress(&hand::getCharacter) != GameAddr(RVA_HAND_GET_CHARACTER)
	 || (const void*)KlibRealAddress(&Character::isDead)  != GameAddr(RVA_CHARACTER_IS_DEAD))
		why = "addresses";
	else
		why = HookInstall(HOOK_ACTIVEPLATOON_CALC_POS, hook_calculateCurrentPos,
				&orig_calculateCurrentPos, installed, true);

	if (!why)
	{
		s_installed = true;
		LogMsg("Corpse pin: installed");
		return;
	}
	orig_calculateCurrentPos = NULL;
	ErrorLog(std::string("Corpse pin: not installed (") + why + ")");
}

long CorpsePinOverrideCount()  { return InterlockedCompareExchange(&s_overrideCount, 0, 0); }
long CorpsePinNoCarrierCount() { return InterlockedCompareExchange(&s_noCarrierCount, 0, 0); }

// Unconditional, on a timer, whatever corpsePin's state is: "the pin never
// fired" and "the line was never printed" have to read differently in a log.
void CorpsePinTick(double now)
{
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;

	std::ostringstream ss;
	ss << "CorpsePin: enabled=" << (fixes::g_fixesCfg.corpsePinEnabled ? 1 : 0)
	   << " installed=" << (s_installed ? 1 : 0)
	   << " calls=" << InterlockedCompareExchange(&s_calls, 0, 0)
	   << " overrides=" << InterlockedCompareExchange(&s_overrideCount, 0, 0)
	   << " noCarrier=" << InterlockedCompareExchange(&s_noCarrierCount, 0, 0);
	LogMsg(ss.str());
}
