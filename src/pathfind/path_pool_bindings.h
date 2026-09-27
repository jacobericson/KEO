// path_pool_bindings.h - Path-thread instrumentation addresses and bindings.
// Included through game.h.

#ifndef KENSHI_ZONE_OPT_PATH_POOL_BINDINGS_H
#define KENSHI_ZONE_OPT_PATH_POOL_BINDINGS_H

#include "base/core.h"

// ---- Path-worker-pool pass-through instrumentation ----
// Four pass-through hooks on the path thread's own functions (contentStream,
// dequeueWork, enqueue_threadSafe, Gates__updateCodes), plus one "called,
// not hooked" utility (isPriorityPath) for the NPC wait diagnostic.

// SectionManager::contentStream (0x3AE350): one call per path-thread pass.
// char __fastcall(SectionManager*): return 1 when the pass served a request.
// Sole caller NavMesh__threadProc (0x3AEFB0), IDA-verified.
const size_t RVA_CONTENT_STREAM = 0x3AE350;
typedef char (*contentStream_t)(void* sectionMgr);
extern contentStream_t orig_contentStream;

// SectionManager::dequeueWork_threadSafe (0x3BEE40): pops one request from
// the queue passed in. Sole caller (IDA xref): contentStream, via
// j_SectionManager__dequeueWork, always with queueBase = mgr+0xB8 (the input
// queue). We stamp the returned request's req+0x00 with the drain QPC --
// never written by the game (ctor 0x14D240, cleanup 0x3BD3A0, submit
// 0x3AAEF0, processPathResult 0x3A27A0 and applyPathResult 0x144C90 all
// checked, none touch it.
const size_t RVA_DEQUEUE_WORK = 0x3BEE40;
typedef void* (*dequeueWork_t)(void* queueBase);
extern dequeueWork_t orig_dequeueWork;

// PathRequestQueue::enqueue_threadSafe (0x3B6110). 4 call sites (IDA xref):
// PathRequestQueue::submit (+0x6B, mgr+0xB8, main thread),
// PathRequestQueue::submitCancel (+0x50, no callers in this build) and
// SectionManager::contentStream twice (+0x5E2 the section-add sentinel,
// +0xC1A the serve-block completion -- both mgr+0xF8, the result queue, path
// thread). Filtered at runtime to the result queue; the input-queue call
// passes through untouched. Same RVA as RVA_ENQUEUE_PATH_REQ / fn_enqueuePathReq.
const size_t RVA_ENQUEUE_THREAD_SAFE = 0x3B6110;
typedef void (*enqueueThreadSafe_t)(void* queueBase, void** itemPtr);
extern enqueueThreadSafe_t orig_enqueueThreadSafe;

// Gates__updateCodes (0x2EF460): the gate-code pass that runs on the path
// thread once section adds drain, before the loading-screen dismissal.
// Sole caller SectionManager::contentStream at +0x7ED.
// Timed from function entry so a zero-gate pass still counts (it still takes
// the handshake).
const size_t RVA_GATES_UPDATE_CODES = 0x2EF460;
typedef __int64 (*gatesUpdateCodes_t)(void* gatesObj);
extern gatesUpdateCodes_t orig_gatesUpdateCodes;

// Gates__findPath (0x2EC540): one gate search, called from three loops of
// Gates__updateCodes through the jmp thunk 0x3C78B.
const size_t RVA_GATES_FIND_PATH = 0x2EC540;

// CharMovement::isPriorityPath (0x790B30, 27 bytes): player-owned check --
// vtable+88(character)+592 != 0 (Faction* -> PlayerInterface*). Called directly, not hooked, from the NPC wait
// diagnostic to report player-owned characters separately.
const size_t RVA_IS_PRIORITY_PATH = 0x790B30;
typedef bool (*isPriorityPath_t)(void* character);
extern isPriorityPath_t fn_isPriorityPath;

// ---- end path-worker-pool instrumentation ----

#endif // KENSHI_ZONE_OPT_PATH_POOL_BINDINGS_H
