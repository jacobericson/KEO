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
