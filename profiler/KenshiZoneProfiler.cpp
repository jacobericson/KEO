#include "game/klib_members.h"
// KenshiZoneProfiler - Standalone zone loading performance profiler
// Loaded as RE_Kenshi sub-plugin via RE_Kenshi.json manifest.
// Hooks 10 functions in the zone streaming pipeline, logs timing per zone transition.
// The frame audit (KenshiFrameAudit.cpp, CallSiteProbe.cpp) adds per-frame
// section timings; all file output goes through its reporter thread.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <core/Functions.h>
#include <Debug.h>

#include <sstream>
#include <iomanip>
#include <cstring>

#include "KenshiFrameAudit.h"
#include "game/klib_bindings.h"


// --- RVA constants (Kenshi Steam 1.0.65) ---

// RVAs from KenshiAddressLogger (runtime-resolved, authoritative)
static const size_t RVA_SHOW_LOADING_MESSAGE = 0x6E9800;  // ForgottenGUI::showLoadingMessage(bool)
static const size_t RVA_UPDATE_MT            = 0x44BBA0;  // ResourceLoader::updateMT
static const size_t RVA_PLATOON_ACTIVATE     = 0x7EB250;  // Platoon::activate
static const size_t RVA_MAIN_LOOP            = 0x787E70;  // GameWorld::mainLoop_GPUSensitiveStuff

// Legacy Steam RVAs, checked against KenshiLib exports
static const size_t RVA_PARSE_FILE           = 0x6C0800;  // RootObjectContainer::parseFile (7741 bytes)
static const size_t RVA_LOAD_ZONE_DATA       = 0xA0E1B0;  // ZoneManager::loadZoneData (584 bytes)
static const size_t RVA_STATE_CONTINUATION   = 0xA0E950;  // Per-frame zone streaming state machine (1521 bytes)
static const size_t RVA_ZONE_CONTENT_INIT    = 0xA12880;  // Zone content initialization (3350 bytes)
static const size_t RVA_CONTENT_STREAMING    = 0x3AE350;  // Major content streaming (3159 bytes, 54 callees)
static const size_t RVA_BUILDING_LOAD        = 0x3AB700;  // Building/town content load (404 bytes)

// Global data RVAs (IDA address - 0x140000000)
static const size_t RVA_GLOBAL_SECTION_MGR   = 0x2133560; // qword_142133560 — section/content manager ptr


// --- Address helper ---

static uintptr_t gameBase = 0;

static inline void* GameAddr(size_t rva)
{
	return (void*)KlibAddress(gameBase, rva);
}


// --- Stats accumulator ---

struct ZoneTransitionStats
{
	double totalWallTimeMs;

	int    parseFileCallCount;
	double parseFileTotalMs;
	double parseFileMaxMs;

	int    loadZoneDataCallCount;
	double loadZoneDataTotalMs;

	int    updateMTFrameCount;
	double updateMTTotalMs;
	double updateMTMaxFrameMs;

	int    platoonActivateCount;
	double platoonActivateTotalMs;

	int    stateContCallCount;
	double stateContTotalMs;
	double stateContMaxMs;
	int    stateFrameCounts[6];  // frames in each state (0-5)
	double stateTimeMs[6];       // ms spent in each state

	int    contentInitCallCount;
	double contentInitTotalMs;

	int    contentStreamCallCount;
	double contentStreamTotalMs;

	int    buildingLoadCallCount;
	double buildingLoadTotalMs;

	int    mainLoopFrameCount;
	double mainLoopTotalMs;
	double mainLoopMaxMs;
	int    mainLoopOver50msCount;  // frames taking >50ms (blocking indicator)

	// State 4 gate diagnostics — which conditions block zone readiness?
	int    state4_totalFrames;
	int    state4_blockedBySections;   // manager+632 > 0
	int    state4_blockedByFlag;       // contentMgr+265 != 0
	int    state4_blockedByProcQueue;  // contentMgr+184 != null
	int    state4_blockedByInputQueue; // contentMgr+136 != null
	int    state4_sectionCountMax;     // peak section count observed
};

static bool                isTransitionActive = false;
static LARGE_INTEGER       transitionStartTime;
static LARGE_INTEGER       qpcFrequency;
static LARGE_INTEGER       pluginStartTime;
static ZoneTransitionStats currentStats;


// --- Log file path (next to DLL, like KenshiAddressLogger) ---

static std::string logFilePath;

