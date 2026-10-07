// The backpack-first rules: the routing decision row by row, the slot read rule, the field parser
// and the hand key's text form, the per-character table through its public API on one thread, and
// the food-score and dialogue item-function rules.
#include <cstdio>
#include <cstring>
#include "inventory/backpack_policy.h"
#include "inventory/backpack_table.h"

#include "check.h"

using game::HandKey;
using namespace keo_inventory;

static HandKey Key(unsigned type, unsigned container, unsigned containerSerial, unsigned index, unsigned serial)
{
	HandKey k = { type, container, containerSerial, index, serial };
	return k;
}

static void CheckRoute()
{
	// Arguments: setting, defaultOn, isAnimal, wearsBackpack, itemIsTheBackpack,
	// itemIsNonEmptyContainer, wouldAutoEquip. Each row varies one input from the all-favouring set.
	CHECK(RouteToBackpackFirst(1, true, false, true, false, false, false),
	      "route: setting on, worn backpack, plain item goes to the backpack");
	CHECK(!RouteToBackpackFirst(0, false, false, true, false, false, false)
	      && !RouteToBackpackFirst(0, true, false, true, false, false, false),
	      "route: setting off routes to the original");
	CHECK(RouteToBackpackFirst(-1, true, false, true, false, false, false),
	      "route: no entry follows the default when on");
	CHECK(!RouteToBackpackFirst(-1, false, false, true, false, false, false),
	      "route: no entry follows the default when off");
	CHECK(!RouteToBackpackFirst(0, true, false, true, false, false, false),
	      "route: an entry of 0 overrides a default of on");
	CHECK(RouteToBackpackFirst(1, false, false, true, false, false, false),
	      "route: an entry of 1 overrides a default of off");
	CHECK(!RouteToBackpackFirst(1, true, true, true, false, false, false),
	      "route: an animal never routes");
	CHECK(!RouteToBackpackFirst(1, true, false, false, false, false, false),
	      "route: no worn backpack routes to the original");
	CHECK(!RouteToBackpackFirst(1, true, false, true, true, false, false),
	      "route: the worn backpack itself is never routed");
	CHECK(!RouteToBackpackFirst(1, true, false, true, false, true, false),
	      "route: a container holding items routes to the original");
	// An empty container is a plain item to this rule (vanilla's own backpack try accepts one).
	CHECK(RouteToBackpackFirst(1, true, false, true, false, false, false)
	      && RouteToBackpackFirst(-1, true, false, true, false, false, false),
	      "route: an empty container routes");
	CHECK(!RouteToBackpackFirst(-1, true, false, true, false, false, true),
	      "route: an item an empty equipment slot accepts routes to the original");
	// The equip read answers true whenever it is unsure (an unreadable section list, or an empty
	// section that accepts the item and might still refuse it), and that alone keeps the original.
	CHECK(!RouteToBackpackFirst(1, false, false, true, false, false, true)
	      && !RouteToBackpackFirst(1, true, false, true, false, false, true),
	      "route: any doubt about an equip keeps the original");
}

static void CheckBox()
{
	// Arguments: callbackDataType, ownerDataType, ownerIsAnimal, ownerIsPlayer, ownerWearsThis.
	// 46 is CONTAINER and 1 CHARACTER; each refusing row varies one input from the wanted set.
	CHECK(BackpackBoxWanted(46, 1, false, true, true),
	      "box: a worn backpack of a player character gets the box");
	CHECK(!BackpackBoxWanted(46, -1, false, false, false) && !BackpackBoxWanted(46, 46, false, true, true),
	      "box: a ground backpack (no owner) gets none");
	CHECK(!BackpackBoxWanted(46, 1, false, false, true),
	      "box: an NPC's backpack gets none");
	CHECK(!BackpackBoxWanted(46, 1, true, true, true),
	      "box: an animal's pack gets none");
	CHECK(!BackpackBoxWanted(46, 1, false, true, false),
	      "box: a carried, not worn, backpack gets none");
	CHECK(!BackpackBoxWanted(1, 1, false, true, true) && !BackpackBoxWanted(-1, -1, false, false, false),
	      "box: a character's own window (owner type not a container) gets none");
}

static void CheckSlot()
{
	CHECK(BackpackSlotReadable(0, 0) && BackpackSlotReadable(4, 4), "slot: an even unchanged sequence is readable");
	CHECK(!BackpackSlotReadable(3, 3) && !BackpackSlotReadable(1, 1), "slot: an odd sequence is a write in progress");
	CHECK(!BackpackSlotReadable(2, 4) && !BackpackSlotReadable(2, 3) && !BackpackSlotReadable(0, 2),
	      "slot: a sequence that moved is torn");
}

