// inventory_config.h - the inventory module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"

namespace keo_inventory {

// Starts as a copy of kInventoryDefaults, then written by LoadConfig on the main
// thread before any hook installs; read-only afterwards, on any thread, but for a live row's field.
struct InventoryConfig
{
	// backpackFirstDefault: picked-up items go into a character's worn backpack before its own
	// inventory, for every character whose own choice (the backpack window's checkbox, saved with
	// the game) is not set. On by default; read at startup only.
	bool backpackFirstDefault;

	// backpackFoodScore: a character with food in its worn backpack does not look for food on the
	// ground. On by default; false skips the hook. Read at startup only.
	bool backpackFoodScoreEnabled;

	// backpackDialogueFunction: a dialogue condition on an item function also finds the item in
	// the tested character's worn backpack. On by default; false leaves the game's check. Read at
	// startup only.
	bool backpackDialogueFunctionEnabled;

	// operatorFillBeforeDeliver: a player's machine operator keeps collecting until its inventory
	// and backpack are full before it delivers; it delivers as the game does while hungry or when
	// the machine cannot run. On by default; false skips the hook. Read at startup only.
	bool operatorFillBeforeDeliverEnabled;

	// operatorHoldUntil: the encumbrance tier (the character panel's word) a machine operator may
	// fill up to while operatorFillBeforeDeliver holds its load; past it, it delivers. weightless,
	// lightweight, moderate, heavy (the default) or overloaded (weight never ends the hold). Live:
	// the KEO tab's close stores it on the main thread; the evaluator reads it once per call on the
	// AI back thread.
	int operatorHoldUntil;
};

extern InventoryConfig g_inventoryCfg;
extern const InventoryConfig kInventoryDefaults;
extern const ConfigKey g_inventoryConfigKeys[];

} // namespace keo_inventory
