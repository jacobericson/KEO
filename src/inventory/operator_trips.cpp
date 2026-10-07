// operator_trips.cpp - The detour on the operator's GOAP state-89 evaluator
// (AI::haveSomeResourcesFromThisMachineButWantThemGoneIfPossible): when vanilla says the load
// should go, a player's operator at its own powered resource machine with valid inputs, not
// hungry, and with room in its main inventory or worn backpack for the product, is told "not
// yet", so the planner's collect task keeps it at the machine. The install step runs on the main
// thread at startup; the detour runs wherever the planner evaluates the requirement.
#include "inventory/operator_trips.h"
#include "inventory/operator_policy.h"
#include "inventory/backpack_reader.h"
#include "plugin/hook_manifest.h"
#include "game/game.h"
#include "base/core.h"
#include <string.h>
#include <string>

namespace operator_trips_detail {
typedef bool  (__fastcall *wantGone_t)(void* ai, const void* subject, const void* v);
typedef void* (__fastcall *anythingButBase_t)(void* handles, const void* h, bool deadOnes);
typedef bool  (__fastcall *aiCheck_t)(void* ai, const void* subject, const void* v);
typedef bool  (__fastcall *wantsToEatNow_t)(void* character);
typedef void* (__fastcall *getter_t)(void* self);
typedef bool  (__fastcall *hasRoom_t)(void* inventory, void* item);
} // namespace operator_trips_detail
using namespace operator_trips_detail;

