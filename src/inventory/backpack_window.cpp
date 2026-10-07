// backpack_window.cpp - The backpack-first tick box in a worn backpack's window. A post-hook on
// BackpackInventoryLayout::setupSections, which builds each backpack window once per instance,
// adds a tick box beside the window's arrange button when the window belongs to a player
// character wearing that backpack. The box shows the character's table entry, else the INI
// default, and a click flips the entry. The box carries its owner's hand key in a user string,
// never a pointer, and is destroyed with the window's layout. Everything here runs on the main
// thread, the table's only writer, and takes no lock.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "inventory/backpack_window.h"
#include "inventory/backpack_first.h"
#include "inventory/backpack_policy.h"
#include "inventory/backpack_table.h"
#include "inventory/backpack_reader.h"
#include "inventory/backpack_sidecar.h"
#include "inventory/inventory_config.h"
#include "game/game.h"
#include "game/klib_member_contract.h"
#include "game/hand_key.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include <windows.h>
#include <string>
#include <cstdio>
#include <string.h>
#include "base/klib_include.h"
#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include "base/klib_include_end.h"

namespace backpack_window_detail {
typedef void (__fastcall *setupSections_t)(void* layout, void* gui, void* sections, void* inventory);
typedef int  (__fastcall *getDataType_t)(void* object);
typedef void* (__fastcall *getFaction_t)(void* object);
} // namespace backpack_window_detail
using namespace backpack_window_detail;

