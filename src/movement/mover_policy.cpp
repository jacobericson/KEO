#include "movement/mover_policy.h"

int WatchedCapacity(bool registryEnabled)
{
	return registryEnabled ? 64 : 32;
}

int ChooseWatchedEvictSlot(const MoverSlot* slots, int count)
{
	if (!slots || count <= 0)
		return -1;

	int    evictIdx   = -1;
	double oldestTime = 0.0;
	for (int i = 0; i < count; ++i)
	{
		if (slots[i].hasMoveOrder)
			continue;
		if (evictIdx < 0 || slots[i].addedTime < oldestTime)
		{
			evictIdx   = i;
			oldestTime = slots[i].addedTime;
		}
	}
	return evictIdx;
}

bool IsRouteTierMatch(bool hasMoveOrder, int moverX, int moverY,
                      int gridX, int gridY)
{
	return hasMoveOrder && moverX == gridX && moverY == gridY;
}

int ReprioDue(bool flagRequested, double now, double lastReprio,
              double intervalSec, bool hasWork, bool fastCadence)
{
	if (flagRequested)
		return REPRIO_FLAG;
	if (!fastCadence && !hasWork)
		return REPRIO_NONE;
	if (now - lastReprio > intervalSec)
		return REPRIO_TIMER;
	return REPRIO_NONE;
}
