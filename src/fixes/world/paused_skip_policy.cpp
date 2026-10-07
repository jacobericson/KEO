// paused_skip_policy.cpp - The paused off-screen skip's pure rules.
#include "fixes/world/paused_skip_policy.h"

// A visible character keeps its whole update; so does one whose update must
// re-snap it (carried, an action slave) or would read a missing animation; a
// living player character keeps the cell lease the update takes. The player
// test is the engine's own and runs last of the three, as the update runs it.
PausedSkipVerdict PausedSkipDecide(const PausedSkipInputs& in, void* ch, unsigned long frame,
                                   PausedSkipPlayerFn isPlayer)
{
	if (in.onScreen || in.visUpdate)
		return PSK_RUN_VISIBLE;
	if (!in.haveAnimation || in.carried || in.actionSlave)
		return PSK_RUN_KEPT;
	if (!in.dead && isPlayer(ch))
		return PSK_RUN_PLAYER;
	if (PausedSkipDue((uintptr_t)ch, frame))
		return PSK_RUN_DUE;
	return PSK_SKIP;
}

// The page-offset term spreads characters allocated at a fixed page stride
// over the period; 64-bit arithmetic keeps the period across a wrap of the
// 32-bit frame counter, since 2^32 is a multiple of the period.
bool PausedSkipDue(uintptr_t ch, unsigned long frame)
{
	unsigned long long h = ((unsigned long long)ch >> 4) ^ ((unsigned long long)ch >> 12);
	return ((unsigned long long)frame + h) % PSK_PERIOD == 0;
}
