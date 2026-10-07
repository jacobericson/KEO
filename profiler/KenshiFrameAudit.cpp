#include "game/klib_members.h"
// KenshiFrameAudit.cpp - Frame audit entry points, Ogre symbol names and module state.
// Each audit_*.cpp lead names its own threads; no probe takes a lock, allocates, or logs.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <process.h>
#include <intrin.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include <core/Functions.h>
#include <Debug.h>
// GetKenshiVersion. The header's inline bodies trip C4482 (KenshiLib's code).
#pragma warning(push)
#pragma warning(disable: 4482)
#include <kenshi/Kenshi.h>
#pragma warning(pop)

#include "CallSiteProbe.h"
#include "KenshiFrameAudit.h"
#include "KenshiFrameAudit_internal.h"
#include "game/klib_bindings.h"
#include "base/ini_names.h"
#include "base/legacy_ini_import.h"
#include "audit_detail.h"
#include "audit_steady.h"
#include "audit_offmain.h"

namespace audit {
LONGLONG g_qpcFreq = 1;
}


namespace kenshiframeaudit_detail {
const char* OGRE_DLL                 = "OgreMain_x64.dll";
const char* SYM_RENDER_ONE_FRAME     = "?renderOneFrame@Root@Ogre@@QEAA_NXZ";
const char* SYM_UPDATE_SCENE_GRAPH   = "?updateSceneGraph@SceneManager@Ogre@@QEAAXXZ";
const char* SYM_CM2_UPDATE           = "?_update@CompositorManager2@Ogre@@QEAAXXZ";
const char* SYM_CM2_SWAP             = "?_swapAllFinalTargets@CompositorManager2@Ogre@@QEAAXXZ";
const char* SYM_RS_RENDER            = "?_render@RenderSystem@Ogre@@UEAAXAEBVRenderOperation@2@@Z";
const char* SYM_GET_RENDER_STAGE     = "?_getCurrentRenderStage@SceneManager@Ogre@@QEBA?AW4IlluminationRenderStage@12@XZ";
const char* SYM_GET_WORKER_THREADS   = "?getNumWorkerThreads@SceneManager@Ogre@@QEBA_KXZ";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* SYM_RENDER_PHASE02 = "?_renderPhase02@SceneManager@Ogre@@UEAAXPEAVCamera@2@PEBV32@PEAVViewport@2@EE_N@Z";
const char* SYM_CULL_PHASE01   = "?_cullPhase01@SceneManager@Ogre@@UEAAXPEAVCamera@2@PEBV32@PEAVViewport@2@EE@Z";
const char* SYM_RENDER_VISIBLE = "?_renderVisibleObjects@SceneManager@Ogre@@UEAAXXZ";
const char* SYM_RSO            = "?renderSingleObject@SceneManager@Ogre@@IEAAXPEAVRenderable@2@PEBVPass@2@_N2@Z";
const char* SYM_SET_PASS       = "?_setPass@SceneManager@Ogre@@UEAAPEBVPass@2@PEBV32@_N1@Z";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* SYM_MO_GET_NAME    = "?getName@MovableObject@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";
const char* SYM_SUBENT_PARENT  = "?getParent@SubEntity@Ogre@@QEBAPEAVEntity@2@XZ";
const char* SYM_ENT_GET_MESH   = "?getMesh@Entity@Ogre@@QEBAAEBV?$SharedPtr@VMesh@Ogre@@@2@XZ";
const char* SYM_BATCH_MESH_REF = "?_getMeshReference@InstanceBatch@Ogre@@QEBAAEBV?$SharedPtr@VMesh@Ogre@@@2@XZ";
const char* SYM_RES_GET_NAME   = "?getName@Resource@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";
const char* SYM_VP_WIDTH       = "?getActualWidth@Viewport@Ogre@@QEBAHXZ";
const char* SYM_VP_HEIGHT      = "?getActualHeight@Viewport@Ogre@@QEBAHXZ";
const char* SYM_VP_TARGET      = "?getTarget@Viewport@Ogre@@QEBAPEAVRenderTarget@2@XZ";
const char* SYM_RT_GET_NAME    = "?getName@RenderTarget@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* SYM_BARRIER_SYNC   = "?sync@Barrier@Ogre@@QEAAXXZ";
const char* SYM_OLD_ANIMS      = "?updateAllOldAnimations@SceneManager@Ogre@@IEAAXXZ";
const char* SYM_GET_VIS_FLAGS  = "?getVisibilityFlags@MovableObject@Ogre@@QEBAIXZ";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char*  D3D11_DLL           = "RenderSystem_Direct3D11_x64.dll";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* PU_DLL             = "Plugin_ParticleUniverse_x64.dll";
const char* SYM_PU_UPDATE      = "?_update@ParticleSystem@ParticleUniverse@@QEAAXM@Z";
const char* SYM_PU_PARTICLES   = "?getNumberOfEmittedParticles@ParticleSystem@ParticleUniverse@@QEAA_KXZ";
const char* SYM_PU_TEMPLATE    = "?getTemplateName@ParticleSystem@ParticleUniverse@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";
const char* SYM_ROOT_SINGLETON = "?getSingletonPtr@Root@Ogre@@SAPEAV12@XZ";
const char* SYM_ROOT_NEXT_FRAME = "?getNextFrameNumber@Root@Ogre@@QEBAKXZ";
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* METRIC_NAMES[NUM_METRICS] = { AUDIT_METRICS(AUDIT_NAME) };
const char* COUNT_NAMES[NUM_COUNTS]   = { AUDIT_COUNTS(AUDIT_NAME) };
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* CURCLASS_NAMES[NUM_CURCLASSES] = { "still", "small", "large" };
const char* SPLITRAY_NAMES[NUM_SPLITRAYS] = { "real", "full", "static", "dynA", "dynB", "dynNoChar" };
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const char* CLASS_NAMES[NUM_CLASSES] = { "steady", "stream", "paused", "menu" };
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
int ClassOf(const FrameRec& r)
{
	if (!(r.flags & F_INGAME)) return CLS_MENU;
	if (r.flags & F_PAUSED)    return CLS_PAUSED;
	if (r.flags & (F_TRANS | F_ZONEBUSY)) return CLS_STREAM;
	return CLS_STEADY;
}

bool Excluded(const FrameRec& r)
{
	return (r.flags & (F_STALL | F_BROKEN | F_PROFLOG | F_CURSORSPLIT)) != 0 || !(r.flags & F_FG);
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

Config g_cfg;

} // audit


namespace kenshiframeaudit_detail {

// =========================================================================
// Globals and time
// =========================================================================

uintptr_t   g_base         = 0;
uintptr_t   g_exeEnd       = 0;       // g_base + SizeOfImage

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

DWORD       g_mainThreadId = 0;
volatile DWORD g_aiThreadId = 0;      // the AI worker, recorded at its first body run
volatile DWORD g_physThreadId = 0;    // Kenshi's PhysicsActual worker
volatile LONG  g_physPhase = PP_IDLE; // sampled by main-thread queries
volatile LONG  g_physActiveSeq = 0;   // physics run currently in the body
LONGLONG       g_physPhaseStart = 0;  // physics thread only
LONGLONG    g_qpcStart     = 0;

} // audit


namespace kenshiframeaudit_detail {
std::string g_dllDir;
std::string g_runName;        // yyyymmdd_hhmmss[_tag]

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
std::vector<QueuedLine> g_lines;               // guarded by g_lineCS
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
FILE* g_profLog  = NULL;
FILE* g_auditLog = NULL;
FILE* g_secCsv   = NULL;
FILE* g_frameCsv = NULL;
FILE* g_physCsv  = NULL;
FILE* g_physqCsv = NULL;
FILE* g_cpuCsv   = NULL;

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
std::vector<FrameRec> g_window;
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
std::vector<Bucket> g_buckets;
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
SecondAcc g_sec;
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

// =========================================================================
// Worker-thread slots (AI, birds, physics)
// =========================================================================

ThreadSlot g_ai, g_birds, g_phys;

} // audit


namespace audit {

CurFrame      g_cur;

CursorState   g_cursor;

} // audit


namespace kenshiframeaudit_detail {

unsigned      g_nextSeq      = 1;
bool          g_installed    = false;
bool          g_boundaryR    = false;   // frames open at Root::renderOneFrame
int           g_rofDepth     = 0;
volatile LONG g_transOpen    = 0;       // showLoadingMessage bracket open
volatile LONG g_transSeen    = 0;       // any edge since the last frame close
volatile LONG g_squadsPending = 0;      // Platoon::activate calls since the last close
bool          g_foreground   = true;
LONGLONG      g_last1Hz      = 0;
int           g_zoneRow      = 0;
int           g_rowLoaded[ZONE_GRID];
int           g_zLoaded      = 0;
int           g_platoons     = 0;
float         g_camAlt       = 0.0f;
bool          g_headerDone   = false;
std::string   g_lastSettings;
void*         g_sceneMgr     = NULL;    // main SceneManager, refreshed at frame open

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
GetRenderStage_t   g_getStage   = NULL;
GetWorkerThreads_t g_getWorkers = NULL;

// Ogre accessors (this -> field), resolved from OgreMain_x64.dll exports.
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
OgreGetter_t    g_moGetName   = NULL;   // MovableObject::getName -> const String&
OgreGetter_t    g_subParent   = NULL;   // SubEntity::getParent -> Entity*
OgreGetter_t    g_entMesh     = NULL;   // Entity::getMesh -> const MeshPtr&
OgreGetter_t    g_batchMesh   = NULL;   // InstanceBatch::_getMeshReference -> const MeshPtr&
OgreGetter_t    g_resGetName  = NULL;   // Resource::getName -> const String&
OgreGetter_t    g_vpTarget    = NULL;   // Viewport::getTarget -> RenderTarget*
OgreGetter_t    g_rtGetName   = NULL;   // RenderTarget::getName -> const String&
OgreIntGetter_t g_vpWidth     = NULL;   // Viewport::getActualWidth
OgreIntGetter_t g_vpHeight    = NULL;   // Viewport::getActualHeight

bool g_renderOn = false;   // scene-call hooks installed and RenderDetail=1
bool g_cmHooked = false;   // CompositorManager2::_update (the `passes` term)
bool g_haveTag[ST_COUNT];  // SiteTag -> probe installed
bool g_bodyHooked = false; // CharBody::update entry hook (list-1 task time)
bool g_moveHooked = false; // CharMovement::update entry hook (list-1 movement time)
bool g_physBodyHooked = false; // publishes detailed physics runs and live phase
bool g_physMakeHooks = false;  // all nontrivial slot +0x30/+0x38 implementations
bool g_physApplyHooks = false; // PhysicsHullT and DoorPhysXEntity slot +0x28
const char* g_hullStatus = "off"; // [Audit] HullDiag: on / off / refused
bool g_syncHooked    = false;   // Barrier::sync
bool g_oldAnimHooked = false;   // SceneManager::updateAllOldAnimations
bool g_bindHooked    = false;   // D3D11RenderSystem::bindGpuProgramParameters
bool g_d3dHooked     = false;   // D3D11RenderSystem::_render
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
VisFlags_t g_getVisFlags = NULL;   // MovableObject::getVisibilityFlags

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
CallCtx   g_stack[MAX_DEPTH];
int       g_depth = 0;

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
PendingCull g_pending[MAX_PENDING];
int         g_npending = 0;

// The renderSingleObject in flight, for the draws it issues.
int  g_rsoDepth = 0;
int  g_rsoCls   = 0;
int  g_rsoMesh  = 0;
bool g_rsoBatch = false;

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

// Main thread, startup: the settings import's log writer. Profiler-log lines carry the
// "seconds: " stamp; before the reporter starts, the queue writes them directly.
static void LegacyIniProfilerLog(const std::string& line)
{
	char prefix[32];
	_snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "%.3f: ", SinceStart(Now()));
	Audit_LogProfiler(std::string(prefix) + line);
}


// =========================================================================
// Public API
// =========================================================================

void Audit_Init(uintptr_t gameBase, const std::string& dllDir)
{
	g_base         = gameBase;
	g_dllDir       = dllDir;
	g_mainThreadId = GetCurrentThreadId();
	{
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)gameBase;
		const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(gameBase + dos->e_lfanew);
		g_exeEnd = gameBase + nt->OptionalHeader.SizeOfImage;
	}
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	g_qpcFreq  = f.QuadPart ? f.QuadPart : 1;
	g_qpcStart = Now();

