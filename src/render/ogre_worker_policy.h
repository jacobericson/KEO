#ifndef KEO_RENDER_OGRE_WORKER_POLICY_H
#define KEO_RENDER_OGRE_WORKER_POLICY_H

// Ogre's worker barrier as the join spin reads it. Pure: no game, Ogre or
// Windows header.

#include <stddef.h>
#include <stdint.h>

// The OgreMain build every offset below was read from.
const unsigned long OGRE_TIMESTAMP   = 0x5CA5F929;
const unsigned long OGRE_IMAGE_SIZE  = 0x9C9000;
// Barrier::sync (exported).
const size_t OGRE_BARRIER_SYNC_RVA   = 0x3DFF40;

// Barrier: the party count (64-bit) and the arrivals so far (32-bit).
const size_t OGRE_BARRIER_PARTIES    = 0x0;
const size_t OGRE_BARRIER_ARRIVED    = 0x10;
const unsigned long long OGRE_MAX_PARTIES = 128;

// The build this file describes.
bool OgreMainIdentityOk(unsigned long stamp, unsigned long imageSize);

// True while a party arriving now would wait: fewer than parties - 1 others
// have arrived. False for a barrier of fewer than two parties or more than
// OGRE_MAX_PARTIES, and for a count outside 0..parties - 1.
bool OgreSpinWanted(long arrived, unsigned long long parties);

// The spin budget in counter ticks: 0 when us or freq is not positive, else
// us * freq / 1000000, at least 1.
long long OgreSpinTicks(int us, long long freq);

#endif // KEO_RENDER_OGRE_WORKER_POLICY_H
