// backpack_first.cpp - The detour on Character::giveItem: a character whose backpack-first setting
// is on offers a picked-up item to its worn backpack before the original fills the main inventory.
// The setting is the character's table entry, else the INI default. The install step runs on the
// main thread at startup; the detour runs wherever the game gives an item.
#include "inventory/backpack_first.h"
#include "inventory/backpack_policy.h"
#include "inventory/backpack_reader.h"
#include "inventory/backpack_table.h"
#include "inventory/inventory_config.h"
#include "game/hand_key.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include <string>

namespace backpack_first_detail {
typedef bool (__fastcall *giveItem_t)(void* character, void* item, bool dropOnFail, bool destroyOnFail);
typedef bool (__fastcall *tryAddItem_t)(void* inventory, void* item, int quantity);
} // namespace backpack_first_detail
using namespace backpack_first_detail;

namespace keo_inventory {

static const size_t VT_INVENTORY_TRY_ADD_ITEM = 0x18;
static giveItem_t orig_giveItem = NULL;
static volatile LONG s_calls = 0, s_routed = 0, s_placed = 0, s_fellBack = 0;
static bool s_installed = false;

// AI back thread and main thread. Tries the worn backpack first for a character whose setting is
// on; on every other path, and when the backpack refuses, the original runs once with its
// arguments untouched. No lock, no allocation, no logging.
static bool __fastcall hook_giveItem(void* character, void* item, bool dropOnFail, bool destroyOnFail)
{
	InterlockedIncrement(&s_calls);
	if (character && item)
	{
		void* backpack = WornBackpack(character);
		void* backpackInv = WornBackpackInventory(character);
		void* itemInv = ItemInventory(item);
		bool route = RouteToBackpackFirst(
			BackpackFirstGet(game::HandKeyOfObject(character)),
			g_inventoryCfg.backpackFirstDefault,
			CharacterIsAnimal(character),
			backpackInv != NULL,
			item == backpack,
			itemInv != NULL && !InventoryIsEmpty(itemInv),
			backpackInv != NULL && InventoryWouldAutoEquip(CharacterInventory(character), item));
		if (route)
		{
			InterlockedIncrement(&s_routed);
			tryAddItem_t tryAdd = *(tryAddItem_t*)(*(uintptr_t*)backpackInv + VT_INVENTORY_TRY_ADD_ITEM);
			if (tryAdd(backpackInv, item, 1))
			{
				InterlockedIncrement(&s_placed);
				return true;
			}
			InterlockedIncrement(&s_fellBack);
		}
	}
	return orig_giveItem(character, item, dropOnFail, destroyOnFail);
}

void InstallBackpackFirst(int* installed, int*)
{
	if (!HookRowWanted(HOOK_CHARACTER_GIVE_ITEM)) return;
	// The reader's callees and layouts are checked before the detour can reach them.
	const char* why = NULL;
	if (BackpackReaderInit(&why))
		why = HookInstall(HOOK_CHARACTER_GIVE_ITEM, hook_giveItem, &orig_giveItem, installed, true);
	if (!why)
	{
		s_installed = true;
		LogMsg("BackpackFirst: installed");
	}
	else
	{
		orig_giveItem = NULL;
		ErrorLog(std::string("BackpackFirst: not installed (") + why + ")");
	}
}

bool BackpackFirstInstalled()
{
	return s_installed;
}

long BackpackFirstCalls()
{
	return InterlockedCompareExchange(&s_calls, 0, 0);
}

long BackpackFirstRouted()
{
	return InterlockedCompareExchange(&s_routed, 0, 0);
}

long BackpackFirstPlaced()
{
	return InterlockedCompareExchange(&s_placed, 0, 0);
}

long BackpackFirstFellBack()
{
	return InterlockedCompareExchange(&s_fellBack, 0, 0);
}

} // namespace keo_inventory
