#pragma once
#include <float.h>

enum OffscreenAction { OFFSCREEN_NONE, OFFSCREEN_SET, OFFSCREEN_CLEAR };

// What to do with one active effect's non-visible update timeout.
//   state       Effect state: 0 running, 1 stopping, 2 dead.
//   age         game seconds since the effect started (particle simulation time).
//   restarted   the effect was not seen on the previous walk, or its age went
//               down since: a pooled system can come back carrying an old timeout.
//   stale       the timeout it carries is the lever's own and differs from the
//               configured one.
// A stopping effect must run to its end, so it never keeps a timeout. A
// running looping effect gets one only once it is older than minAge, so it is
// never frozen before its particles have filled out; until then, and after a
// restart, any timeout it carries is cleared. A stale timeout is set again.
inline OffscreenAction OffscreenTimeoutAction(bool looping, int state, bool timeoutSet,
                                              float age, float minAge, bool restarted,
                                              bool stale = false)
{
	if (state == 1)
		return timeoutSet ? OFFSCREEN_CLEAR : OFFSCREEN_NONE;
	if (state != 0 || !looping || !(age >= 0.0f && age <= FLT_MAX))
		return OFFSCREEN_NONE;
	if (restarted || age < minAge)
		return timeoutSet ? OFFSCREEN_CLEAR : OFFSCREEN_NONE;
	return (timeoutSet && !stale) ? OFFSCREEN_NONE : OFFSCREEN_SET;
}
