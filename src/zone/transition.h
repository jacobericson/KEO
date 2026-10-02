// transition.h — Zone transition bracket state (Layer 3)
// isTransitionActive/deferredFrameCount/transitionStartTime defined in transition_hook.cpp.
// navMeshThreadHandle/savedThreadPriority are file-static in navmesh_sched.cpp.
// Depends on: config.h (for Windows.h types)

#ifndef KEO_TRANSITION_H
#define KEO_TRANSITION_H

#include "base/config.h"


// =========================================================================
// Transition state (defined in transition_hook.cpp, read by the preload split units)
// =========================================================================

extern bool           isTransitionActive;
extern int            deferredFrameCount;
extern LARGE_INTEGER  transitionStartTime;

// Deferred transition completion. The dismissal call to showLoadingMessage
// can arrive on the contentStream (path) thread, concurrently with the main
// thread, so that hook does no CRT work at all: it stamps the end time and the
// calling thread id, then raises transitionEndPending. hook_updateCameraZone
// claims the flag with InterlockedCompareExchange and runs the completion body
// (thread restore, both log lines, preload reset) on the main thread.
// transitionEndQpc is written before transitionEndPending is raised and read
// after it is claimed; the interlocked pair on the flag orders the stamp.
extern volatile LONG  transitionEndPending;
extern LARGE_INTEGER  transitionEndQpc;

// Runs the deferred completion if one is pending. Main thread only.
// Bracket instrumentation is owned by transition_hook.cpp: the target 3x3
// line at bracket open ("Transition target:") and, for transitions over
// 300 ms, " frames=s<state>:<n>,..." appended to the "Transition:" line.
void TransitionCompleteIfPending();

// The bracket generation, incremented when a bracket opens. Any thread.
LONG TransitionGeneration();


#endif // KEO_TRANSITION_H
