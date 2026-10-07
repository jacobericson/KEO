#ifndef KEO_RENDER_OGRE_WORKER_PRIORITY_H
#define KEO_RENDER_OGRE_WORKER_PRIORITY_H

// The Ogre worker priority (ogreWorkerPriority): on a switch to on, the main
// scene manager's own worker threads, each confirmed by its start routine, are
// raised to above-normal priority after their priority is saved; a switch to
// off puts the saved values back. No hook: everything runs in the main
// thread's frame tick. DEV builds only: PROD compiles both functions as stubs.

void InstallOgreWorkerPriority(int* installed, int*);
void OgreWorkerPriorityTick(double now);

#endif // KEO_RENDER_OGRE_WORKER_PRIORITY_H
