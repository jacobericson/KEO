// core.h — Platform, logging, timing (Layer 0)
// No game knowledge. Included by every module.

#ifndef KENSHI_ZONE_OPT_CORE_H
#define KENSHI_ZONE_OPT_CORE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <core/Functions.h>
#include <Debug.h>

#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cmath>
#include <string>



// Captured on first hook_dispatchJob call (by nm_dispatch.cpp).
// Read by NavMeshCrashHandler in plugin/crash_record.cpp to compare against GetCurrentThreadId()
// at crash time — diagnoses whether processJob crashes on the bg thread or elsewhere.
extern volatile DWORD g_navMeshBgThreadId;

// Set to 1 by the NavMesh::stop hook when the game begins tearing the navmesh
// system down (hook_navMeshStop, nm_lazy_hooks.cpp, sets it as its first statement,
// before RetireNavMeshWorkers runs). Read by the crash handler (plugin/crash_record.cpp) to
// append "afterStop=1" to any crash record written after that point -- tagged,
// not suppressed, since a real mod crash can still happen there.
extern volatile LONG g_navMeshStopSeen;

// The navmesh worker's phase, for the crash record. Each worker
// points t_navMeshWorkerPhase at its own phase word (nm_worker_pool.cpp, which
// writes it through InterlockedExchange as the job moves on) and sets
// t_navMeshWorkerId; on every other thread the pointer stays NULL. The crash
// handler (plugin/crash_record.cpp) runs on the faulting thread, so it reads its own thread's
// pointer and appends " wphase=w<id>:<phase>" when it is set: one TLS load and
// one volatile read, no lock, no CRT.
enum NavMeshWorkerPhase
{
	WPHASE_IDLE = 0,      // between jobs, or waiting for one
	WPHASE_CLAIMED,       // a job is unlinked; cache lookup (the L2 read included)
	WPHASE_WAIT_CLONE,    // CloneNMG waiting for processJobCS
	WPHASE_CLONING,       // CloneNMG under processJobCS, then the clone-local setup
	WPHASE_WAIT_MISS,     // ProcessNavMeshJob waiting for processJobCS (missLock)
	WPHASE_GENERATING,    // under missLock: processJobAlt, partialGeneration
	WPHASE_BUILDING,      // buildCollision (a HIT, a late HIT or after a generation)
	WPHASE_STORING,       // the L1 store, the result hand-off and the L2 write
	WPHASE_COUNT
};
extern __declspec(thread) volatile LONG* t_navMeshWorkerPhase;
extern __declspec(thread) int            t_navMeshWorkerId;
// A static literal ("idle" ... "storing", "?" out of range). Any thread, no CRT.
const char* NavMeshWorkerPhaseName(LONG phase);


#include "base/clock.h"


// =========================================================================
// Logging
// =========================================================================

std::string GetDLLDirectory();
void InitLogFile();
void LogMsg(const std::string& line);

// True on the thread that called InitLogFile (startPlugin -> game main thread).
bool IsMainThread();

// Lines from threads where LogMsg is not wanted (the NavMesh bg thread's lazy
// hook install and first dispatch: LogMsg's synchronous write is not wanted there). On the
// main thread this is LogMsg. Anywhere else the line is copied into a fixed
// queue (no allocation; DEFERRED_LOG_CHARS per line including the tag
// "[deferred: tid=... t=...]", 32 lines, overflow counted and reported) and
// the main thread writes it from FlushDeferredLogLines.
const int DEFERRED_LOG_CHARS = 384;
void LogMsgDeferrable(const char* line);

// Main thread only; a no-op elsewhere. Writes every queued line, oldest first.
// Called every frame from the top of hook_updateCameraZone, beside the
// destroy-list replay and ahead of its early returns (save load, preload off,
// no camera), so a deferred line lands within a frame.
void FlushDeferredLogLines();

// The log path for a caller that must not wait on the log without a bound (the
// navmesh worker retire). Main thread. LogMsgBounded takes logCS only if it is
// free within boundMs (false, nothing written, when it is not), then, under it,
// writes the deferred lines first (pendingLogCS taken under logCS, a leaf) and
// the line. LogRetireFallback appends the line, timestamped as in the log, to
// KenshiZoneOpt.retire.txt beside the log: no lock and no CRT stream.
bool LogMsgBounded(const char* line, unsigned boundMs);
void LogRetireFallback(const char* line);

