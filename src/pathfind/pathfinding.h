// pathfinding.h -- Pathfinding hook declarations + public function declarations (Layer 3)
// Depends on: config.h

#ifndef KEO_PATHFINDING_H
#define KEO_PATHFINDING_H

#include "base/config.h"

// Diagnostic hooks (bg thread) + requestPath (main thread)
char hook_csFindPath(void* manager, unsigned int startFaceKey, void* startPos,
                      void* destPos, float radius, char param5, void* resultBuf);
char hook_csCheckFaceConn(void* manager, unsigned int startFace, unsigned int destFace);
void hook_findPathFull(void* streamingCollection, void* searchState, void* findPathOutput);
void hook_requestPath(void* havokChar, float* destination, int priority);

void LogPathfindDiagStats(double now);
void ArmPathProbe();
void DumpPathProbe(double now);

// Priority boost hooks + stuck detection (main thread)
void hook_pathReqSubmit(void* sectionMgr, void* requestObj, bool highPriority);
void LogPhase12Stats(double now);
void PollPlayerMovementState(double now);
void StorePlayerClickDest(uintptr_t character, const float* dest, double now);
void SamplePlayerArrivals();
long  PlayerFarArrivals();
float PlayerFarArriveMaxDist();

// Fallback hook (bg thread)
char hook_csFindPathFallback(void* manager, unsigned int startFaceKey, void* startPos,
                              unsigned int destFaceKey, void* destPos, float radius,
                              float param6, char param7, void* resultBuf);

// The fallback hook running at all.
extern volatile long fallbackInvocations;

// Path-result extraction guard + streaming-collection timestamp
void hook_addInstance(void* collection, __int64 sectionData,
                      __int64 param3, __int64 param4, int param5);
unsigned __int64 hook_contentStreamCallee0x8869(void* manager,
                                                 unsigned int faceKey,
                                                 void* searchOutput,
                                                 unsigned int* resultBuf);

// "PathGuard:" counter line, main thread, rate-limited.
void LogPathGuardStats(double now);

#endif // KEO_PATHFINDING_H
