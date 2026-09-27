// core.cpp - Timing and NavMesh-stop state, the worker phase names, the guard counter,
// the profiler image resolve and the log-file object definitions.
#include "base/core.h"
#include "base/core_internal.h"


// =========================================================================
// Timing state
// =========================================================================

LARGE_INTEGER qpcFrequency;
LARGE_INTEGER pluginStartTime;

// Set once on first hook_dispatchJob call via InterlockedCompareExchange.
volatile DWORD g_navMeshBgThreadId = 0;

// Set to 1 by the NavMesh::stop hook when the game begins tearing the navmesh
// system down (hook_navMeshStop, nm_lazy_hooks.cpp, sets it as its first statement,
// before RetireNavMeshWorkers runs). Read by the crash handler (plugin/crash_record.cpp) to
// append "afterStop=1" to any crash record written after that point -- tagged,
// not suppressed, since a real mod crash can still happen there.
volatile LONG g_navMeshStopSeen = 0;

// See core.h -- set once by the NavMesh::stop hook's own install (nm_lazy_hooks.cpp).
volatile LONG g_navMeshStopHookInstalled = 0;

// The worker phase for the crash record (core.h).
__declspec(thread) volatile LONG* t_navMeshWorkerPhase = NULL;
__declspec(thread) int            t_navMeshWorkerId    = -1;

const char* NavMeshWorkerPhaseName(LONG phase)
{
	static const char* const kNames[WPHASE_COUNT] = {
		"idle", "claimed", "waitClone", "cloning", "waitMiss", "generating", "building", "storing"
	};
	if (phase < 0 || phase >= WPHASE_COUNT)
		return "?";
	return kNames[phase];
}


// =========================================================================
// Our-guard counter — see core.h
// =========================================================================

__declspec(thread) int g_inOurGuard = 0;

volatile uintptr_t g_profilerImageBase = 0;
volatile uintptr_t g_profilerImageSize = 0;

bool ProfilerImageResolve()
{
	if (g_profilerImageBase)
		return true;
	uintptr_t base = (uintptr_t)GetModuleHandleA("KenshiZoneProfiler.dll");
	if (!base)
		return false;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	g_profilerImageSize = nt->OptionalHeader.SizeOfImage;
	g_profilerImageBase = base;
	return true;
}

void ProfilerImageResolveTick(double now)
{
	static double first = -1.0;
	static double lastTry = -1.0;
	if (g_profilerImageBase)
		return;
	if (first < 0.0)
		first = now;
	if (now - first > 60.0 || (lastTry >= 0.0 && now - lastTry < 1.0))
		return;
	lastTry = now;
	ProfilerImageResolve();
}


namespace core_detail
{ // Shared log-file objects retain their original initialization unit.
std::string logFilePath;
std::ofstream logFile;
} // namespace core_detail
using namespace core_detail;