static std::string GetDLLDirectory()
{
	char path[MAX_PATH];
	HMODULE hm = NULL;
	GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&GetDLLDirectory, &hm);
	GetModuleFileNameA(hm, path, sizeof(path));
	std::string dir(path);
	size_t pos = dir.find_last_of("\\/");
	if (pos != std::string::npos)
		dir = dir.substr(0, pos + 1);
	return dir;
}


// --- QPC helper ---

static inline double QPCToMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end)
{
	return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)qpcFrequency.QuadPart;
}


// --- Log output ---

static double ElapsedSec()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - pluginStartTime.QuadPart) / (double)qpcFrequency.QuadPart;
}

// Main thread only (DebugLog and ostringstream are not safe on the game's
// Havok threads). The file write goes through the audit reporter thread.
static void LogLine(const std::string& line)
{
	Audit_MarkProfilerLogFrame();
	DebugLog(line);

	std::ostringstream ts;
	ts << std::fixed << std::setprecision(3) << ElapsedSec() << ": " << line;
	Audit_LogProfiler(ts.str());
}

static void LogTransitionSummary(const ZoneTransitionStats& currentStats)
{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);

	LogLine("=== Zone Transition Profile ===");

	ss << "  Total: " << currentStats.totalWallTimeMs << " ms";
	LogLine(ss.str()); ss.str("");

	ss << "  parseFile: " << currentStats.parseFileCallCount << " calls, "
	   << currentStats.parseFileTotalMs << " ms total, "
	   << currentStats.parseFileMaxMs << " ms max";
	LogLine(ss.str()); ss.str("");

	ss << "  loadZoneData: " << currentStats.loadZoneDataCallCount << " calls, "
	   << currentStats.loadZoneDataTotalMs << " ms total";
	LogLine(ss.str()); ss.str("");

	ss << "  updateMT: " << currentStats.updateMTFrameCount << " frames, "
	   << currentStats.updateMTTotalMs << " ms total, "
	   << currentStats.updateMTMaxFrameMs << " ms max/frame";
	LogLine(ss.str()); ss.str("");

	ss << "  Platoon::activate: " << currentStats.platoonActivateCount << " calls, "
	   << currentStats.platoonActivateTotalMs << " ms total";
	LogLine(ss.str()); ss.str("");

	ss << "  stateMachine: " << currentStats.stateContCallCount << " calls, "
	   << currentStats.stateContTotalMs << " ms total, "
	   << currentStats.stateContMaxMs << " ms max";
	LogLine(ss.str()); ss.str("");

	ss << "    state[0]=" << currentStats.stateFrameCounts[0] << "f/"
	   << currentStats.stateTimeMs[0] << "ms"
	   << "  [1]=" << currentStats.stateFrameCounts[1] << "f/"
	   << currentStats.stateTimeMs[1] << "ms"
	   << "  [2]=" << currentStats.stateFrameCounts[2] << "f/"
	   << currentStats.stateTimeMs[2] << "ms"
	   << "  [3]=" << currentStats.stateFrameCounts[3] << "f/"
	   << currentStats.stateTimeMs[3] << "ms"
	   << "  [4]=" << currentStats.stateFrameCounts[4] << "f/"
	   << currentStats.stateTimeMs[4] << "ms"
	   << "  [5]=" << currentStats.stateFrameCounts[5] << "f/"
	   << currentStats.stateTimeMs[5] << "ms";
	LogLine(ss.str()); ss.str("");

	if (currentStats.state4_totalFrames > 0)
	{
		ss << "    state4 gates (" << currentStats.state4_totalFrames << "f): "
		   << "sections=" << currentStats.state4_blockedBySections << "f (max=" << currentStats.state4_sectionCountMax << "), "
		   << "flag265=" << currentStats.state4_blockedByFlag << "f, "
		   << "procQueue=" << currentStats.state4_blockedByProcQueue << "f, "
		   << "inputQueue=" << currentStats.state4_blockedByInputQueue << "f";
		LogLine(ss.str()); ss.str("");
	}

	ss << "  contentInit: " << currentStats.contentInitCallCount << " calls, "
	   << currentStats.contentInitTotalMs << " ms total";
	LogLine(ss.str()); ss.str("");

	ss << "  contentStream: " << currentStats.contentStreamCallCount << " calls, "
	   << currentStats.contentStreamTotalMs << " ms total";
	LogLine(ss.str()); ss.str("");

	ss << "  buildingLoad: " << currentStats.buildingLoadCallCount << " calls, "
	   << currentStats.buildingLoadTotalMs << " ms total";
	LogLine(ss.str()); ss.str("");

	ss << "  mainLoop: " << currentStats.mainLoopFrameCount << " frames, "
	   << currentStats.mainLoopTotalMs << " ms total, "
	   << currentStats.mainLoopMaxMs << " ms max/frame, "
	   << currentStats.mainLoopOver50msCount << " frames >50ms";
	LogLine(ss.str()); ss.str("");

	// Note: stateMachine time includes loadZoneData calls (minimal overlap).
	// contentInit/contentStream/buildingLoad may include parseFile calls.
	double accounted = currentStats.parseFileTotalMs + currentStats.loadZoneDataTotalMs
	                 + currentStats.updateMTTotalMs + currentStats.platoonActivateTotalMs
	                 + currentStats.stateContTotalMs + currentStats.contentInitTotalMs
	                 + currentStats.contentStreamTotalMs + currentStats.buildingLoadTotalMs;
	double unaccounted = currentStats.totalWallTimeMs - accounted;
	double pct = (currentStats.totalWallTimeMs > 0.0)
	           ? (unaccounted / currentStats.totalWallTimeMs * 100.0) : 0.0;
	ss << "  Unaccounted: " << unaccounted << " ms (" << pct << "%)";
	LogLine(ss.str());

	LogLine("=== End Zone Profile ===");
}


