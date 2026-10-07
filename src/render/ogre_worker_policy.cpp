#include "render/ogre_worker_policy.h"

bool OgreMainIdentityOk(unsigned long stamp, unsigned long imageSize)
{
	return stamp == OGRE_TIMESTAMP && imageSize == OGRE_IMAGE_SIZE;
}

bool OgreSpinWanted(long arrived, unsigned long long parties)
{
	return parties >= 2 && parties <= OGRE_MAX_PARTIES && arrived >= 0
	    && (unsigned long long)arrived + 1 < parties;
}

long long OgreSpinTicks(int us, long long freq)
{
	if (us <= 0 || freq <= 0)
		return 0;
	long long t = (long long)us * freq / 1000000;
	return t < 1 ? 1 : t;
}

int OgreWorkerSlots(uintptr_t begin, uintptr_t end)
{
	if (end < begin || (end - begin) % OGRE_WORKER_SLOT_SIZE)
		return -1;
	return (int)((end - begin) / OGRE_WORKER_SLOT_SIZE);
}

bool OgreWorkersUsable(int slots, unsigned long long numWorkers)
{
	return slots >= 1 && slots <= OGRE_MAX_WORKERS && (unsigned long long)slots == numWorkers;
}

bool OgrePriorityRestorable(int saved)
{
	return saved != OGRE_PRIORITY_ERROR;
}

bool OgrePriorityRetryDue(bool on, bool sceneMissing, double now, double lastTry)
{
	return on && sceneMissing && now - lastTry >= OGRE_PRIORITY_RETRY_S;
}
