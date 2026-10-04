// path_result_trace.h - The movement trace's path-thread half: a successful player request's path
// result copied, after the path hook's original, into a ring the main thread drains. The session
// build alone; the release build compiles it out.
#ifndef KEO_PATH_RESULT_TRACE_H
#define KEO_PATH_RESULT_TRACE_H

#include "movement/movement_trace_policy.h"

#ifdef KEO_DEBUG

// Main thread, at the trace's arm: the copy runs from now on.
void PathResultTraceArm();
// The contentStream path thread, after hook_csFindPath's or hook_csFindPathFallback's original:
// resultBuf (the request's result array) copied with navMesh's world shift when ok and player.
// Returns at once unless armed. No lock, allocation or log.
void PathResultTraceCopy(void* navMesh, const void* resultBuf, int ok, int player);
// Main thread: the next copied result after *taken (TraceResultTake's contract).
bool PathResultTraceTake(long* taken, TraceResult* out, long* overruns, long* torn);

#else // KEO_DEBUG: the release build compiles the copy out

inline void PathResultTraceArm() {}
inline void PathResultTraceCopy(void*, const void*, int, int) {}
inline bool PathResultTraceTake(long*, TraceResult*, long*, long*) { return false; }

#endif // KEO_DEBUG

#endif // KEO_PATH_RESULT_TRACE_H
