// movement_trace.h - The movement trace (the session build, movementTrace=on): per-member position
// samples by distance moved, the copied path results attributed to the members, and each closed
// order's lines appended to one file beside the log, for the offline harness. Main thread. The
// release build compiles it out: each entry is then an empty inline.
#ifndef KEO_MOVEMENT_TRACE_H
#define KEO_MOVEMENT_TRACE_H

#include <stdint.h>
#include "movement/movement_trace_policy.h"

#ifdef KEO_DEBUG

// SamplePlayerArrivals, each live tracked member, every frame: a sample when it moved
// TRACE_SAMPLE_UNITS since its last. Returns at once unless armed.
void MovementTraceSample(uintptr_t character, uintptr_t cm, uintptr_t hc, int characterState);
// OrderOutcomePoll, every frame: the copied results attributed, then each order closed since the last
// frame written, then the Trace: line on its period. Returns at once unless armed.
void MovementTraceFrame(double now);
// OrderOutcomeReset (a save load): every order's buffers, the queued writes and the unmatched results
// freed. Returns at once unless armed.
void MovementTraceReset();
// The order-outcome table's close note, which can run inside the order hook: the order's write queued
// for the next MovementTraceFrame. Returns at once unless armed.
void MovementTraceOnOrderClose(int orderNum);

#else // KEO_DEBUG: the release build compiles the trace out

inline void MovementTraceSample(uintptr_t, uintptr_t, uintptr_t, int) {}
inline void MovementTraceFrame(double) {}
inline void MovementTraceReset() {}
inline void MovementTraceOnOrderClose(int) {}

#endif // KEO_DEBUG

#endif // KEO_MOVEMENT_TRACE_H