// --- FPS tracking (frame-to-frame intervals, outside transitions) ---

static LARGE_INTEGER fpsPrevFrameStart;
static bool          fpsTrackingValid = false;
static int           fpsFrameCount    = 0;
static double        fpsTotalMs       = 0.0;
static double        fpsMaxMs         = 0.0;
static double        fpsMinMs         = 999999.0;
static int           fpsOver16Count   = 0;   // >16.67ms = below 60fps
static int           fpsOver33Count   = 0;   // >33.33ms = below 30fps
static double        fpsLastLogTime   = 0.0;

static void ResetFPSTracking()
{
	fpsFrameCount  = 0;
	fpsTotalMs     = 0.0;
	fpsMaxMs       = 0.0;
	fpsMinMs       = 999999.0;
	fpsOver16Count = 0;
	fpsOver33Count = 0;
	fpsLastLogTime = ElapsedSec();
}

static void LogFPSReport(const char* label, double windowSec)
{
	if (fpsFrameCount < 1)
		return;

	double avgMs   = fpsTotalMs / fpsFrameCount;
	double avgFPS  = 1000.0 / avgMs;
	double pctSub60 = 100.0 * fpsOver16Count / fpsFrameCount;
	double pctSub30 = 100.0 * fpsOver33Count / fpsFrameCount;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "[FPS] " << label << " (" << fpsFrameCount << "f / "
	   << windowSec << "s): avg=" << avgFPS << " fps (" << avgMs << "ms)"
	   << ", min=" << fpsMinMs << "ms, max=" << fpsMaxMs << "ms"
	   << ", <60fps=" << pctSub60 << "%, <30fps=" << pctSub30 << "%";
	if (Audit_LegacyFpsEnabled())
		LogLine(ss.str());
}


// =========================================================================
// Hook 1: showLoadingMessage - brackets the entire zone transition
// =========================================================================

typedef void (*showLoadingMessage_t)(void* thisPtr, bool on);
showLoadingMessage_t showLoadingMessage_orig = nullptr;

// The dismissal (on=false) can arrive on the contentStream thread, where
// ostringstream/DebugLog are unsafe. The summary is snapshotted here and
// logged by the main thread at the next mainLoop.
static ZoneTransitionStats pendingStats;
static volatile LONG       pendingSummary = 0;

static void FlushPendingTransitionSummary()
{
	if (InterlockedCompareExchange(&pendingSummary, 0, 1) != 1)
		return;
	LogTransitionSummary(pendingStats);
	ResetFPSTracking();
	fpsTrackingValid = false;
}

void showLoadingMessage_hook(void* thisPtr, bool on)
{
	Audit_TransitionEdge(on);

	if (on && !isTransitionActive)
	{
		if (Audit_IsMainThread())
		{
			FlushPendingTransitionSummary();

			// Log gameplay FPS accumulated since last transition
			if (fpsFrameCount > 0)
			{
				double now = ElapsedSec();
				LogFPSReport("pre-transition", now - fpsLastLogTime);
			}
			ResetFPSTracking();
		}
		fpsTrackingValid = false;

		isTransitionActive = true;
		memset(&currentStats, 0, sizeof(currentStats));
		QueryPerformanceCounter(&transitionStartTime);
	}
	else if (!on && isTransitionActive)
	{
		LARGE_INTEGER endTime;
		QueryPerformanceCounter(&endTime);
		currentStats.totalWallTimeMs = QPCToMs(transitionStartTime, endTime);
		isTransitionActive = false;
		pendingStats = currentStats;
		InterlockedExchange(&pendingSummary, 1);
		fpsTrackingValid = false;
	}

	showLoadingMessage_orig(thisPtr, on);
}