static int Parse(const char* text, unsigned* out, int max)
{
	return ParseUnsignedFields(text, (int)strlen(text), out, max);
}

static void CheckFields()
{
	unsigned f[8];
	memset(f, 0, sizeof f);
	int n = Parse("1 22 333 4444 4294967295", f, 8);
	CHECK(n == 5 && f[0] == 1 && f[1] == 22 && f[2] == 333 && f[3] == 4444 && f[4] == 4294967295u,
	      "fields: five fields parse");
	memset(f, 0, sizeof f);
	n = Parse("\t7  \t 8\t\t9   ", f, 8);
	CHECK(n == 3 && f[0] == 7 && f[1] == 8 && f[2] == 9, "fields: tabs and repeated spaces separate");
	CHECK(Parse("1 2 x 4", f, 8) == -1 && Parse("12a", f, 8) == -1 && Parse("1 2\r", f, 8) == -1,
	      "fields: a letter fails");
	CHECK(Parse("4294967296", f, 8) == -1 && Parse("1 99999999999999999999999 3", f, 8) == -1,
	      "fields: a value over 32 bits fails");
	CHECK(Parse("1 2 3 4 5 6", f, 5) == -1 && Parse("1 2", f, 1) == -1 && Parse("1 2 3 4 5", f, 5) == 5,
	      "fields: more than max fails");
	CHECK(Parse("", f, 5) == 0 && Parse("  \t ", f, 5) == 0 && ParseUnsignedFields("9", 0, f, 5) == 0,
	      "fields: an empty line is zero fields");
}

static void CheckKey()
{
	const HandKey ref = Key(2, 7, 31, 405, 4294967295u);
	const char* text = "2 7 31 405 4294967295";
	char buf[64];
	int n = HandKeyFormat(ref, buf, (int)sizeof buf);
	HandKey back = Key(11, 0, 0, 0, 0);
	bool parsed = n > 0 && HandKeyParse(buf, n, &back);
	CHECK(n == (int)strlen(text) && strcmp(buf, text) == 0 && parsed && game::HandKeyEqual(back, ref),
	      "key: format then parse is the same key");

	HandKey untouched = Key(3, 3, 3, 3, 3);
	CHECK(!HandKeyParse("2 7 31 405", 10, &untouched) && !HandKeyParse("2 7 31 405 9 1", 14, &untouched)
	      && game::HandKeyEqual(untouched, Key(3, 3, 3, 3, 3)), "key: four fields do not parse");
	CHECK(!HandKeyParse("11 0 0 0 0", 10, &untouched) && game::HandKeyEqual(untouched, Key(3, 3, 3, 3, 3)),
	      "key: the null handle does not parse");

	// The text is 21 characters, so 21 bytes leave no room for the terminator and 22 fit.
	char small[22];
	memset(small, 'Z', sizeof small);
	int w = HandKeyFormat(ref, small, 21);
	bool clean = true;
	for (int i = 0; i < (int)sizeof small; ++i)
		clean = clean && small[i] == 'Z';
	CHECK(w == 0 && clean && HandKeyFormat(ref, small, 22) == 21, "key: a buffer too small writes nothing");
}

