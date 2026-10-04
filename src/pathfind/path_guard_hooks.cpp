// path_guard_hooks.cpp - The two conditional diagnostic path hooks and their PathGuard: line: the
// streaming collection's instance add (counted and handed to the lifecycle rows) and the result
// extraction's fault guard. Each on the thread its hook runs on; the line on the main thread.
#include "pathfind/pathfinding.h"
#include "pathfind/pathfind_diag.h"
#include "movement/tracking.h"
#include "fixes/streaming/navmesh_life.h"
#include "fixes/stitch/stitch_source.h"
#include <sstream>

// Extraction guard + streaming-collection timestamp counters
static volatile long extractionCrashRescue = 0;  // faults caught inside the extraction loop
static volatile long addInstanceHookCalls  = 0;  // sanity: the addInstance hook is firing

// =========================================================================
// Extraction guard + addInstance timestamp
// =========================================================================

// hkaiStreamingCollection::addInstance (navmesh bg thread). Counts the
// insertions the section table takes and hands the registered instance to the
// lifecycle rows, which need the slot index the original writes. No blocking,
// no locks.
void hook_addInstance(void* collection, __int64 sectionData,
                      __int64 param3, __int64 param4, int param5)
{
	game::g_hookOrig.orig_addInstance(collection, sectionData, param3, param4, param5);
	InterlockedIncrement(&addInstanceHookCalls);
	NavMeshLifeOnAdd(collection, sectionData);
	StitchSourceOnAdd(collection, sectionData, param4);
}

// Havok::contentStreamCallee_0x8869 (contentStream bg thread): the loop that
// walks the search output and fills the result buffer. It dereferences the
// section an edge names without checking it, so an instance removed while the
// loop runs faults. On a fault the edge count is restored to what it was
// before the call, which logically erases the partial writes past it: the game
// reads "no new edges", treats it as no path and the character re-paths on the
// next tick instead of following a half-written result.
//
// Nothing in this function may need unwinding (a __try function takes no C++
// object with a destructor): POD locals, Win32 atomics and a literal log only.
static volatile long s_extractionLogged = 0;

unsigned __int64 hook_contentStreamCallee0x8869(void* manager,
                                                 unsigned int faceKey,
                                                 void* searchOutput,
                                                 unsigned int* resultBuf)
{
	unsigned int origCount = resultBuf ? (*(unsigned int*)KLIB_MEMBER(5, resultBuf, ResultPathArray_m_size, 8)) : 0;
	unsigned __int64 result = origCount;
	// A fault in the extraction chain is what this guard exists for, and
	// the rescue counter records it. Keep it out of the crash recorder.
	GuardEnter();
	__try {
		result = game::g_hookOrig.orig_contentStreamCallee0x8869(manager, faceKey, searchOutput, resultBuf);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement(&extractionCrashRescue);
		if (resultBuf) (*(unsigned int*)KLIB_MEMBER(5, resultBuf, ResultPathArray_m_size, 8)) = origCount;
		result = origCount;
		// A rescue is never silent. Bg thread, and no C++ object may live in a
		// function with __try: a literal through the deferrable logger only,
		// once per session (the counter above carries the rest).
		if (InterlockedExchange(&s_extractionLogged, 1) == 0)
			LogMsgDeferrable("PathGuard: fault inside the path-result extraction loop, "
			                 "result rolled back to no new edges");
	}
	GuardLeave();
	return result;
}


// "PathGuard:" line, main thread. Prints while anything happened and at most
// once per interval, independently of the priority-tier reporter.
void LogPathGuardStats(double now)
{
	static double lastLog = 0.0;
	static long   lastTotal = -1;

	long ecr    = InterlockedCompareExchange(&extractionCrashRescue, 0, 0);
	long aiHook = InterlockedCompareExchange(&addInstanceHookCalls, 0, 0);
	long evicts = InterlockedCompareExchange(&watchedEvictions, 0, 0);
	long total  = ecr + aiHook + evicts;

	if (total == 0 || total == lastTotal)
		return;
	if (now - lastLog < 30.0)
		return;
	lastLog   = now;
	lastTotal = total;

	std::ostringstream ss;
	ss << "PathGuard: extractAV=" << ecr
	   << " addInst=" << aiHook
	   << " evict=" << evicts;
	LogMsg(ss.str());
}