namespace keo_inventory {

// Read from the IDB 2026-09-30: AI::me +0x2F8; RootObjectBase vtable +0x58 getFaction;
// Faction::isPlayer +0x250; Building vtable +0x2F8 getFunctionStuff; StorageBuilding +0x440 output
// kind and vtable +0x550 getProductionItemData; Character vtable +0x160 getInventory; Inventory
// vtable +0x20 hasRoomForItem.
static const size_t kAiMe = 0x2F8, kFactionIsPlayer = 0x250, kStorageOutputKind = 0x440;
static const size_t kVtGetFaction = 0x58, kVtFunctionStuff = 0x2F8, kVtProductItem = 0x550;
static const size_t kVtGetInventory = 0x160, kVtHasRoom = 0x20;

static wantGone_t        orig_operatorWantGone  = NULL;
static anythingButBase_t fn_getAnythingButBase  = NULL;
static aiCheck_t         fn_buildingHasPower    = NULL;
static aiCheck_t         fn_isMachineInputInvalid = NULL;
static wantsToEatNow_t   fn_wantsToEatNow       = NULL;
static void*             s_handleManager        = NULL;
static volatile LONG     s_reason[OR_COUNT];

// Each callee's first bytes in this build: the row installs only when all four match.
static const unsigned char kAnythingButBaseHead[16] =
	{ 0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF1,0x48,0x63,0x4A };
static const unsigned char kBuildingHasPowerHead[16] =
	{ 0x48,0x83,0xEC,0x28,0x83,0x7A,0x08,0x00,0x74,0x07,0x32,0xC0,0x48,0x83,0xC4,0x28 };
static const unsigned char kMachineInputInvalidHead[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xCA,0xE8,0xDE,0x8D,0xAA,0xFF,0x48,0x8B };
static const unsigned char kWantsToEatNowHead[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x01,0x48,0x8B,0xD9,0xFF,0x90,0x40,0x03 };

static void* VGet(void* self, size_t slot)
{
	return ((getter_t)(*(void***)self)[slot / 8])(self);
}

// Fills f in OperatorReasonOf's order and stops at the first fact that fails. Vanilla getters
// only; every pointer read is NULL-checked.
static OperatorReason GatherOperatorFacts(void* ai, const void* subject, const void* v, OperatorFacts* f)
{
	void* me = *(void* const*)((const char*)ai + kAiMe);
	void* faction = me ? VGet(me, kVtGetFaction) : NULL;
	f->isPlayer = faction && *(void* const*)((const char*)faction + kFactionIsPlayer) != NULL;
	if (!f->isPlayer) return OperatorReasonOf(*f);
	void* obj = fn_getAnythingButBase(s_handleManager, subject, true);
	void* fs = obj ? VGet(obj, kVtFunctionStuff) : NULL;
	f->haveMachine = fs != NULL;
	if (!f->haveMachine) return OperatorReasonOf(*f);
	f->ownsMachine = VGet(fs, kVtGetFaction) == faction;
	if (!f->ownsMachine) return OperatorReasonOf(*f);
	f->isResource = *(const int*)((const char*)fs + kStorageOutputKind) == OPERATOR_RESOURCE_OUTPUT;
	if (!f->isResource) return OperatorReasonOf(*f);
	f->powered = fn_buildingHasPower(ai, subject, v);
	if (!f->powered) return OperatorReasonOf(*f);
	f->inputsValid = !fn_isMachineInputInvalid(ai, subject, v);
	if (!f->inputsValid) return OperatorReasonOf(*f);
	f->hungry = fn_wantsToEatNow(me);
	if (f->hungry) return OperatorReasonOf(*f);
	void* item = VGet(fs, kVtProductItem);
	f->haveProduct = item != NULL;
	if (!f->haveProduct) return OperatorReasonOf(*f);
	void* inv = VGet(me, kVtGetInventory);
	bool room = inv && ((hasRoom_t)(*(void***)inv)[kVtHasRoom / 8])(inv, item);
	if (!room)
	{
		void* bag = WornBackpackInventory(me);
		room = bag && ((hasRoom_t)(*(void***)bag)[kVtHasRoom / 8])(bag, item);
	}
	f->hasRoom = room;
	return OperatorReasonOf(*f);
}

// AI back thread inside the GOAP planner pass (and wherever else the requirement is checked;
// thread-agnostic). The original's answer, or false while the operator should keep collecting.
// Our code takes no lock, allocates nothing and logs nothing; hasRoomForItem's cache is vanilla's.
static bool __fastcall hook_operatorWantGone(void* ai, const void* subject, const void* v)
{
	const bool vanilla = orig_operatorWantGone(ai, subject, v);
	if (!vanilla)
		InterlockedIncrement(&s_reason[OR_VANILLA_FALSE]);
	if (!vanilla || !ai)
		return vanilla;
	OperatorFacts f;
	memset(&f, 0, sizeof(f));
	f.vanillaTrue = true;
	const OperatorReason r = GatherOperatorFacts(ai, subject, v, &f);
	InterlockedIncrement(&s_reason[r]);
	return OperatorAnswer(vanilla, r);
}

void InstallOperatorTrips(int* installed, int*)
{
	if (!HookRowWanted(HOOK_OPERATOR_WANT_GONE)) return;
	// The reader binds idempotently; unbound, the room test could never see a backpack. Each
	// callee's head is checked before its binding is set, and every binding before the install.
	const char* reader = NULL;   // set only when the reader refuses
	const char* why = NULL;
	if (!BackpackReaderInit(&reader))
		why = "reader refused: ";
	else if (memcmp((const void*)GameAddr(RVA_HANDLES_ANYTHING_BUT_BASE), kAnythingButBaseHead, 16) != 0)
		why = "getAnythingButBase";
	else if (memcmp((const void*)GameAddr(RVA_AI_BUILDING_HAS_POWER), kBuildingHasPowerHead, 16) != 0)
		why = "buildingHasPower";
	else if (memcmp((const void*)GameAddr(RVA_AI_MACHINE_INPUT_INVALID), kMachineInputInvalidHead, 16) != 0)
		why = "isMachineInputInvalid";
	else if (memcmp((const void*)GameAddr(RVA_CHARACTER_WANTS_TO_EAT_NOW), kWantsToEatNowHead, 16) != 0)
		why = "wantsToEatNow";
	else
	{
		// Before the install: the detour calls them as soon as it is in.
		fn_getAnythingButBase    = (anythingButBase_t)GameAddr(RVA_HANDLES_ANYTHING_BUT_BASE);
		fn_buildingHasPower      = (aiCheck_t)GameAddr(RVA_AI_BUILDING_HAS_POWER);
		fn_isMachineInputInvalid = (aiCheck_t)GameAddr(RVA_AI_MACHINE_INPUT_INVALID);
		fn_wantsToEatNow         = (wantsToEatNow_t)GameAddr(RVA_CHARACTER_WANTS_TO_EAT_NOW);
		s_handleManager          = (void*)GameAddr(RVA_GLOBAL_HANDLE_MANAGER);
		why = HookInstall(HOOK_OPERATOR_WANT_GONE, hook_operatorWantGone, &orig_operatorWantGone, installed, true);
	}
	if (!why)
	{
		LogMsg("OperatorTrips: installed");
	}
	else
	{
		orig_operatorWantGone = NULL;
		ErrorLog(std::string("OperatorTrips: not installed (") + why + (reader ? reader : "")
		         + "); operators deliver as vanilla");
	}
}

void OperatorTripsCountersRead(long* out, int n)
{
	if (!out)
		return;
	for (int i = 0; i < n && i < OR_COUNT; ++i)
		out[i] = InterlockedCompareExchange(&s_reason[i], 0, 0);
}

} // namespace keo_inventory
