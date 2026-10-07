#ifndef KEO_NPC_FAIL_MEMO_H
#define KEO_NPC_FAIL_MEMO_H

// The NPC failed-search memo (the npcFailMemo key, DEV builds): an NPC's path search toward a goal
// that the same start cluster already failed to reach, under the same navmesh, door state and
// session and within the time-to-live, is answered "failed" without the A*; observe runs the A*
// anyway and checks what the memo would have answered. Also the capped searches' face reads for
// AstarSlow and the requester ring, and the door-state pass-through.

#include "pathfind/npc_fail_memo_policy.h"

// One covered search, from the check before the A* to the bookkeeping after it.
struct NpcFailMemoCall
{
	int      mode;        // NFM_OFF when the search is not covered
	NfmKey   key;
	unsigned startFace;
	NfmEpoch epoch;
	int      wouldHit;    // observe: a live entry matched
	int      exactFace;   // ... and its start face is this search's own
	int      recordedStatus;
	int      unreachableCovered;
};

#ifdef KEO_DEBUG
// Path thread, inside the A* detour once the request's labels are taken. True when the memo
// answered: the recorded failure status and cause are in the output and the A* must not run.
bool NpcFailMemoBefore(void* collection, void* input, void* output, AstarCallerClass cls,
                       bool playerTag, bool waved, NpcFailMemoCall* call);
// Path thread, after the A*: observe's check and the insert; for a capped character search, the
// goal face's data word and the start's cluster key (-1 when unread) and the requester ring.
void NpcFailMemoAfter(const NpcFailMemoCall* call, void* collection, void* input, AstarCallerClass cls,
                      int status, int cause, int iterations, long long ticks, const void* request,
                      int* goalData, int* startCluster);
#endif

// Main thread, every frame: publishes the key's mode, starts a new epoch on a mode change and on a
// save load, and writes the NpcFailMemo: and NpcCapWho: lines every 10 s. A stub in PROD.
void NpcFailMemoTick(double now, bool saveLoading);

// Startup step: the door-state pass-through. A stub in PROD.
void InstallNpcFailMemo(int* installed, int*);

#endif // KEO_NPC_FAIL_MEMO_H