	LegacyIniImport(g_dllDir, LEGACY_PROFILER_INI_NAME, PROFILER_INI_NAME, NULL, false, &LegacyIniProfilerLog);
	LoadConfig();
	InitNameTables();
	memset(g_haveTag, 0, sizeof(g_haveTag));

	time_t now = time(NULL);
	struct tm lt;
	localtime_s(&lt, &now);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);
	g_runName = stamp;
	if (!g_cfg.tag.empty())
		g_runName += "_" + g_cfg.tag;
	if (g_cfg.timerExperiment)
		g_runName += "_timer1";

	if (g_cfg.timerExperiment)
	{
		typedef UINT (WINAPI *TimeBeginPeriod_t)(UINT);
		HMODULE winmm = LoadLibraryA("winmm.dll");
		TimeBeginPeriod_t tbp = winmm ? (TimeBeginPeriod_t)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
		if (tbp)
			tbp(1);
	}

	if (g_cfg.enabled)
		g_ring = new FrameRec[(size_t)RING_SIZE];

	InitializeCriticalSection(&g_lineCS);
	g_lineCSReady = true;
	g_reporter = (HANDLE)_beginthreadex(NULL, 0, &ReporterProc, NULL, 0, NULL);
	if (g_reporter)
	{
		SetThreadPriority(g_reporter, THREAD_PRIORITY_BELOW_NORMAL);
		InterlockedExchange(&g_reporterRunning, 1);
	}
}

