#pragma once
#include <stddef.h>
#include <string>

// The nearest view depth of anything the main camera's cull keeps: the
// candidate lower bound on receiver depth that skipping a shadow cascade
// would need. Dry only: it is logged against the cascade splits, never acted on.

typedef void* (*WorldAabbFn)(const void* obj, float* centerHalf6);

// Main thread: forgets the camera, the fold and the window.
void ReceiverDepth_Reset();
// Any thread: whether frustum is the main camera being folded.
bool ReceiverDepth_Watches(const void* frustum);
// Cull worker threads: folds data[begin, end), culled for camera, into the
// running minimum. No allocation, lock or logging.
void ReceiverDepth_Fold(const void* camera, void* const* data, size_t begin, size_t end, WorldAabbFn aabb);
// Main thread, before the first cascade of each shadow render: takes the
// minimum folded since the last call as this frame's candidate, checks it
// against splits (cascades + 1 depths) and the camera's focus distance
// (negative when unknown), and watches mainCamera from now on.
void ReceiverDepth_OnShadowStart(const float* splits, int cascades, const void* mainCamera, float focusDistance);
// Main thread: the Render: line's depth fields for the window just ended;
// resets the window.
std::string ReceiverDepthToken(double windowSec);
// Main thread: ends the window without formatting it.
void ReceiverDepth_ClearWindow();
