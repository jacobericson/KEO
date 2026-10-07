// backpack_reader.cpp - The worn backpack and the inventory facts backpack-first needs, read at
// the game's own offsets. Each field offset below is encoded in an instruction whose bytes
// BackpackReaderInit checks, so a build whose layouts moved binds nothing and every reader answers
// as if there were no backpack (or, for the equip test, as if the item would equip). The item's
// getInventory slot is the one giveItem itself calls; isAnimal's is the KenshiLib vtable's. After
// init, any thread: plain reads and the two game callees, which neither lock nor allocate.
#include "inventory/backpack_reader.h"
#include "game/game.h"
#include <string.h>

namespace backpack_reader_detail {
// Bytes this build carries at sites whose instructions encode the offsets above; a mismatch
// means the layouts moved and backpack-first stays off.
struct ByteCheck { const char* why; size_t rva; unsigned char bytes[16]; int len; };

typedef void* (__fastcall *getSectionOfType_t)(void* inventory, int slot);
typedef bool  (__fastcall *isLimitedSlotCompatible_t)(void* section, void* item);
typedef void* (__fastcall *virtualGetter_t)(void* self);
} // namespace backpack_reader_detail
using namespace backpack_reader_detail;

static const size_t OFF_CHARACTER_INVENTORY  = 0x2E8;   // Character::inventory
static const size_t OFF_INV_SEARCH_COUNT     = 0x70;    // Inventory::sectionsInSearchOrder.count (32-bit)
static const size_t OFF_INV_SEARCH_DATA      = 0x78;    // Inventory::sectionsInSearchOrder.stuff
static const size_t OFF_INV_ALL_ITEMS_COUNT  = 0x18;    // Inventory::_allItems.count (32-bit)
static const size_t OFF_SECTION_LIMITED_SLOT = 0xB8;    // InventorySection::limitedSlot (AttachSlot)
static const size_t OFF_SECTION_ITEMS_FIRST  = 0x40;    // InventorySection::items._Myfirst
static const size_t OFF_SECTION_ITEMS_LAST   = 0x48;    // InventorySection::items._Mylast
static const size_t OFF_ITEM_OBJECT_TYPE     = 0x13C;   // RootObjectBase::objectType (itemType)
static const size_t VT_ITEM_GET_INVENTORY    = 0x160;   // Item::getInventory
static const size_t VT_CHARACTER_IS_ANIMAL   = 0x248;   // Character::isAnimal

static const int ATTACH_BACKPACK_SLOT = 12;
static const int ATTACH_NONE_SLOT     = 7;
static const int ITEM_TYPE_CONTAINER  = 46;
static const int MAX_SEARCH_SECTIONS  = 64;

static const ByteCheck kChecks[] =
{
	{ "getSectionOfType", RVA_INVENTORY_GET_SECTION_OF_TYPE,
	  { 0x44,0x8B,0x49,0x70,0x45,0x33,0xC0,0x45,0x85,0xC9,0x74,0x1E,0x4C,0x8B,0x51,0x78 }, 16 },
	{ "limitedSlot", RVA_INVENTORY_GET_SECTION_OF_TYPE + 0x16,
	  { 0x39,0x90,0xB8,0x00,0x00,0x00 }, 6 },
	{ "isLimitedSlotCompatible", RVA_INVSECTION_LIMITED_COMPATIBLE,
	  { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48 }, 16 },
	{ "isEmpty", RVA_INVENTORY_IS_EMPTY,
	  { 0x33,0xC0,0x39,0x41,0x18,0x0F,0x94,0xC0,0xC3 }, 9 },
	{ "inventory", RVA_GIVE_ITEM_INVENTORY_CHECK,
	  { 0x48,0x83,0xEC,0x30,0x48,0x83,0xB9,0xE8,0x02,0x00,0x00,0x00 }, 12 },
	{ "sectionItems", RVA_GET_BACKPACK_ITEMS_CHECK,
	  { 0x48,0x8B,0x48,0x48,0x48,0x39,0x48,0x40 }, 8 },
	{ "objectType", RVA_BACKPACK_TYPE_CHECK,
	  { 0x83,0xBB,0x3C,0x01,0x00,0x00,0x2E }, 7 },
};

