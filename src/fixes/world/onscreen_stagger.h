#ifndef KEO_FIXES_ONSCREEN_STAGGER_H
#define KEO_FIXES_ONSCREEN_STAGGER_H

// The far visibility-check stagger (onScreenStagger): an entry detour on
// Character::updateOnScreenCheck that, while on, answers a far character the
// last check left off screen with the far branch's own writes on three AI runs
// in four, and lets the engine's full check run on the fourth, on a camera
// jump, after a save load and for anything nearer or visible. The detour runs
// on the AI back thread (the main thread with characterMultithreading off);
// the tick runs on the main thread, hands the key to the detour and keeps the
// frame counter and the camera baseline. DEV builds also record counters
// and write the OnScreenStagger: heartbeat.

void InstallOnScreenStagger(int* installed, int*);
void OnScreenStaggerTick(double now, bool saveLoading);

#endif // KEO_FIXES_ONSCREEN_STAGGER_H
