#include "render/ogre_worker_policy.h"

#include "check.h"

static void CheckIdentity()
{
	Check(OgreMainIdentityOk(0x5CA5F929, 0x9C9000), "identity: the Kenshi OgreMain build matches");
	Check(!OgreMainIdentityOk(0x5CA5F92A, 0x9C9000), "identity: another timestamp refuses");
	Check(!OgreMainIdentityOk(0x5CA5F929, 0x9CA000), "identity: another image size refuses");
}

static void CheckSpin()
{
	Check(OgreSpinWanted(0, 16), "spin: main arriving first at a 16-party join waits");
	Check(OgreSpinWanted(14, 16), "spin: 14 of 15 others arrived still waits");
	Check(!OgreSpinWanted(15, 16), "spin: all 15 others arrived does not wait");
	Check(OgreSpinWanted(0, 2), "spin: a two-party barrier waits for its one worker");
	Check(!OgreSpinWanted(1, 2), "spin: a two-party barrier does not wait once it arrived");
	Check(!OgreSpinWanted(0, 1), "spin: a one-party barrier never waits");
	Check(!OgreSpinWanted(0, 129), "spin: a party count above the cap never waits");
	Check(!OgreSpinWanted(-1, 16) && !OgreSpinWanted(16, 16), "spin: a negative or overfull count never waits");
}

static void CheckTicks()
{
	Check(OgreSpinTicks(20, 10000000) == 200, "ticks: 20 us at 10 MHz is 200");
	Check(OgreSpinTicks(0, 10000000) == 0 && OgreSpinTicks(-5, 10000000) == 0
	      && OgreSpinTicks(20, 0) == 0 && OgreSpinTicks(20, -1) == 0,
	      "ticks: 0 us or no frequency is 0");
	Check(OgreSpinTicks(1, 100000) == 1, "ticks: a budget below one tick is one");
	Check(OgreSpinTicks(50, 3000000000LL) == 150000, "ticks: 50 us at a 3 GHz counter does not overflow");
}

static void CheckSlots()
{
	const uintptr_t base = 0x10000000;
	Check(OgreWorkerSlots(base, base + 240) == 15, "slots: 15 workers");
	Check(OgreWorkerSlots(base, base) == 0, "slots: an empty vector is 0");
	Check(OgreWorkerSlots(base, base + 24) == -1, "slots: a partial slot is -1");
	Check(OgreWorkerSlots(base + 32, base) == -1, "slots: a reversed range is -1");
}

static void CheckUsable()
{
	Check(OgreWorkersUsable(15, 15), "usable: 15 of 15");
	Check(!OgreWorkersUsable(15, 14), "usable: a count that differs refuses");
	Check(!OgreWorkersUsable(0, 0) && !OgreWorkersUsable(65, 65), "usable: none or more than 64 refuses");
}

static void CheckRestore()
{
	Check(OgrePriorityRestorable(-2) && OgrePriorityRestorable(0) && OgrePriorityRestorable(1)
	      && OgrePriorityRestorable(15),
	      "restore: a read priority is restorable");
	Check(!OgrePriorityRestorable(0x7FFFFFFF), "restore: the error value is not");
}

static void CheckRetry()
{
	Check(OgrePriorityRetryDue(true, true, 11.0, 10.0), "retry: on with no scene manager, a second later, retries");
	Check(!OgrePriorityRetryDue(true, true, 10.5, 10.0), "retry: not within a second of the last try");
	Check(!OgrePriorityRetryDue(false, true, 20.0, 10.0), "retry: never while the switch is off");
	Check(!OgrePriorityRetryDue(true, false, 20.0, 10.0), "retry: never once a scene manager was found");
}

int main()
{
	CheckIdentity();
	CheckSpin();
	CheckTicks();
	CheckSlots();
	CheckUsable();
	CheckRestore();
	CheckRetry();
	return CheckExit("ogre_worker_units");
}
