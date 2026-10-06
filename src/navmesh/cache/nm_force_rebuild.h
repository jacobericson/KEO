// nm_force_rebuild.h - The game's rebuild-navmesh key (NavMesh::generate(ZoneMap*)): force marks a
// type-0 claim consumes, the forced job's cache replace, the queue's front, the loading-panel hold
// and the key's detour. The marks: the main thread writes them, a claim and a job's end
// compare-exchange them; nothing here takes a lock but nmCacheCS in NmForceRebuildClearKey.
#ifndef KEO_NM_FORCE_REBUILD_H
#define KEO_NM_FORCE_REBUILD_H

#include <stddef.h>
#include "navmesh/cache/nm_cache_key.h"

class NmQueueLock;

// How a forced job ended: DROPPED unless its store ran.
enum NmForceOutcome { NM_FORCE_DROPPED = 0, NM_FORCE_STORED, NM_FORCE_REFUSED };

// A claim, holding the queue lock as q: a type-0 job of a cell with a live
// MARKED mark claims it. Returns the cell (and *wordOut, the CLAIMED word),
// or -1. Interlocked only.
int  NmForceRebuildClaimLocked(const NmQueueLock& q, uintptr_t zone, int jobType, long* wordOut);
// A forced job's end, any NavMesh thread, no lock: counts the outcome and
// ends the mark it claimed (a mark re-pressed or reset meanwhile is left).
void NmForceRebuildFinish(int cell, long claimedWord, int outcome);
// A forced job's lookup point, no lock held on entry: with keyOk, every L1
// entry of key is evicted under nmCacheCS, then key's L2 file is deleted with
// no lock. Returns true when the delete was refused (retry at the job's end).
bool NmForceRebuildClearKey(bool keyOk, const NavMeshCacheKey& key);
void NmForceRebuildRetryDelete(const NavMeshCacheKey& key);
// Counts L1 entries a forced store evicted.
void NmForceRebuildNoteEvicted(int n);
// " forced=... forcedEvict=..." once any mark was claimed, expired or lost; else empty.
void NmForceRebuildFormatCacheToken(char* out, size_t outSize);

// The prioritizer's front bucket (main thread, under the queue lock): a type-0
// job of a cell whose mark is MARKED within its TTL.
bool NmForceRebuildWantsFront(uintptr_t zone, int jobType, __int64 nowQpc);
// hook_showLoadingMessage, any thread, on a dismissal: true while a key press
// holds the panel (off the main thread, under the cap). Interlocked and QPC only.
bool NmForceRebuildHoldsDismissal();
// Main thread: inside the key's call of the game's NavMesh::generate.
bool NmForceRebuildInKeyCall();
// hook_showLoadingMessage on a show: records that the call showed the panel,
// and, for a key press holding it, that the panel is up for the press.
void NmForceRebuildNoteShow();
// hook_showLoadingMessage on a dismissal that reaches the game, any thread:
// the press's panel is down. Interlocked only.
void NmForceRebuildNoteDismissed();
// Main thread, every camera frame: expires and drops lost marks, ends the hold
// (one line), and returns true when the held dismissal is to be issued now.
bool NmForceRebuildTick(void* zoneMgr, bool saveLoading);
// Save-load reset (main thread): every mark, the press and the hold cleared.
void NmForceRebuildOnWorldReset();
// startPlugin's install step for the row HOOK_NAVMESH_GENERATE_ZONEMAP.
void InstallNavMeshRebuildKey(int* installed, int* total);

#endif
