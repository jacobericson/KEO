#pragma once
#include <windows.h>

// Every render_scene pass fires the game's QueueListenerCutter::
// preRenderQueues, whose tail advances each fog-volume fade by the frame's dt,
// so fade speed scales with the pass count. A lever that skips passes makes
// that one update call itself for each pass skipped, with the same arguments.

// Verifies the update call and its call site once and remembers the result;
// later calls only return it.
bool InstallFogFade();
// Main thread: whether the update can run now (verified, and the fog
// controller and the renderer's camera both exist).
bool FogFadeAvailable();
// Main thread: runs the update `passes` times with preRenderQueues'
// arguments. False, doing nothing, when FogFadeAvailable() is false.
bool FogFadeCompensate(LONG passes);
