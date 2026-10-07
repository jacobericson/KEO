// inventory_config.cpp - defaults and INI rows for the inventory module.
#include "inventory/inventory_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

namespace keo_inventory {

const InventoryConfig kInventoryDefaults =
{
	true, // backpackFirstDefault
	true, // backpackFoodScoreEnabled
	true, // backpackDialogueFunctionEnabled
	true, // operatorFillBeforeDeliverEnabled
};

InventoryConfig g_inventoryCfg = kInventoryDefaults;
} // namespace keo_inventory

namespace inventory_config_detail {
union InventoryConfigPodCheck { keo_inventory::InventoryConfig s; };
} // namespace inventory_config_detail
using namespace inventory_config_detail;

namespace keo_inventory {

const ConfigKey g_inventoryConfigKeys[] =
{
	CFG_OBOOL("backpackFirstDefault", InventoryConfig, backpackFirstDefault,                 DOC, SHOW,
	  "Backpack first",
	  "Picked-up items go into the worn backpack first, unless the character's backpack window says otherwise."),
	CFG_OBOOL("backpackFoodScore", InventoryConfig, backpackFoodScoreEnabled,                DOC, SHOW,
	  "Food in the backpack counts",
	  "A character carrying food in its worn backpack does not go looking for food on the ground."),
	CFG_OBOOL("backpackDialogueFunction", InventoryConfig, backpackDialogueFunctionEnabled,  DOC, SHOW,
	  "Dialogue checks the backpack",
	  "A dialogue line that needs an item of a kind also finds it in the worn backpack."),
	CFG_OBOOL("operatorFillBeforeDeliver", InventoryConfig, operatorFillBeforeDeliverEnabled, DOC, SHOW,
	  "Operators fill up before delivering",
	  "A machine operator collects until its inventory and backpack are full, then delivers; not while hungry."),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace keo_inventory
