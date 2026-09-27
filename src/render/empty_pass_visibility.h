#pragma once
#include <windows.h>

// What a compositor node's render_scene passes would draw this frame, read
// from the scene manager's cull list and the passes' own cameras.
enum PassContent
{
	PASS_EMPTY,      // no live object in the node's render queues
	PASS_OCCUPIED,   // objects, not tested (the visibility test is unavailable)
	PASS_HIDDEN,     // objects, none of which the pass's cull can keep
	PASS_MAY_DRAW,   // an object might be drawn
	PASS_UNSURE      // could not tell: unexpected layout, camera state, resize or scan budget
};

// Resolves the OgreMain exports and checks the layout the test reads. On
// false, NodePassContent answers from occupancy alone: PASS_EMPTY or
// PASS_OCCUPIED, never PASS_HIDDEN.
bool InstallPassVisibility(HMODULE ogre);
bool PassVisibilityAvailable();

// Main thread, before the workspace's nodes run; no allocation, lock or
// logging. `resizePending`: this frame's update resizes the target, so the
// viewports the passes will use are not known yet. Adds the objects tested
// to *scanned.
PassContent NodePassContent(void* node, void* scene, size_t firstRq, size_t endRq, bool resizePending,
                            size_t* scanned);
