#ifndef KEO_FIXES_PAUSED_SKIP_H
#define KEO_FIXES_PAUSED_SKIP_H

// The paused off-screen skip (pausedOffscreenSkip): an entry detour on
// Character::pausedUpdate that, while on, reduces the paused update of a
// character off screen, not in visible-update mode, not carried, not an
// action slave and not a living player character to its own load check,
// except one paused frame in sixteen. The detour and the tick both run on the
// main thread: the tick hands the detour the key, advances the frame counter
// and writes the PausedSkip: line. DEV builds only: PROD compiles both
// functions as stubs.

void InstallPausedSkip(int* installed, int*);
void PausedSkipTick(double now);

#endif // KEO_FIXES_PAUSED_SKIP_H
