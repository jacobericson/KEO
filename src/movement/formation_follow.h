// formation_follow.h - The follow probe (the session build, its key at probe): a run-together group's
// members near the leader get the engine's follow task on it, the poll captures the rest as they
// close, and the Follow: line reads what the engine does with them. It steers nothing. Main thread,
// except FormationFollowNoteRequest and FormationFollowArmed (any thread). The release build compiles
// it out: each entry is then an empty inline.
#ifndef KEO_FORMATION_FOLLOW_H
#define KEO_FORMATION_FOLLOW_H

#include <stdint.h>
#include "movement/formation_follow_policy.h"

#ifdef KEO_DEBUG

// From the order hook after the original: the active group whose first member is chars[0] gives each
// member within its gather radius of that leader the Follow order on it; a repeat of the group's order
// gives its followers the order again. Returns at once unless armed.
void FormationFollowAfterOrder(const uintptr_t* chars, int n);
// From the end of PollFormationGroups, with the player list it read: releases, captures, promotion, the
// reads and the Follow: line. Returns at once unless armed.
void FormationFollowPoll(double now, unsigned int scCount, const uintptr_t* scStuff);
// Ends the following of chars[0..n) for a FollowRelease reason; the engine has replaced their task, so
// nothing is sent. An order or the stop key taking a record's leader marks the leader lost.
void FormationFollowRelease(const uintptr_t* chars, int n, int reason);
// From ClearFormationGroups (a save load): forgets every record; the next poll counts the player
// characters still on a follow task.
void FormationFollowOnClear();
// Whether the movement's character follows under the probe; a true answer is counted as an owned skip
// at seam (a FollowSeam). False at once unless armed.
bool FormationOwnsFollower(uintptr_t cm, int seam);
// The same answer, counted nowhere: for a test run every frame. False at once unless armed.
bool FormationFollowerOwned(uintptr_t cm);
// Any thread: whether the probe is armed.
bool FormationFollowArmed();
// Any thread, from hook_requestPath before its original: counts a follower's path request. No lock,
// allocation or log; returns at once unless armed.
void FormationFollowNoteRequest(void* havokChar);

#else // KEO_DEBUG: the release build compiles the probe out

inline void FormationFollowAfterOrder(const uintptr_t*, int) {}
inline void FormationFollowPoll(double, unsigned int, const uintptr_t*) {}
inline void FormationFollowRelease(const uintptr_t*, int, int) {}
inline void FormationFollowOnClear() {}
inline bool FormationOwnsFollower(uintptr_t, int) { return false; }
inline bool FormationFollowerOwned(uintptr_t) { return false; }
inline bool FormationFollowArmed() { return false; }
inline void FormationFollowNoteRequest(void*) {}

#endif // KEO_DEBUG

#endif // KEO_FORMATION_FOLLOW_H