namespace keo_inventory {

static const size_t OFF_LAYOUT_MAIN_WIDGET     = 0x8;
static const size_t OFF_GUI_CALLBACK_OBJECT    = 0x50;
static const size_t OFF_CONTAINER_OWNER        = 0x230;
static const size_t OFF_FACTION_IS_PLAYER      = KLIB_OFF_Faction_isPlayer;
static const size_t VT_ROOT_GET_DATA_TYPE      = 0x20;
static const size_t VT_ROOT_GET_FACTION        = 0x58;
static const int    DATA_TYPE_CHARACTER        = 1;
static const int    DATA_TYPE_CONTAINER        = 46;
static const char*  const BOX_NAME_SUFFIX      = "keo_backpackFirst";
static const char*  const BOX_KEY              = "keo_key";
static const char*  const ARRANGE_SUFFIX       = "ArrangeButton";

// Instructions whose bytes encode the three offsets above: the layout's main widget, the window's
// callback object, and the backpack's callback owner.
static const unsigned char kMainWidgetCheck[8] = { 0x48,0x8B,0x4F,0x08,0x48,0x83,0xC1,0x08 };
static const unsigned char kOwnerReadCheck[4]  = { 0x4C,0x8B,0x5B,0x50 };
static const unsigned char kOwnerWriteCheck[7] = { 0x4C,0x89,0x9F,0x30,0x02,0x00,0x00 };

static setupSections_t orig_setupSections = NULL;
static volatile LONG s_boxes = 0, s_skipped = 0, s_noArrange = 0, s_clicks = 0, s_clickFailed = 0;

// The object's RootObjectBase::getDataType, or -1 for NULL.
static int DataTypeOf(void* object)
{
	if (!object)
		return -1;
	getDataType_t fn = *(getDataType_t*)(*(const uintptr_t*)object + VT_ROOT_GET_DATA_TYPE);
	return fn(object);
}

// The character's faction is the player's: its isPlayer pointer is set.
static bool IsPlayerFaction(void* character)
{
	getFaction_t fn = *(getFaction_t*)(*(const uintptr_t*)character + VT_ROOT_GET_FACTION);
	void* faction = fn(character);
	return faction && *(void* const*)((const char*)faction + OFF_FACTION_IS_PLAYER) != NULL;
}

// Depth first under parent, at most levels deep: the first widget whose name ends with suffix.
static MyGUI::Widget* FindBySuffix(MyGUI::Widget* parent, const char* suffix, int levels)
{
	if (!parent || levels <= 0)
		return NULL;
	const size_t n = strlen(suffix);
	const size_t count = parent->getChildCount();
	for (size_t i = 0; i < count; ++i)
	{
		MyGUI::Widget* child = parent->getChildAt(i);
		if (!child)
			continue;
		const std::string& name = child->getName();
		if (name.size() >= n && name.compare(name.size() - n, n, suffix) == 0)
			return child;
		MyGUI::Widget* found = FindBySuffix(child, suffix, levels - 1);
		if (found)
			return found;
	}
	return NULL;
}

// Main thread, from MyGUI's click dispatch. Flips the owner's choice and the box; nothing is
// thrown back into the game. The box's key is followed to its character's live hand first, and
// the table re-keyed, so the write updates the entry the character's pickups read.
static void OnBackpackFirstClick(MyGUI::Widget* sender)
{
	try
	{
		MyGUI::Button* box = sender ? sender->castType<MyGUI::Button>(false) : NULL;
		const std::string& text = sender ? sender->getUserString(BOX_KEY) : std::string();
		game::HandKey key;
		if (!box || !HandKeyParse(text.c_str(), (int)text.size(), &key))
		{
			InterlockedIncrement(&s_clickFailed);
			return;
		}
		game::HandKey live;
		if (BackpackResolveKey(key, &live) && !game::HandKeyEqual(live, key))
		{
			char buf[64] = { 0 };
			HandKeyFormat(live, buf, (int)sizeof(buf));
			sender->setUserString(BOX_KEY, buf);
			key = live;
		}
		BackpackRekeyNow(false);
		int cur = BackpackFirstGet(key);
		bool on = cur < 0 ? g_inventoryCfg.backpackFirstDefault : cur != 0;
		if (!BackpackFirstSet(key, on ? 0 : 1))
		{
			InterlockedIncrement(&s_clickFailed);
			return;
		}
		box->setStateSelected(!on);
		InterlockedIncrement(&s_clicks);
	}
	catch (...)
	{
		InterlockedIncrement(&s_clickFailed);
	}
}

// Main thread, once per backpack window, after setupSections built its sections and buttons.
// Every type is read before a type-specific read; the widget pointers live only for this call.
static void AddBackpackFirstBox(void* layout, void* gui)
{
	void* container = gui ? *(void**)((char*)gui + OFF_GUI_CALLBACK_OBJECT) : NULL;
	int containerType = DataTypeOf(container);
	void* owner = containerType == DATA_TYPE_CONTAINER ? *(void**)((char*)container + OFF_CONTAINER_OWNER) : NULL;
	int ownerType = DataTypeOf(owner);
	bool isAnimal = false, isPlayer = false, wears = false;
	if (ownerType == DATA_TYPE_CHARACTER)
	{
		isAnimal = CharacterIsAnimal(owner);
		isPlayer = IsPlayerFaction(owner);
		wears = WornBackpack(owner) == container;
	}
	if (!BackpackBoxWanted(containerType, ownerType, isAnimal, isPlayer, wears))
	{
		InterlockedIncrement(&s_skipped);
		return;
	}

	// A squad move since the last re-key tick is followed before the entry is read.
	BackpackRekeyNow(false);

	MyGUI::Widget* root = layout ? *(MyGUI::Widget**)((char*)layout + OFF_LAYOUT_MAIN_WIDGET) : NULL;
	MyGUI::Widget* arrange = FindBySuffix(root, ARRANGE_SUFFIX, 4);
	MyGUI::Widget* parent = arrange ? arrange->getParent() : NULL;
	if (!parent)
	{
		InterlockedIncrement(&s_noArrange);
		return;
	}
	// The layout prefixes every widget name; the box takes the arrange button's prefix.
	const std::string& arrangeName = arrange->getName();
	const std::string prefix = arrangeName.substr(0, arrangeName.size() - strlen(ARRANGE_SUFFIX));
	if (root->findWidget(prefix + BOX_NAME_SUFFIX))
		return;

	const MyGUI::IntCoord& c = arrange->getCoord();
	int side = c.height;
	MyGUI::Button* box = parent->createWidget<MyGUI::Button>("Kenshi_TickBoxSkin", MyGUI::IntCoord(c.left + c.width + 4, c.top, side, side), MyGUI::Align::Right | MyGUI::Align::Bottom, prefix + BOX_NAME_SUFFIX);

	game::HandKey key = game::HandKeyOfObject(owner);
	char buf[64] = { 0 };
	HandKeyFormat(key, buf, (int)sizeof(buf));
	box->setUserString(BOX_KEY, buf);

	int cur = BackpackFirstGet(key);
	box->setStateSelected(cur < 0 ? g_inventoryCfg.backpackFirstDefault : cur != 0);

	box->eventMouseButtonClick += MyGUI::newDelegate(&OnBackpackFirstClick);
	InterlockedIncrement(&s_boxes);
}

// Main thread, once per backpack window, after the window's own sections and buttons exist.
static void __fastcall hook_setupSections(void* layout, void* gui, void* sections, void* inventory)
{
	orig_setupSections(layout, gui, sections, inventory);
	try
	{
		AddBackpackFirstBox(layout, gui);
	}
	catch (...)
	{
		InterlockedIncrement(&s_skipped);
	}
}

void InstallBackpackWindow(int* installed, int*)
{
	if (!HookRowWanted(HOOK_BACKPACK_LAYOUT_SETUP_SECTIONS)) return;
	// A box would write a table no pickup reads.
	if (!BackpackFirstInstalled()) { LogError("BackpackWindow: not installed (backpackFirst)"); return; }
	const char* why = NULL;
	if (memcmp((const void*)GameAddr(RVA_LAYOUT_MAIN_WIDGET_CHECK), kMainWidgetCheck, sizeof(kMainWidgetCheck)) != 0)
		why = "mainWidget";
	else if (memcmp((const void*)GameAddr(RVA_BACKPACK_OWNER_READ_CHECK), kOwnerReadCheck, sizeof(kOwnerReadCheck)) != 0)
		why = "ownerRead";
	else if (memcmp((const void*)GameAddr(RVA_BACKPACK_OWNER_WRITE_CHECK), kOwnerWriteCheck, sizeof(kOwnerWriteCheck)) != 0)
		why = "ownerWrite";
	if (!why)
		why = HookInstall(HOOK_BACKPACK_LAYOUT_SETUP_SECTIONS, hook_setupSections, &orig_setupSections, installed, true);
	if (!why)
		LogMsg("BackpackWindow: installed");
	else
		LogError(std::string("BackpackWindow: not installed (") + why + ")");
}

void BackpackWindowCounters(long out[5])
{
	out[0] = InterlockedCompareExchange(&s_boxes, 0, 0);
	out[1] = InterlockedCompareExchange(&s_skipped, 0, 0);
	out[2] = InterlockedCompareExchange(&s_noArrange, 0, 0);
	out[3] = InterlockedCompareExchange(&s_clicks, 0, 0);
	out[4] = InterlockedCompareExchange(&s_clickFailed, 0, 0);
}

} // namespace keo_inventory