// =========================================================================
// Hook 2: parseFile - the primary I/O bottleneck
// =========================================================================

typedef bool (*parseFile_t)(void* thisPtr, void* filePath, void* defaultData,
                            unsigned short version, void* callback, bool skipInstances);
parseFile_t parseFile_orig = nullptr;

bool parseFile_hook(void* thisPtr, void* filePath, void* defaultData,
                    unsigned short version, void* callback, bool skipInstances)
{
	if (!isTransitionActive)
		return parseFile_orig(thisPtr, filePath, defaultData, version, callback, skipInstances);

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	bool result = parseFile_orig(thisPtr, filePath, defaultData, version, callback, skipInstances);
	QueryPerformanceCounter(&end);

	double ms = QPCToMs(start, end);
	currentStats.parseFileCallCount++;
	currentStats.parseFileTotalMs += ms;
	if (ms > currentStats.parseFileMaxMs)
		currentStats.parseFileMaxMs = ms;

	return result;
}


// =========================================================================
// Hook 3: loadZoneData - zone + 3x3 neighbor loading
// =========================================================================

typedef void (*loadZoneData_t)(void* zoneMgr, void* zoneEntry, __int64 zoneId,
                               int radius, int param1, int param2);
loadZoneData_t loadZoneData_orig = nullptr;

void loadZoneData_hook(void* zoneMgr, void* zoneEntry, __int64 zoneId,
                       int radius, int param1, int param2)
{
	if (!isTransitionActive)
	{
		loadZoneData_orig(zoneMgr, zoneEntry, zoneId, radius, param1, param2);
		return;
	}

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	loadZoneData_orig(zoneMgr, zoneEntry, zoneId, radius, param1, param2);
	QueryPerformanceCounter(&end);

	double ms = QPCToMs(start, end);
	currentStats.loadZoneDataCallCount++;
	currentStats.loadZoneDataTotalMs += ms;
}


// =========================================================================
// Hook 4: ResourceLoader::updateMT - per-frame mesh finalization
// =========================================================================

typedef void (*updateMT_t)(void* thisPtr);
updateMT_t updateMT_orig = nullptr;

void updateMT_hook(void* thisPtr)
{
	if (!isTransitionActive)
	{
		Audit_MeshesEnter();
		updateMT_orig(thisPtr);
		Audit_MeshesExit();
		return;
	}

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	Audit_MeshesEnter();
	updateMT_orig(thisPtr);
	Audit_MeshesExit();
	QueryPerformanceCounter(&end);

	double ms = QPCToMs(start, end);
	currentStats.updateMTFrameCount++;
	currentStats.updateMTTotalMs += ms;
	if (ms > currentStats.updateMTMaxFrameMs)
		currentStats.updateMTMaxFrameMs = ms;
}


// =========================================================================
// Hook 5: Platoon::activate - platoon spawn cost
// =========================================================================

typedef void (*platoonActivate_t)(void* thisPtr);
platoonActivate_t platoonActivate_orig = nullptr;

void platoonActivate_hook(void* thisPtr)
{
	Audit_SquadActivated();
	if (!isTransitionActive)
	{
		platoonActivate_orig(thisPtr);
		return;
	}

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	platoonActivate_orig(thisPtr);
	QueryPerformanceCounter(&end);

	double ms = QPCToMs(start, end);
	currentStats.platoonActivateCount++;
	currentStats.platoonActivateTotalMs += ms;
}


// =========================================================================
// Hook 10: GameWorld::mainLoop - total per-frame time during transitions
// =========================================================================

typedef void (*mainLoop_t)(void* thisPtr, float time);
mainLoop_t mainLoop_orig = nullptr;

