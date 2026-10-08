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
