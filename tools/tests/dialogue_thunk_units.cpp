// The dialogue item-function patch's pure half: the call site's lead bytes against this suite's own
// copy of them, the thunk's bytes range by range, the call's rel32 reach, the original call's
// target through the j_ thunk, and the hooked-head rule the food-score install applies to
// getNumFoodItems.
#include <cstdio>
#include <cstring>
#include "inventory/dialogue_thunk_policy.h"
#include "game/prologue_policy.h"

#include "check.h"

using namespace keo_inventory;

// DialogLineData::checkConditions 0x678998..0x6789AE, as read from the binary.
static const unsigned char kLead[23] =
{
	0x49, 0x8B, 0x04, 0x24, 0x49, 0x8B, 0xCC, 0xFF, 0x90, 0x60, 0x01, 0x00,
	0x00, 0x8B, 0xD3, 0x48, 0x8B, 0xC8, 0xE8, 0x63, 0x2E, 0x9D, 0xFF
};

// Inventory::getNumFoodItems's first 16 bytes, as read from the binary.
static const unsigned char kFoodHead[16] =
{
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xC2, 0xBA, 0x0F, 0x00
};

static void CheckLead()
{
	CHECK(kDialogLeadLen == 23 && memcmp(kDialogLeadBytes, kLead, sizeof(kLead)) == 0
	      && DialogLeadMatches(kLead),
	      "lead: the IDB bytes match");
	static const int kFlipAt[4] = { 0, 5, 18, 22 };
	bool refused = !DialogLeadMatches(NULL);
	for (int i = 0; i < 4; ++i)
	{
		unsigned char b[23];
		memcpy(b, kLead, sizeof(b));
		b[kFlipAt[i]] ^= 0x01;
		refused = refused && !DialogLeadMatches(b);
	}
	CHECK(refused, "lead: one changed byte refuses");
}

static void CheckThunk()
{
	const unsigned __int64 wrapper = 0x1122334455667788ULL;
	unsigned char t[kDialogThunkLen + 4];
	memset(t, 0xEE, sizeof(t));
	const bool built = DialogThunkBuild(t, sizeof(t), wrapper);
	CHECK(built && kDialogThunkLen == 17 && t[0] == 0x4D && t[1] == 0x8B && t[2] == 0xC4,
	      "thunk: starts mov r8, r12");
	CHECK(built && t[3] == 0xFF && t[4] == 0x25 && t[5] == 0x00 && t[6] == 0x00 && t[7] == 0x00
	      && t[8] == 0x00,
	      "thunk: jmp [rip+0] follows");
	CHECK(built && t[9] == 0x88 && t[10] == 0x77 && t[11] == 0x66 && t[12] == 0x55 && t[13] == 0x44
	      && t[14] == 0x33 && t[15] == 0x22 && t[16] == 0x11 && t[17] == 0xEE,
	      "thunk: the wrapper address is little-endian at 9");
	unsigned char s[kDialogThunkLen];
	CHECK(!DialogThunkBuild(s, kDialogThunkLen - 1, wrapper) && !DialogThunkBuild(NULL, 64, wrapper)
	      && DialogThunkBuild(s, kDialogThunkLen, wrapper),
	      "thunk: refuses a short buffer");
	CHECK(!DialogThunkBuild(s, sizeof(s), 0), "thunk: refuses a null wrapper");
}

static void CheckRel32()
{
	const unsigned __int64 call = 0x1406789AAULL;   // next instruction 0x1406789AF
	int rel = 0;
	CHECK(DialogCallRel32(call, 0x1006789AFULL, &rel) && rel == -0x40000000,
	      "rel32: reaches a thunk 1 GB below");
	rel = 0;
	CHECK(DialogCallRel32(call, 0x150000000ULL, &rel) && rel == 0x0F987651
	      && call + 5 + (unsigned __int64)(__int64)rel == 0x150000000ULL,
	      "rel32: reaches a thunk above the call");
	rel = 12345;
	CHECK(!DialogCallRel32(call, call + 5 + 0xC0000000ULL, &rel)
	      && !DialogCallRel32(call, call + 5 - 0xC0000000ULL, &rel) && rel == 12345,
	      "rel32: refuses a thunk 3 GB away");
}

static void CheckHead()
{
	unsigned char b[16];
	memcpy(b, kFoodHead, sizeof(b));
	CHECK(ClassifyPrologue(b, kFoodHead, true) == PROLOGUE_ORIGINAL,
	      "head: the original getNumFoodItems head is accepted");

	memcpy(b, kFoodHead, sizeof(b));
	b[0] = 0xE9; b[1] = 0x10; b[2] = 0x20; b[3] = 0x30; b[4] = 0x40;
	CHECK(ClassifyPrologue(b, kFoodHead, true) == PROLOGUE_SHARED,
	      "head: another plugin's E9 over getNumFoodItems with the tail intact is accepted");

	memcpy(b, kFoodHead, sizeof(b));
	b[0] = 0xFF; b[1] = 0x25; b[2] = 0x00; b[3] = 0x10; b[4] = 0x00; b[5] = 0x00;
	CHECK(ClassifyPrologue(b, kFoodHead, true) == PROLOGUE_SHARED,
	      "head: an FF 25 head with the tail intact is accepted");

	memcpy(b, kFoodHead, sizeof(b));
	b[0] = 0xE9; b[1] = 0x10; b[2] = 0x20; b[3] = 0x30; b[4] = 0x40;
	b[10] ^= 0x01;
	CHECK(ClassifyPrologue(b, kFoodHead, true) == PROLOGUE_MISMATCH,
	      "head: an E9 head with a changed tail refuses");

	memcpy(b, kFoodHead, sizeof(b));
	b[0] = 0x90;
	CHECK(ClassifyPrologue(b, kFoodHead, true) == PROLOGUE_MISMATCH,
	      "head: a changed first byte without a jump refuses");
}

// j_Inventory__hasItemFunction at 0x4B812, as read from the binary: jmp 0x7456C0.
static const unsigned char kJThunk[5] = { 0xE9, 0xA9, 0x9E, 0x6F, 0x00 };

static void CheckTarget()
{
	CHECK(DialogueOriginalCallTarget(0x6789AAULL, kLead) == 0x4B812ULL,
	      "target: the lead's call reaches the j_ thunk");
	unsigned char moved[23];
	memcpy(moved, kLead, sizeof(moved));
	moved[19] ^= 0x10;
	CHECK(DialogueOriginalCallTarget(0x6789AAULL, moved) == 0x4B812ULL + 0x10,
	      "target: a changed rel32 byte moves the target");
	unsigned __int64 to = 0;
	CHECK(DialogJumpTarget(0x4B812ULL, kJThunk, &to) && to == 0x7456C0ULL,
	      "target: the j_ thunk jumps to hasItemFunction");
	unsigned char notJmp[5] = { 0xE8, 0xA9, 0x9E, 0x6F, 0x00 };
	to = 7;
	CHECK(!DialogJumpTarget(0x4B812ULL, notJmp, &to) && to == 7 && !DialogJumpTarget(0x4B812ULL, NULL, &to),
	      "target: a thunk that is not an E9 refuses");
}

int main()
{
	CheckLead();
	CheckThunk();
	CheckRel32();
	CheckTarget();
	CheckHead();
	return CheckExit("dialogue_thunk_units");
}
