#ifndef KENSHI_ZONE_OPT_NM_BUILDLOCK_H
#define KENSHI_ZONE_OPT_NM_BUILDLOCK_H

#include "base/config.h"
#include <string>

// The wide cover: one mod lock (buildCollisionCS) around a whole builder or
// partialGeneration. Lock order processJobCS -> buildCollisionCS; it is a leaf.
struct BuildCollisionScope
{
	LARGE_INTEGER holdStart;
	BuildCollisionScope();
	~BuildCollisionScope();
private:
	BuildCollisionScope(const BuildCollisionScope&);
	BuildCollisionScope& operator=(const BuildCollisionScope&);
};

// NavMesh bg thread, first dispatch, before any worker exists: installs the
// builder hooks and decides the lock mode once.
void InstallBuildLockHooks();

bool BuildLockNarrowActive();
// Main thread: " bcMode=… bcOverlap=… bcWait=… bcHold=… bcTotal=… bsc=… blTry=… blStall=… [blRecur=…] [blFail=…] [stitchAsym=…] stitch=…".
std::string BuildLockStatsSuffix();

// The thread inside a narrow builder scope that holds the game's build mutex,
// and which of the two regions it took it for; 0 when nobody holds it. Read
// from the crash record: a hold ended by a fault never reaches the release
// that would time it, so no timing counter can carry this fact out.
extern volatile LONG g_buildLockOwnerTid;
extern volatile LONG g_buildLockOwnerState;

#endif