// NULL until every check passed; written once on the main thread before the detour goes in.
static getSectionOfType_t        fn_getSectionOfType        = NULL;
static isLimitedSlotCompatible_t fn_isLimitedSlotCompatible = NULL;

template <typename T>
static T ReadAt(const void* base, size_t off)
{
	return *(const T*)((const char*)base + off);
}

static void* CallVirtualGetter(void* self, size_t slot)
{
	virtualGetter_t fn = *(virtualGetter_t*)(*(const uintptr_t*)self + slot);
	return fn(self);
}

// A section's item vector holds nothing.
static bool SectionEmpty(const void* section)
{
	return ReadAt<const void*>(section, OFF_SECTION_ITEMS_FIRST) == ReadAt<const void*>(section, OFF_SECTION_ITEMS_LAST);
}

namespace keo_inventory {

bool BackpackReaderInit(const char** why)
{
	for (int i = 0; i < (int)(sizeof(kChecks) / sizeof(kChecks[0])); ++i)
	{
		const ByteCheck& c = kChecks[i];
		if (memcmp((const void*)GameAddr(c.rva), c.bytes, (size_t)c.len) != 0)
		{
			if (why)
				*why = c.why;
			return false;
		}
	}
	fn_getSectionOfType        = (getSectionOfType_t)GameAddr(RVA_INVENTORY_GET_SECTION_OF_TYPE);
	fn_isLimitedSlotCompatible = (isLimitedSlotCompatible_t)GameAddr(RVA_INVSECTION_LIMITED_COMPATIBLE);
	return true;
}

void* CharacterInventory(void* character)
{
	return character ? ReadAt<void*>(character, OFF_CHARACTER_INVENTORY) : NULL;
}

void* ItemInventory(void* item)
{
	return item ? CallVirtualGetter(item, VT_ITEM_GET_INVENTORY) : NULL;
}

bool InventoryIsEmpty(void* inventory)
{
	return !inventory || ReadAt<unsigned>(inventory, OFF_INV_ALL_ITEMS_COUNT) == 0;
}

bool CharacterIsAnimal(void* character)
{
	return character && CallVirtualGetter(character, VT_CHARACTER_IS_ANIMAL) != NULL;
}

void* WornBackpack(void* character)
{
	void* inv = CharacterInventory(character);
	if (!inv || !fn_getSectionOfType)
		return NULL;
	void* section = fn_getSectionOfType(inv, ATTACH_BACKPACK_SLOT);
	if (!section || SectionEmpty(section))
		return NULL;
	// SectionItem is { Item* item; ushort x, y, w, h; }: the item is the entry's first qword.
	const void* first = ReadAt<const void*>(section, OFF_SECTION_ITEMS_FIRST);
	void* item = ReadAt<void*>(first, 0);
	if (!item || ReadAt<int>(item, OFF_ITEM_OBJECT_TYPE) != ITEM_TYPE_CONTAINER)
		return NULL;
	return item;
}

void* WornBackpackInventory(void* character)
{
	void* backpack = WornBackpack(character);
	return backpack ? ItemInventory(backpack) : NULL;
}

bool InventoryWouldAutoEquip(void* inventory, void* item)
{
	if (!inventory || !item || !fn_isLimitedSlotCompatible)
		return true;
	const int count = ReadAt<int>(inventory, OFF_INV_SEARCH_COUNT);
	void* const* sections = ReadAt<void* const*>(inventory, OFF_INV_SEARCH_DATA);
	if (!sections || count < 0 || count > MAX_SEARCH_SECTIONS)
		return true;
	for (int i = 0; i < count; ++i)
	{
		void* section = sections[i];
		if (!section)
			continue;
		if (ReadAt<int>(section, OFF_SECTION_LIMITED_SLOT) == ATTACH_NONE_SLOT)
			continue;
		if (!SectionEmpty(section))
			continue;
		if (fn_isLimitedSlotCompatible(section, item))
			return true;
	}
	return false;
}

} // namespace keo_inventory
