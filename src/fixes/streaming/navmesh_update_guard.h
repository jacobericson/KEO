#ifndef KENSHI_ZONE_OPT_FIXES_NAVMESH_UPDATE_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_NAVMESH_UPDATE_GUARD_H

#include <string>

// Crash classifier for NavMesh::update (the navmesh/path thread's whole pass).
//
// update takes the SectionManager's changeMutex (+0x200) exclusively near its
// start and releases it at its end. A fault anywhere between those two points
// kills the thread with the mutex still set, and a boost::shared_mutex is not
// released when its owner thread dies. Every navigability query try-locks that
// same mutex shared and returns "no point here" the instant the try fails, so
// an orphaned changeMutex makes it impossible to order any character anywhere
// for the rest of the session, while the rest of the game keeps running.
//
// A fault there is a reader tripping over damage in the streaming collection,
// and the exclusive lock is the only thing keeping every other reader out of
// the same damage. So the lock is left held on purpose: this wraps the call in
// SEH, proves from the frame chain whether the faulting thread owned the lock,
// writes navmesh_guard.txt and then lets the fault stand untouched. Nothing is
// released, the thread is not resumed, and the fault reaches the same handler
// it would reach with no guard installed: RE_Kenshi's, which offers an
// emergency save and then ends the process whichever way that is answered.
//
// The guard installs no hook. It is a wrapper around a call the mod already
// owns, so no build-gate site and no banner count changes.

typedef char (*NavMeshUpdateFn)(void*);

// Main thread, once, after the config is loaded. gameBase is the exe's load
// address; the record file is written next to the mod's other logs. A false
// `enabled` makes NavMeshUpdateGuardCall a plain pass-through forever.
void NavMeshUpdateGuardInit(const std::string& dllDir, unsigned __int64 gameBase, bool enabled);

// Calls orig(sectionMgr) under the guard. Safe from any thread; only the
// navmesh/path thread actually reaches it.
char NavMeshUpdateGuardCall(void* sectionMgr, NavMeshUpdateFn orig);

// True once a fault has been latched, i.e. the change lock is being left held
// and the session can no longer answer a navigability query. Interlocked read;
// reported on the PathQueue: log line.
bool NavMeshUpdateGuardLatched();

#endif // KENSHI_ZONE_OPT_FIXES_NAVMESH_UPDATE_GUARD_H
