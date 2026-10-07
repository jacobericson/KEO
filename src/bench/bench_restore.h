#pragma once

// Restores a run could not make at its end: the speed (refused behind a menu
// or during a transition) and, after a save load, the keyboard-camera flag.
// They complete once no save load, menu or transition is in the way. Main
// thread only.
struct BenchPendingRestore
{
	bool  speed;
	float speedValue;
	bool  paused;
	bool  kbd;
	bool  kbdValue;
};

bool BenchRestoreGateClear(bool saveLoading);           // no save load, menu or transition
void BenchRestoreQueue(const BenchPendingRestore& p);   // replaces any queued restore
bool BenchRestorePending();
void BenchRestoreDrop();                                // at quit: nothing is restored
void BenchRestoreTick(bool saveLoading);                // completes a queued restore once the gate clears

// How a run that set the speed leaves it at its end: restored to the user's
// (BSE_RESTORE), left at the user's own change (BSE_USER_CHANGE), or, when the
// user unpaused the run's own pause, set to the user's (BSE_USER_UNPAUSE). The
// user's speed is the one at the run's start. A restore the gate refuses is
// queued in *pend.
enum BenchSpeedEndKind { BSE_RESTORE, BSE_USER_CHANGE, BSE_USER_UNPAUSE };
struct BenchSpeedEnd
{
	bool  left;        // the user's own change stands
	bool  resumeSet;   // paused by the user: the user's speed is the one the pause key resumes at
	bool  unpaused;    // the user's speed is set (or queued) after an unpause
	float unpausedAt, pauseKey, set;
};
void BenchEndSpeed(BenchSpeedEndKind how, float userSpeed, float userNormal, bool userPaused, bool worldGone,
                   bool saveLoading, BenchPendingRestore* pend, BenchSpeedEnd* out);
