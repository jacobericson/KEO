#ifndef KEO_RENDER_OGRE_JOIN_SPIN_H
#define KEO_RENDER_OGRE_JOIN_SPIN_H

// The Ogre barrier join spin (ogreJoinSpinUs): an entry detour on OgreMain's
// exported Barrier::sync that, while on, lets the main thread spin up to the
// key's microseconds while the other parties have not all arrived, then calls
// the original unchanged. The forking thread, the main thread, is the only
// caller of the export; the tick runs on the main thread too and hands the key
// to the detour as a budget in counter ticks. DEV builds only: PROD compiles
// both functions as stubs.

void InstallOgreJoinSpin(int* installed, int*);
void OgreJoinSpinTick(double now);

#endif // KEO_RENDER_OGRE_JOIN_SPIN_H
