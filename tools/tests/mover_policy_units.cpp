#include <cstdio>
#include "movement/mover_policy.h"

#include "check.h"

int main()
{
	// Capacity follows the registry key.
	{
		Check(WatchedCapacity(true) == 64, "registry on keeps 64 entries");
		Check(WatchedCapacity(false) == 32, "registry off keeps 32 entries");
	}

	// Eviction takes the oldest entry with no move order.
	{
		MoverSlot s[4];
		s[0].hasMoveOrder = true;  s[0].addedTime = 1.0;
		s[1].hasMoveOrder = false; s[1].addedTime = 9.0;
		s[2].hasMoveOrder = false; s[2].addedTime = 4.0;
		s[3].hasMoveOrder = true;  s[3].addedTime = 0.5;
		Check(ChooseWatchedEvictSlot(s, 4) == 2, "oldest order-less entry is chosen");
	}

	// A character carrying a move order is never evicted, even when it is the
	// oldest entry of all.
	{
		MoverSlot s[3];
		s[0].hasMoveOrder = true; s[0].addedTime = 0.0;
		s[1].hasMoveOrder = true; s[1].addedTime = 1.0;
		s[2].hasMoveOrder = true; s[2].addedTime = 2.0;
		Check(ChooseWatchedEvictSlot(s, 3) == -1, "every entry carrying an order means no eviction");
	}

	// Degenerate inputs.
	{
		MoverSlot s[1];
		s[0].hasMoveOrder = false; s[0].addedTime = 3.0;
		Check(ChooseWatchedEvictSlot(s, 0) == -1, "empty registry has no slot to evict");
		Check(ChooseWatchedEvictSlot(0, 1) == -1, "null registry has no slot to evict");
		Check(ChooseWatchedEvictSlot(s, 1) == 0, "the only order-less entry is chosen");
	}

	// A negative addedTime (a clock read before the first stamp) still ranks
	// as older than a positive one.
	{
		MoverSlot s[2];
		s[0].hasMoveOrder = false; s[0].addedTime =  5.0;
		s[1].hasMoveOrder = false; s[1].addedTime = -1.0;
		Check(ChooseWatchedEvictSlot(s, 2) == 1, "the smallest addedTime wins");
	}

	// Route tier: order plus the zone the mover stands in.
	{
		Check(IsRouteTierMatch(true, 7, 8, 7, 8), "mover with an order in this zone matches");
		Check(!IsRouteTierMatch(false, 7, 8, 7, 8), "mover without an order never matches");
		Check(!IsRouteTierMatch(true, 7, 9, 7, 8), "a different zone never matches");
		Check(!IsRouteTierMatch(true, 6, 8, 7, 8), "a different column never matches");
	}

	// Reprio: a request beats the timer and fires however recent the last pass.
	{
		Check(ReprioDue(true, 100.0, 99.99, 1.0, false, true) == REPRIO_FLAG,
		      "a requested pass fires immediately");
		Check(ReprioDue(true, 100.0, 99.99, 3.0, false, false) == REPRIO_FLAG,
		      "a requested pass fires on the slow cadence too");
	}

	// Fast cadence: the interval alone decides, tracked work or not.
	{
		Check(ReprioDue(false, 101.5, 100.0, 1.0, false, true) == REPRIO_TIMER,
		      "fast cadence fires past the interval with nothing tracked");
		Check(ReprioDue(false, 100.5, 100.0, 1.0, true, true) == REPRIO_NONE,
		      "fast cadence waits out the interval");
	}

	// Slow cadence: the legacy gate, and the longer interval.
	{
		Check(ReprioDue(false, 105.0, 100.0, 3.0, false, false) == REPRIO_NONE,
		      "slow cadence stays idle while nothing is tracked or queued");
		Check(ReprioDue(false, 105.0, 100.0, 3.0, true, false) == REPRIO_TIMER,
		      "slow cadence fires past the interval when work exists");
		Check(ReprioDue(false, 102.0, 100.0, 3.0, true, false) == REPRIO_NONE,
		      "slow cadence waits out its longer interval");
	}

	// Retention's moving flag: a step needs a move order behind it.
	{
		Check(MoverRetainMoving(true, true), "a step on a move order is moving");
		Check(!MoverRetainMoving(true, false), "a baseline entry's stale step is not moving");
		Check(!MoverRetainMoving(false, true), "an order with no step left is not moving");
		Check(!MoverRetainMoving(false, false), "a parked baseline entry is not moving");
	}

	return CheckExit("mover_policy_units");
}