void mainLoop_hook(void* thisPtr, float time)
{
	FlushPendingTransitionSummary();

	LARGE_INTEGER frameStart;
	QueryPerformanceCounter(&frameStart);

	// FPS: measure frame-to-frame interval (outside transitions only)
	if (fpsTrackingValid && !isTransitionActive)
	{
		double intervalMs = QPCToMs(fpsPrevFrameStart, frameStart);
		fpsFrameCount++;
		fpsTotalMs += intervalMs;
		if (intervalMs > fpsMaxMs) fpsMaxMs = intervalMs;
		if (intervalMs < fpsMinMs) fpsMinMs = intervalMs;
		if (intervalMs > 16.667) fpsOver16Count++;
		if (intervalMs > 33.333) fpsOver33Count++;
	}
	fpsPrevFrameStart = frameStart;
	fpsTrackingValid = true;

	if (!isTransitionActive)
	{
		Audit_MainLoopEnter(time);
		mainLoop_orig(thisPtr, time);
		Audit_MainLoopExit();

		// Periodic FPS report (~10s)
		if (fpsFrameCount > 0)
		{
			double now = ElapsedSec();
			if (now - fpsLastLogTime > 10.0)
			{
				LogFPSReport("periodic", now - fpsLastLogTime);
				ResetFPSTracking();
			}
		}
		return;
	}

	Audit_MainLoopEnter(time);
	mainLoop_orig(thisPtr, time);
	Audit_MainLoopExit();

	LARGE_INTEGER end;
	QueryPerformanceCounter(&end);

	// Note: isTransitionActive may have changed inside mainLoop
	// (showLoadingMessage(false) fires from within). Still record
	// this frame since it was part of the transition.
	double ms = QPCToMs(frameStart, end);
	currentStats.mainLoopFrameCount++;
	currentStats.mainLoopTotalMs += ms;
	if (ms > currentStats.mainLoopMaxMs)
		currentStats.mainLoopMaxMs = ms;
	if (ms > 50.0)
		currentStats.mainLoopOver50msCount++;
}


// =========================================================================
// Hook 6: State machine continuation - per-frame zone streaming driver
// =========================================================================

typedef __int64 (*stateCont_t)(void*);
stateCont_t stateCont_orig = nullptr;

__int64 stateCont_hook(void* a1)
{
	bool wasDuringTransition = isTransitionActive;
	int stateBefore = *(int*)(KLIB_MEMBER(5, (uintptr_t)a1, ZoneManager_loadingPhase, 1475032));

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	__int64 result = stateCont_orig(a1);
	QueryPerformanceCounter(&end);

	Audit_StateMachineMs(QPCToMs(start, end));

	if (wasDuringTransition || isTransitionActive)
	{
		double ms = QPCToMs(start, end);
		currentStats.stateContCallCount++;
		currentStats.stateContTotalMs += ms;
		if (ms > currentStats.stateContMaxMs)
			currentStats.stateContMaxMs = ms;

		if (stateBefore >= 0 && stateBefore <= 5)
		{
			currentStats.stateFrameCounts[stateBefore]++;
			currentStats.stateTimeMs[stateBefore] += ms;
		}

		// State 4 gate diagnostics: sample the 4 conditions BEFORE the frame runs
		if (stateBefore == 4)
		{
			currentStats.state4_totalFrames++;
			uintptr_t mgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
			if (mgr)
			{
				int sectionCount = *(int*)(KLIB_MEMBER(5, mgr, NavMesh_addList_count, 632));
				if (sectionCount > currentStats.state4_sectionCountMax)
					currentStats.state4_sectionCountMax = sectionCount;
				if (sectionCount > 0)
					currentStats.state4_blockedBySections++;

				uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(5, mgr, NavMesh_generator, 656));
				if (cm)
				{
					if (*(unsigned char*)(KLIB_MEMBER(5, cm, NavMeshGenerator_doingStuff, 265)))
						currentStats.state4_blockedByFlag++;
					if (*(uintptr_t*)(KLIB_MEMBER(5, cm, NavMeshGenerator_done_front, 184)))
						currentStats.state4_blockedByProcQueue++;
					if (*(uintptr_t*)(KLIB_MEMBER(5, cm, NavMeshGenerator_queue_front, 136)))
						currentStats.state4_blockedByInputQueue++;
				}
			}
		}
	}

	return result;
}


// =========================================================================
// Hook 7: Zone content initialization (buildings/objects, iterates 4096 zones)
// =========================================================================

typedef void (*contentInit_t)(void*, void*);
contentInit_t contentInit_orig = nullptr;

void contentInit_hook(void* a1, void* a2)
{
	bool wasDuringTransition = isTransitionActive;

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	contentInit_orig(a1, a2);
	QueryPerformanceCounter(&end);

	if (wasDuringTransition || isTransitionActive)
	{
		double ms = QPCToMs(start, end);
		currentStats.contentInitCallCount++;
		currentStats.contentInitTotalMs += ms;
	}
}


