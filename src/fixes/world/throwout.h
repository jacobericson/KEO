#ifndef KEO_FIXES_THROWOUT_H
#define KEO_FIXES_THROWOUT_H

// The throw-out loop's fix. A post-hook on Character::getDropped writes the body's walls flag
// from the gate code at its position; detours on the two throw-out task slots mark their drops,
// which the post-hook records in the hold table; the finder, replaced, skips a held body. The
// detours run on the carrier's update thread (the AI back thread, or the main thread): no lock,
// no allocation, no logging; counters are Interlocked. The tick and the install run on the main
// thread.

namespace fixes {

// A kInstallSteps entry: the four rows while throwOutFix is on.
void InstallThrowout(int* installed, int* total);
// Main thread, from the camera tick's list. Returns at once while throwOutFix is off. Its first
// call decides the finder's mode once per process (whether a plugin known to hook the finder is
// loaded) and logs it. Then: a save load's rising edge clears the table; once a second, it ends
// a hold whose body woke, is gone or passed the cap; once a minute, the "Throwout:" heartbeat.
void ThrowoutTick(double now, bool saveLoading);

} // namespace fixes

#endif // KEO_FIXES_THROWOUT_H
