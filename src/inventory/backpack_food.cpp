// backpack_food.cpp - The post-hook on AI::scoreFindFoodOnGround: vanilla scores walking to ground
// food only while the main inventory holds none the character can eat; this also asks the worn
// backpack. The install step runs on the main thread at startup; the post-hook runs wherever the
// AI scores its goals.
#include "inventory/backpack_food.h"
#include "inventory/backpack_policy.h"
#include "inventory/backpack_reader.h"
#include "inventory/byte_check_policy.h"
#include "inventory/dialogue_item_function.h"
#include "plugin/hook_manifest.h"
#include "game/game.h"
#include "game/klib_member_contract.h"
#include "base/core.h"
#include <string>

namespace backpack_food_detail {
typedef float (__fastcall *scoreFindFoodOnGround_t)(void* ai, const void* subject, const void* v);
typedef int   (__fastcall *getNumFoodItems_t)(void* inventory, void* race);
} // namespace backpack_food_detail
using namespace backpack_food_detail;

namespace keo_inventory {

static scoreFindFoodOnGround_t orig_scoreFindFoodOnGround = NULL;
static getNumFoodItems_t       fn_getNumFoodItems         = NULL;
static const size_t kAiMe = KLIB_OFF_AI_me;
// Inventory::getNumFoodItems's first bytes in this build. The binding is used when they match, or
// when another plugin's jump sits in front of a matching tail.
static const unsigned char kGetNumFoodItemsHead[16] =
	{ 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x4C,0x8B,0xC2,0xBA,0x0F,0x00 };
static volatile LONG s_foodCalls = 0, s_foodZeroed = 0;

// AI back thread (the main thread with characterMultithreading off). Vanilla's score, or 0 when it
// is at least 1 (a walk to ground food; lower scores are kept) and the worn backpack holds food
// this character can eat. No lock, no allocation, no logging.
static float __fastcall hook_scoreFindFoodOnGround(void* ai, const void* subject, const void* v)
{
	float r = orig_scoreFindFoodOnGround(ai, subject, v);
	InterlockedIncrement(&s_foodCalls);
	if (r < 1.0f || !ai)
		return r;
	void* me = *(void* const*)((const char*)ai + kAiMe);
	void* bag = me ? WornBackpackInventory(me) : NULL;
	if (!FoodScoreZero(r, bag && fn_getNumFoodItems(bag, me) > 0))
		return r;
	InterlockedIncrement(&s_foodZeroed);
	return 0.0f;
}

void InstallBackpackFood(int* installed, int*)
{
	if (!HookRowWanted(HOOK_SCORE_FIND_FOOD_ON_GROUND)) return;
	// The reader binds idempotently; unbound, the post-hook could never see a backpack.
	const char* reader = NULL;   // set only when the reader refuses
	const bool readerBound = BackpackReaderInit(&reader);
	// The callee's head is checked before the post-hook can reach it, under the gate's own
	// shared-site rule: an E9 or FF 25 jump over a matching tail is another plugin's detour, and
	// calling the exe address still reaches the count.
	const char* why = NULL;
	ByteRowLog rows;
	ByteRowLogReset(&rows);
	if (!readerBound)
		why = "reader refused: ";
	else if (!ByteRowCheck(&rows, "getNumFoodItems", BYTE_CHECK_CALLEE_HEAD,
	                       (const unsigned char*)GameAddr(RVA_INVENTORY_GET_NUM_FOOD_ITEMS), kGetNumFoodItemsHead, 16))
		why = rows.why;
	else
	{
		// Before the install: the post-hook calls it as soon as it is in.
		fn_getNumFoodItems = (getNumFoodItems_t)GameAddr(RVA_INVENTORY_GET_NUM_FOOD_ITEMS);
		why = HookInstall(HOOK_SCORE_FIND_FOOD_ON_GROUND, hook_scoreFindFoodOnGround, &orig_scoreFindFoodOnGround, installed, true);
	}
	if (!why)
	{
		LogMsg(std::string("BackpackFood: installed shared=") + ByteRowShared(&rows));
	}
	else
	{
		orig_scoreFindFoodOnGround = NULL;
		fn_getNumFoodItems = NULL;
		LogError(std::string("BackpackFood: not installed (") + why + (reader ? reader : "") + "); food scoring stays vanilla");
	}
}

long BackpackFoodCalls()
{
	return InterlockedCompareExchange(&s_foodCalls, 0, 0);
}

void BackpackFoodCountersRead(long* foodZeroed, long* dialogCalls, long* dialogBackpackHits)
{
	long calls = 0, hits = 0;
	DialogueItemFunctionCounters(&calls, &hits);
	if (foodZeroed)
		*foodZeroed = InterlockedCompareExchange(&s_foodZeroed, 0, 0);
	if (dialogCalls)
		*dialogCalls = calls;
	if (dialogBackpackHits)
		*dialogBackpackHits = hits;
}

} // namespace keo_inventory
