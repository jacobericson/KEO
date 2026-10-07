#pragma once
#include <windows.h>

// The OgreMain scene switches (DEV builds; PROD links stubs): sceneForkSkip,
// instEmptySkip and rqStructClear. Each install is a startup install step on
// the main thread; each tick runs on the main thread once per frame.

#ifdef KEO_DEBUG
// NULL when ogre is the OgreMain build the scene switches' offsets were read
// from, else the reason it is not. DEV builds only.
const char* OgreSceneBuildRefusal(HMODULE ogre);
#endif

void InstallSceneForkSkip(int* installed, int*);
void SceneForkSkipTick(double now);
void InstallInstEmptySkip(int* installed, int*);
void InstEmptySkipTick(double now);
void InstallRqStructClear(int* installed, int*);
void RqStructClearTick(double now);