// =========================================================================
// Hook 8: Major content streaming (3159 bytes, 54 callees)
// =========================================================================

typedef char (*contentStream_t)(void*, void*, void*, double);
contentStream_t contentStream_orig = nullptr;

char contentStream_hook(void* a1, void* a2, void* a3, double a4)
{
	bool wasDuringTransition = isTransitionActive;

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	char result = contentStream_orig(a1, a2, a3, a4);
	QueryPerformanceCounter(&end);

	if (wasDuringTransition || isTransitionActive)
	{
		double ms = QPCToMs(start, end);
		currentStats.contentStreamCallCount++;
		currentStats.contentStreamTotalMs += ms;
	}

	return result;
}


// =========================================================================
// Hook 9: Building/town content load
// =========================================================================

typedef void (*buildingLoad_t)(void*, void*);
buildingLoad_t buildingLoad_orig = nullptr;

void buildingLoad_hook(void* a1, void* a2)
{
	bool wasDuringTransition = isTransitionActive;

	LARGE_INTEGER start, end;
	QueryPerformanceCounter(&start);
	buildingLoad_orig(a1, a2);
	QueryPerformanceCounter(&end);

	if (wasDuringTransition || isTransitionActive)
	{
		double ms = QPCToMs(start, end);
		currentStats.buildingLoadCallCount++;
		currentStats.buildingLoadTotalMs += ms;
	}
}


// =========================================================================
// Plugin entry point - called by RE_Kenshi's plugin loader
// =========================================================================

__declspec(dllexport) void startPlugin()
{
	gameBase = (uintptr_t)GetModuleHandleA(NULL);
	QueryPerformanceFrequency(&qpcFrequency);
	QueryPerformanceCounter(&pluginStartTime);
	logFilePath = GetDLLDirectory() + "KenshiZoneProfiler.log";

	// Shared parity gate precedes all reporter work and hook installation.
	if (!InitKlibBindings(gameBase, static_cast<void (*)(const char*)>(&DebugLog)))
	{
		ErrorLog("Binding gate refused initialization; no hooks installed");
		return;
	}

	// Reporter thread (all file output), INI, main-thread id.
	Audit_Init(gameBase, GetDLLDirectory());

	int installed = 0;

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_SHOW_LOADING_MESSAGE), showLoadingMessage_hook, &showLoadingMessage_orig))
		installed++;
	else
		ErrorLog("FAILED to hook showLoadingMessage");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_PARSE_FILE), parseFile_hook, &parseFile_orig))
		installed++;
	else
		ErrorLog("FAILED to hook parseFile");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_LOAD_ZONE_DATA), loadZoneData_hook, &loadZoneData_orig))
		installed++;
	else
		ErrorLog("FAILED to hook loadZoneData");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_UPDATE_MT), updateMT_hook, &updateMT_orig))
		installed++;
	else
		ErrorLog("FAILED to hook updateMT");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_PLATOON_ACTIVATE), platoonActivate_hook, &platoonActivate_orig))
		installed++;
	else
		ErrorLog("FAILED to hook Platoon::activate");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_STATE_CONTINUATION), stateCont_hook, &stateCont_orig))
		installed++;
	else
		ErrorLog("FAILED to hook stateContinuation");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_ZONE_CONTENT_INIT), contentInit_hook, &contentInit_orig))
		installed++;
	else
		ErrorLog("FAILED to hook zoneContentInit");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_CONTENT_STREAMING), contentStream_hook, &contentStream_orig))
		installed++;
	else
		ErrorLog("FAILED to hook contentStreaming");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_BUILDING_LOAD), buildingLoad_hook, &buildingLoad_orig))
		installed++;
	else
		ErrorLog("FAILED to hook buildingLoad");

	if (KenshiLib::SUCCESS == KenshiLib::AddHook(GameAddr(RVA_MAIN_LOOP), mainLoop_hook, &mainLoop_orig))
		installed++;
	else
		ErrorLog("FAILED to hook mainLoop");

	std::ostringstream msg;
	msg << "Initialized - " << installed << "/10 hooks installed"
	    << ", klibStep=" << 5 << ", klibMembers=" << 5;
	DebugLog(msg.str());

	// Frame audit hooks and call-site probes (KenshiFrameAudit.cpp).
	Audit_Install();
}
