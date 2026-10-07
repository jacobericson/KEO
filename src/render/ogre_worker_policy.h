#ifndef KEO_RENDER_OGRE_WORKER_POLICY_H
#define KEO_RENDER_OGRE_WORKER_POLICY_H

// Ogre's worker barrier and worker threads as the join spin and the worker
// priority read them. Pure: no game, Ogre or Windows header.

#include <stddef.h>
#include <stdint.h>

// The OgreMain build every offset below was read from.
const unsigned long OGRE_TIMESTAMP   = 0x5CA5F929;
const unsigned long OGRE_IMAGE_SIZE  = 0x9C9000;
// Barrier::sync (exported) and the worker threads' start routine.
const size_t OGRE_BARRIER_SYNC_RVA   = 0x3DFF40;
const size_t OGRE_WORKER_START_RVA   = 0x2CD1E0;

// Barrier: the party count (64-bit) and the arrivals so far (32-bit).
const size_t OGRE_BARRIER_PARTIES    = 0x0;
const size_t OGRE_BARRIER_ARRIVED    = 0x10;
// SceneManager: the worker count, and the worker vector's begin and end. An
// element is a shared pointer whose first word is the ThreadHandle, whose
// first word is the thread's OS handle.
const size_t OGRE_SM_WORKER_COUNT    = 0x4A28;
const size_t OGRE_SM_WORKERS_BEGIN   = 0x4B28;
const size_t OGRE_SM_WORKERS_END     = 0x4B30;
const size_t OGRE_WORKER_SLOT_SIZE   = 16;

const unsigned long long OGRE_MAX_PARTIES = 128;
const int OGRE_MAX_WORKERS           = 64;
const int OGRE_PRIORITY_RAISED       = 1;            // THREAD_PRIORITY_ABOVE_NORMAL
const int OGRE_PRIORITY_ERROR        = 0x7FFFFFFF;   // THREAD_PRIORITY_ERROR_RETURN

// The build this file describes.
bool OgreMainIdentityOk(unsigned long stamp, unsigned long imageSize);

// True while a party arriving now would wait: fewer than parties - 1 others
// have arrived. False for a barrier of fewer than two parties or more than
// OGRE_MAX_PARTIES, and for a count outside 0..parties - 1.
bool OgreSpinWanted(long arrived, unsigned long long parties);

// The spin budget in counter ticks: 0 when us or freq is not positive, else
// us * freq / 1000000, at least 1.
long long OgreSpinTicks(int us, long long freq);

// The number of worker slots between begin and end; -1 when end is below
// begin or the span is not whole slots.
int OgreWorkerSlots(uintptr_t begin, uintptr_t end);

// The priority may act: 1..OGRE_MAX_WORKERS slots, exactly numWorkers.
bool OgreWorkersUsable(int slots, unsigned long long numWorkers);

// A saved priority can be put back: it was read.
bool OgrePriorityRestorable(int saved);

// Seconds between raise attempts while the switch is on and no main scene
// manager has been found yet.
const double OGRE_PRIORITY_RETRY_S   = 1.0;

// The raise is tried again: the switch is on, the last try found no main
// scene manager, and at least OGRE_PRIORITY_RETRY_S has passed since it.
bool OgrePriorityRetryDue(bool on, bool sceneMissing, double now, double lastTry);

#endif // KEO_RENDER_OGRE_WORKER_POLICY_H
