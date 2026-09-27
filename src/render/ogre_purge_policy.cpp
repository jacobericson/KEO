#include "render/ogre_purge_policy.h"

OgrePurgeDecision OgrePurgeSkipDecide(bool eligible, double now, double lastRunAt, double maxSkipSeconds)
{
	OgrePurgeDecision d;
	if (!eligible)
	{
		d.action = OGREPURGE_RUN;
		d.countSkip = false;
		d.countFallback = false;
		return d;
	}
	double elapsed = now - lastRunAt;
	if (maxSkipSeconds > 0.0 && elapsed > maxSkipSeconds)
	{
		d.action = OGREPURGE_RUN;
		d.countSkip = false;
		d.countFallback = true;
	}
	else
	{
		d.action = OGREPURGE_SKIP;
		d.countSkip = true;
		d.countFallback = false;
	}
	return d;
}
