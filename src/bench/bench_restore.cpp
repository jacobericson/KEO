#include "bench/bench_restore.h"
#include "bench/bench_game.h"
#include "bench/bench_game_math.h"
#include "base/core.h"
#include <stdio.h>

namespace bench_restore_detail {
bool                s_active = false;
BenchPendingRestore s_pending = { false, 0.0f, false, false, false };
}
using namespace bench_restore_detail;

bool BenchRestoreGateClear(bool saveLoading)
{
	return !saveLoading && BenchMenusClear() && BenchTransitionClear();
}

void BenchRestoreQueue(const BenchPendingRestore& p)
{
	s_pending = p;
	s_active = true;
}

bool BenchRestorePending()
{
	return s_active;
}

void BenchRestoreDrop()
{
	s_active = false;
}

// A save load does not reset the speed (only setGameSpeed and userPause
// write it), so a pending restore outlives one and completes after it.
void BenchRestoreTick(bool saveLoading)
{
	if (!s_active || !BenchRestoreGateClear(saveLoading))
		return;
	if (s_pending.speed && !BenchRestoreSpeed(s_pending.speedValue, s_pending.paused))
		return;
	if (s_pending.kbd)
		BenchSetKeyboardCamera(s_pending.kbdValue);
	s_active = false;

	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	char buf[128];
	if (s_pending.speed)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Bench: restored speed=%.2f paused=%d qpc=%lld", s_pending.speedValue,
		            s_pending.paused ? 1 : 0, (long long)q.QuadPart);
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Bench: restored qpc=%lld", (long long)q.QuadPart);
	LogMsg(buf);
}

void BenchEndSpeed(BenchSpeedEndKind how, float userSpeed, float userNormal, bool userPaused, bool worldGone,
                   bool saveLoading, BenchPendingRestore* pend, BenchSpeedEnd* out)
{
	BenchSpeedEnd none = { false, false, false, 0.0f, 0.0f, 0.0f };
	*out = none;
	float speed = userPaused ? userNormal : userSpeed;
	if (how == BSE_USER_UNPAUSE)
	{
		// Whatever route ended the run's pause (the pause key, the play button,
		// a speed key), the run ends at the user's speed.
		out->unpaused = true;
		out->unpausedAt = BenchGetSpeed();
		out->pauseKey = BenchGetPausedResumeSpeed();
		out->set = BenchUnpauseSpeed(userSpeed, userNormal, userPaused);
		if (!BenchRestoreGateClear(saveLoading) || !BenchSetSpeed(out->set))
		{
			pend->speed = true;
			pend->speedValue = out->set;
			pend->paused = false;
		}
	}
	else if (how == BSE_USER_CHANGE)
	{
		// A pause (the key or a dialogue) saved the run's speed as the one to
		// resume at; the user's goes back in its place. A speed change stands.
		out->left = true;
		out->resumeSet = !(BenchGetSpeed() > 0.0f) && BenchSetPausedResumeSpeed(speed);
	}
	else if (worldGone || !BenchRestoreGateClear(saveLoading) || !BenchRestoreSpeed(speed, userPaused))
	{
		pend->speed = true;
		pend->speedValue = speed;
		pend->paused = userPaused;
	}
}