void Audit_Install()
{
	if (!g_cfg.enabled)
	{
		AuditLine("[Audit] disabled ([Audit] Enabled=0)");
		return;
	}

	KenshiLib::BinaryVersion ver = KenshiLib::GetKenshiVersion();
	bool steam = ver.GetVersion() == "1.0.65" && ver.GetPlatform() == KenshiLib::BinaryVersion::STEAM;
	AuditLine("[Audit] Kenshi build: " + ver.ToString() + (steam ? "" : " (exe hooks and sites need Steam 1.0.65: disabled)"));

	bool haveRenderOneFrame = false;
	InstallOgreHooks(GetModuleHandleA(OGRE_DLL), &haveRenderOneFrame);
	InstallParticleHooks(GetModuleHandleA(OGRE_DLL));

	int sitesOk = 0, sitesTotal = 0;
	bool listeners = false;
	if (steam)
	{
		listeners = InstallExeHooks();
		sitesOk   = InstallSites(&sitesTotal);
	}
	InstallCursor(steam);
	InstallOffMain(steam);
	// The census reads exe structures (Steam 1.0.65) at the `particles` site.
	g_fxCensusOn = g_cfg.particles && g_fxLayoutOk && steam && g_haveTag[ST_PARTICLES];

	// Without all three frame listeners there is nothing to attribute: keep
	// every hook passive (renderOneFrame included) rather than emit BROKEN frames.
	g_boundaryR = haveRenderOneFrame && listeners;
	g_installed = listeners;
	bool listenerTimer = Listeners_Init(g_cfg.listeners && listeners);

	const char* fx = g_fxHooked ? (g_fxCensusOn ? "on" : "timing-only")
	                            : (g_fxCensusOn ? "census-only" : "off");
	std::string cursor = !g_haveTag[ST_MOUSERAY] ? std::string("off")
		: (g_cursor.splitOn ? Fmt("split/%d", g_cfg.cursorSplitEvery)
		                    : std::string(g_cursor.layoutOk ? "on" : "timing-only"));
	const char* physx = !g_cfg.physxDetail ? "off"
		: (g_physBodyHooked && g_haveTag[ST_MOUSERAY] &&
		   g_physMakeHooks && g_physApplyHooks && g_haveTag[ST_PHYS_FETCH] &&
		   oIsIndoors ? "on" : "partial");
	std::string summary = Fmt("[Audit] Installed: hooks %d/%d sites %d/%d page=%p draws=%s render=%s fx=%s cursor=%s physx=%s listeners=%s hulls=%s cpu=%s steady=%s offmain=%s boundary=%s timerExp=%d run=%s%s",
	                          g_hooksOk, g_hooksTotal, sitesOk, sitesTotal, CallSiteProbe::StubPage(),
	                          g_cfg.draws ? "on" : "off", g_renderOn ? "on" : "off", fx, cursor.c_str(), physx,
	                          listenerTimer ? "on" : "off", g_hullStatus, g_cfg.cpuSample ? "on" : "off", SteadyStatus(), OffMainStatus(), g_boundaryR ? "renderOneFrame" : "frameStarted",
	                          g_cfg.timerExperiment ? 1 : 0, g_runName.c_str(),
	                          listeners ? "" : " (frame listeners missing: audit inactive)");
	AuditLine(summary);
	DebugLog(summary);

	// The bench tool aligns "Bench ..." qpc= values (KEO.log, a
	// different plugin, same process clock) to the audit's own seconds.
	std::string qpcLine = Fmt("[Audit] qpcStart=%lld qpcFreq=%lld", g_qpcStart, g_qpcFreq);
	AuditLine(qpcLine);
	DebugLog(qpcLine);
}