// Verbose logging: active in DEV builds (ZONEOPT_DEBUG defined) and compiled
// out entirely in PROD.
#ifdef ZONEOPT_DEBUG
void LogDebug(const std::string& line);
#else
inline void LogDebug(const std::string&) {}
#endif


// =========================================================================
// Our-guard counter
// =========================================================================
//
// The mod's own __try blocks fault on purpose: the work-buffer probes walk
// until they hit the end of an allocation, and the path-result guard exists
// because the extraction chain can fault on a stale face key. Those faults
// are handled where they happen and must never reach the crash recorder,
// which would otherwise write a crash_dump.txt for a non-event and, in PROD,
// spend the one recorded fault of the process on it.
//
// Per thread, so a probe on the NavMesh bg thread never masks a real fault on
// the main thread. Nesting is counted, not flagged.

extern __declspec(thread) int g_inOurGuard;

inline void GuardEnter() { ++g_inOurGuard; }
inline void GuardLeave() { --g_inOurGuard; }
inline bool InOurGuard() { return g_inOurGuard != 0; }

// KenshiZoneProfiler.dll's image, so the crash recorder can skip the access
// violations the profiler's own __try reads raise and handle. Size is stored
// before base (volatile stores are release stores under MSVC), so a nonzero
// base means both are set; zero means not known, and nothing is skipped.
// The recorder only compares: it never calls the loader.
extern volatile uintptr_t g_profilerImageBase;
extern volatile uintptr_t g_profilerImageSize;

inline bool InProfilerImage(uintptr_t addr)
{
	uintptr_t base = g_profilerImageBase;
	return base && addr >= base && addr - base < g_profilerImageSize;
}

// Main thread: looks the profiler up once; true once the range is known.
bool ProfilerImageResolve();
// Main thread, every frame: while the range is unknown, retries once per
// second for the first 60 s of ticks, then stops.
void ProfilerImageResolveTick(double now);


// =========================================================================
// Build gate
// =========================================================================
//
// The mod writes 5-byte jumps into 20-odd game functions and byte-patches one
// more. Every one of those addresses is a hardcoded RVA for Steam/GOG 1.0.65,
// so on any other build they name whatever happens to sit there. Comparing the
// first 16 bytes of each site against bytes read from the IDB turns that from a
// silent corruption into a refusal to install.
//
// The table itself lives in plugin/hook_manifest.cpp; core only does the compare.

struct HookPrologue
{
	const char*   name;
	uintptr_t     rva;
	unsigned char bytes[16];
	// True for a site that only carries a diagnostic. A mismatch on one of these
	// turns that diagnostic off; it must not take the whole plugin down with it,
	// because the diagnostic is not what the player installed the mod for. Set
	// from each row's kind where plugin/hook_manifest.cpp expands the rows.
	bool          diagnostic;
};

// Called once from startPlugin, before any VerifyPrologue call.
void SetCoreGameBase(uintptr_t base);
uintptr_t CoreGameBase();

// memcmp the 16 bytes at gameBase + rva against expect. Logs once and returns
// false on a mismatch or an unreadable address.
//
// A site another plugin already detoured (5-byte `E9 rel32` or 6-byte
// `FF 25 rel32`, optionally 0x90/0xCC padded to offset 8) passes when the bytes
// past the detour still match: the binary is the one we know, and KenshiLib's
// AddHook chains onto the existing detour. *sharedOut is set true for those, so
// the caller can report them. A tail mismatch is still a hard failure.
bool VerifyPrologue(uintptr_t rva, const unsigned char* expect, const char* name,
                    bool* sharedOut = NULL);

// Same compare at an absolute address. `where` names the site in log lines.
bool VerifyPrologueAt(const void* addr, const unsigned char* expect, const char* name,
                      const char* where, bool* sharedOut = NULL);


// =========================================================================
// Havok TLS heap allocator (used by navmesh_cache + pathfind_diag)
// =========================================================================

// Called by InitGameBindings to pass gameBase + RVA without core depending on game.h
void SetHavokTlsParams(uintptr_t base, size_t rva);
void* HavokTlsAlloc(size_t size);
void  HavokTlsFree(void* ptr, size_t size);


#endif // KENSHI_ZONE_OPT_CORE_H
