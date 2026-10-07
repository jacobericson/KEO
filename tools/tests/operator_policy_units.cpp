// The operator hold's pure half: each guard alone sends the operator to deliver, the first failure
// in the gather order is the one reported, the answer changes only on a hold, the conjunction agrees
// with the reason, jamming is no input, and the heartbeat's buckets and names.
#include <cstdio>
#include <cstring>
#include "inventory/operator_policy.h"

#include "check.h"

using namespace keo_inventory;

static OperatorFacts AllHold()
{
	OperatorFacts f;
	f.vanillaTrue = true;
	f.isPlayer    = true;
	f.haveMachine = true;
	f.ownsMachine = true;
	f.isResource  = true;
	f.powered     = true;
	f.inputsValid = true;
	f.hungry      = false;
	f.haveProduct = true;
	f.hasRoom     = true;
	return f;
}

// One guard flipped from its holding value: its own reason, and vanilla's true answer kept.
static bool DeliversWith(OperatorFacts f, OperatorReason expect)
{
	const OperatorReason r = OperatorReasonOf(f);
	return r == expect && OperatorAnswer(true, r) == true;
}

static void CheckGuards()
{
	const OperatorFacts all = AllHold();
	CHECK(OperatorReasonOf(all) == OR_HOLD && OperatorAnswer(true, OperatorReasonOf(all)) == false,
	      "hold: every guard holds");

	OperatorFacts f;
	f = all; f.isPlayer = false;
	CHECK(DeliversWith(f, OR_NOT_PLAYER), "guard: not player delivers");
	f = all; f.haveMachine = false;
	CHECK(DeliversWith(f, OR_NO_MACHINE), "guard: no machine delivers");
	f = all; f.ownsMachine = false;
	CHECK(DeliversWith(f, OR_NOT_OWNED), "guard: not owned delivers");
	f = all; f.isResource = false;
	CHECK(DeliversWith(f, OR_NOT_RESOURCE), "guard: gear output delivers");
	f = all; f.powered = false;
	CHECK(DeliversWith(f, OR_UNPOWERED), "guard: unpowered delivers");
	f = all; f.inputsValid = false;
	CHECK(DeliversWith(f, OR_INPUTS_INVALID), "guard: invalid inputs deliver");
	f = all; f.hungry = true;
	CHECK(DeliversWith(f, OR_HUNGRY), "guard: hungry delivers");
	f = all; f.haveProduct = false;
	CHECK(DeliversWith(f, OR_NO_PRODUCT), "guard: no product delivers");
	f = all; f.hasRoom = false;
	CHECK(DeliversWith(f, OR_NO_ROOM), "guard: no room delivers");
}

static void CheckVanillaFalse()
{
	OperatorFacts f = AllHold();
	f.vanillaTrue = false;
	bool ok = OperatorReasonOf(f) == OR_VANILLA_FALSE && OperatorAnswer(false, OperatorReasonOf(f)) == false;
	for (int r = 0; r < OR_COUNT; ++r)
		ok = ok && OperatorAnswer(false, (OperatorReason)r) == false;
	CHECK(ok, "vanilla false is never changed");
}

static void CheckOrder()
{
	OperatorFacts f = AllHold();
	f.isPlayer = false;
	f.hasRoom  = false;
	bool ok = OperatorReasonOf(f) == OR_NOT_PLAYER;
	f = AllHold();
	f.hungry  = true;
	f.powered = false;
	ok = ok && OperatorReasonOf(f) == OR_UNPOWERED;
	f = AllHold();
	f.hungry      = true;
	f.haveProduct = false;
	ok = ok && OperatorReasonOf(f) == OR_HUNGRY;
	CHECK(ok, "order: the first failure is reported");
}

static void CheckAnswer()
{
	CHECK(OperatorAnswer(true, OR_HOLD) == false, "answer: a hold is false");
	bool ok = true;
	for (int r = OR_HOLD + 1; r < OR_COUNT; ++r)
		ok = ok && OperatorAnswer(true, (OperatorReason)r) == true
		        && OperatorAnswer(false, (OperatorReason)r) == false;
	CHECK(ok, "answer: any other reason is vanilla's");
}

static void CheckConjunction()
{
	bool ok = true;
	for (int m = 0; m < 256; ++m)
	{
		OperatorFacts f;
		f.vanillaTrue = (m & 1) != 0;
		f.isPlayer    = (m & 2) != 0;
		f.ownsMachine = (m & 4) != 0;
		f.isResource  = (m & 8) != 0;
		f.hasRoom     = (m & 16) != 0;
		f.powered     = (m & 32) != 0;
		f.inputsValid = (m & 64) != 0;
		f.hungry      = (m & 128) != 0;
		f.haveMachine = true;
		f.haveProduct = true;
		const bool hold = HoldDelivery(f.vanillaTrue, f.isPlayer, f.ownsMachine, f.isResource,
		                               f.hasRoom, f.powered, f.inputsValid, f.hungry);
		ok = ok && hold == (OperatorReasonOf(f) == OR_HOLD);
	}
	CHECK(ok, "conjunction: HoldDelivery matches OperatorReasonOf");
}

static void CheckNoJammed()
{
	// Ten bools and nothing else: a jamming fact would be an eleventh member.
	Check(sizeof(OperatorFacts) == 10, "jammed is not an input");
}

static void CheckBuckets()
{
	CHECK(HaulBucket(5) == 5, "bucket: 5 is 5");
	CHECK(HaulBucket(40) == 16 && HaulBucket(16) == 16 && HaulBucket(15) == 15,
	      "bucket: 40 is 16");
	CHECK(HaulBucket(-1) == 0 && HaulBucket(0) == 0, "bucket: negative is 0");
	CHECK(HAUL_BUCKETS == 17 && HaulBucket(1000000) == HAUL_BUCKETS - 1, "bucket: the largest is HAUL_BUCKETS - 1");
}

static void CheckNames()
{
	static const char* const kNames[OR_COUNT] =
	{
		"hold", "vanilla", "notPlayer", "noMachine", "notOwned", "notResource", "unpowered",
		"inputs", "hungry", "noProduct", "noRoom"
	};
	bool ok = true;
	for (int r = 0; r < OR_COUNT; ++r)
	{
		const char* n = OperatorReasonName((OperatorReason)r);
		ok = ok && n && strcmp(n, kNames[r]) == 0;
	}
	ok = ok && OperatorReasonName(OR_COUNT) && strcmp(OperatorReasonName(OR_COUNT), "?") == 0;
	CHECK(ok, "names: every reason has a name");
}

int main()
{
	CheckGuards();
	CheckVanillaFalse();
	CheckOrder();
	CheckAnswer();
	CheckConjunction();
	CheckNoJammed();
	CheckBuckets();
	CheckNames();
	return CheckExit("operator_policy_units");
}