bool Audit_IsMainThread()
{
	return IsMain();
}

bool Audit_LegacyFpsEnabled()
{
	return g_cfg.legacyFps;
}

void Audit_LogProfiler(const std::string& line)
{
	QueueLine(LOG_PROFILER, line);
}

void Audit_MainLoopEnter(float time)
{
	if (!g_installed || !IsMain() || !g_cur.open)
		return;
	uintptr_t gw = KlibAddress(g_base, RVA_GAMEWORLD);
	g_cur.speed = *(const float*)(KLIB_MEMBER(5, gw, GameWorld_frameSpeedMult, GW_SPEED));
	g_cur.dtMs  = time * 1000.0f;
	if (*(const unsigned char*)(KLIB_MEMBER(5, gw, GameWorld_paused, GW_PAUSED)))
		g_cur.flags |= F_PAUSED;
	g_cur.M0  = Now();
	g_cur.hM0 = true;
}

void Audit_MainLoopExit()
{
	if (!g_installed || !IsMain() || !g_cur.open || !g_cur.hM0)
		return;
	g_cur.M1  = Now();
	g_cur.hM1 = true;
	SampleWorld();
}

void Audit_MeshesEnter()
{
	if (!g_installed || !IsMain() || !g_cur.open)
		return;
	g_cur.meshT0 = Now();
}

void Audit_MeshesExit()
{
	if (!g_installed || !IsMain() || !g_cur.open || !g_cur.meshT0)
		return;
	g_cur.ml[M_ML_MESH - ML_FIRST] += Now() - g_cur.meshT0;
	g_cur.meshT0 = 0;
}

void Audit_StateMachineMs(double ms)
{
	if (!g_installed || !IsMain() || !g_cur.open)
		return;
	g_cur.zoneSMms += (float)ms;
}

void Audit_SquadActivated()
{
	if (g_installed)
		InterlockedIncrement(&g_squadsPending);
}

void Audit_TransitionEdge(bool on)
{
	// contentStream also calls showLoadingMessage(false) with no bracket open
	// (after any section batch), so only a real change of state counts.
	LONG v = on ? 1 : 0;
	if (InterlockedExchange(&g_transOpen, v) != v)
		InterlockedExchange(&g_transSeen, 1);
}

void Audit_MarkProfilerLogFrame()
{
	if (g_installed && IsMain() && g_cur.open)
		g_cur.flags |= F_PROFLOG;
}