static void CheckTable()
{
	const HandKey a = Key(2, 7, 31, 405, 1);
	const HandKey b = Key(2, 7, 31, 406, 2);
	const HandKey c = Key(2, 8, 31, 405, 3);

	// The scan bound only rises, so it is checked first, while the table is untouched: it moves with
	// each slot first taken, and neither an erase nor a clear lowers it.
	bool bound = BackpackFirstSlotCount() == 0 && BackpackFirstGet(a) == -1;
	BackpackFirstSet(a, 1);
	BackpackFirstSet(b, 0);
	bound = bound && BackpackFirstSlotCount() == 2;
	BackpackFirstErase(1);
	bound = bound && BackpackFirstSlotCount() == 2 && BackpackFirstEntryCount() == 1;
	BackpackFirstClear();
	bound = bound && BackpackFirstSlotCount() == 2 && BackpackFirstEntryCount() == 0;
	CHECK(bound, "table: the high-water mark bounds the scan");

	BackpackFirstSet(a, 1);
	BackpackFirstSet(b, 0);
	CHECK(BackpackFirstGet(a) == 1 && BackpackFirstGet(b) == 0, "table: a set key reads back");
	CHECK(BackpackFirstGet(c) == -1 && BackpackFirstGet(Key(2, 7, 31, 405, 9)) == -1,
	      "table: an unknown key reads -1");

	BackpackFirstSet(a, 0);
	CHECK(BackpackFirstGet(a) == 0 && BackpackFirstEntryCount() == 2 && BackpackFirstSlotCount() == 2,
	      "table: set on an existing key updates it");

	HandKey k0 = Key(11, 0, 0, 0, 0);
	int on0 = -1;
	bool slot0 = BackpackFirstEntry(0, &k0, &on0) && game::HandKeyEqual(k0, a) && on0 == 0;
	BackpackFirstErase(0);
	bool freed = !BackpackFirstEntry(0, NULL, NULL) && BackpackFirstGet(a) == -1 && BackpackFirstEntryCount() == 1;
	BackpackFirstSet(c, 1);
	HandKey k1 = Key(11, 0, 0, 0, 0);
	bool reused = BackpackFirstEntry(0, &k1, NULL) && game::HandKeyEqual(k1, c) && BackpackFirstSlotCount() == 2;
	CHECK(slot0 && freed && reused && !BackpackFirstEntry(-1, NULL, NULL)
	      && !BackpackFirstEntry(BACKPACK_TABLE_CAP, NULL, NULL),
	      "table: erase frees the slot and set reuses the lowest");

	BackpackFirstClear();
	CHECK(BackpackFirstEntryCount() == 0 && BackpackFirstGet(a) == -1 && BackpackFirstGet(b) == -1
	      && BackpackFirstGet(c) == -1, "table: clear empties every slot");

	// Slot 0 holds a (1), slot 1 holds b (0): slot 0 re-keyed to c reads under c and no longer under a.
	BackpackFirstSet(a, 1);
	BackpackFirstSet(b, 0);
	BackpackFirstRekey(0, c);
	HandKey k2 = Key(11, 0, 0, 0, 0);
	int on2 = -1;
	CHECK(BackpackFirstGet(c) == 1 && BackpackFirstGet(a) == -1 && BackpackFirstEntry(0, &k2, &on2)
	      && game::HandKeyEqual(k2, c) && on2 == 1, "table: rekey moves an entry to its new key");

	// Slot 0 holds c (1), slot 1 holds b (0): slot 0 re-keyed onto b leaves slot 1's entry and
	// erases slot 0.
	BackpackFirstRekey(0, b);
	CHECK(BackpackFirstGet(b) == 0 && BackpackFirstGet(c) == -1 && !BackpackFirstEntry(0, NULL, NULL)
	      && BackpackFirstEntryCount() == 1, "table: rekey onto a key already present keeps the newer entry");

	BackpackFirstClear();
	bool filled = true;
	for (int i = 0; i < BACKPACK_TABLE_CAP; ++i)
		filled = filled && BackpackFirstSet(Key(2, 1, 1, (unsigned)i, 1000u + (unsigned)i), i & 1);
	const long fullBefore = BackpackFirstFullCount();
	bool refused = !BackpackFirstSet(Key(2, 1, 1, 9999, 9999), 1);
	bool updates = BackpackFirstSet(Key(2, 1, 1, 7, 1007), 0) && BackpackFirstGet(Key(2, 1, 1, 7, 1007)) == 0;
	CHECK(filled && refused && updates && BackpackFirstFullCount() == fullBefore + 1
	      && BackpackFirstSlotCount() == BACKPACK_TABLE_CAP && BackpackFirstEntryCount() == BACKPACK_TABLE_CAP
	      && BackpackFirstGet(Key(2, 1, 1, 9999, 9999)) == -1 && BackpackFirstGet(Key(2, 1, 1, 510, 1510)) == 0
	      && BackpackFirstGet(Key(2, 1, 1, 511, 1511)) == 1, "table: a full table refuses and counts");
	BackpackFirstClear();
	CHECK(BackpackFirstRaceCount() == 0, "table: one thread never races itself");
}

static void CheckFood()
{
	// Arguments: vanillaWants (the original's score above 0), backpackHasFood.
	CHECK(FoodScoreZero(true, true), "food: wants ground food and the backpack has food zeroes");
	CHECK(!FoodScoreZero(true, false), "food: no food in the backpack keeps vanilla");
	CHECK(!FoodScoreZero(false, true) && !FoodScoreZero(false, false),
	      "food: vanilla not wanting is never changed");
}

static void CheckDialog()
{
	// Arguments: mainHas (the main inventory's answer), wearsBackpack.
	CHECK(!DialogUseBackpack(true, true) && !DialogUseBackpack(true, false),
	      "dialog: the main inventory's yes is final");
	CHECK(!DialogUseBackpack(false, false), "dialog: no backpack keeps vanilla's no");
	CHECK(DialogUseBackpack(false, true), "dialog: a main no with a backpack asks the backpack");
}

int main()
{
	CheckRoute();
	CheckBox();
	CheckSlot();
	CheckFields();
	CheckKey();
	CheckTable();
	CheckFood();
	CheckDialog();
	return CheckExit("backpack_policy_units");
}
