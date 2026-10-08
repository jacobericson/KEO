#pragma once
#include <windows.h>

// The OgreMain scene switches: sceneForkSkip and instEmptySkip. Each install
// runs at startup on the main thread; each tick runs there once per frame.
// DEV builds also record counters and write recurring heartbeats.

// NULL when ogre is the OgreMain build the scene switches' offsets were read
// from, else the reason it is not.
const char* OgreSceneBuildRefusal(HMODULE ogre);

void InstallSceneForkSkip(int* installed, int*);
void SceneForkSkipTick(double now);
void InstallInstEmptySkip(int* installed, int*);
void InstEmptySkipTick(double now);
