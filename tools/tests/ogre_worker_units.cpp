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

int main()
{
	CheckIdentity();
	CheckSpin();
	CheckTicks();
	return CheckExit("ogre_worker_units");
}
