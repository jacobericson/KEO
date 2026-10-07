// KenshiFrameAudit internals shared by the audit's own translation units.
// Everything else in KenshiFrameAudit.cpp stays file-local.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>
#include <string.h>
#include <string>

#include "game/klib_member_contract.h"

namespace audit
{

extern LONGLONG g_qpcFreq;   // QueryPerformanceFrequency, set in Audit_Init

inline LONGLONG Now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

inline float TicksToMs(LONGLONG ticks)
{
	return (float)((double)ticks * 1000.0 / (double)g_qpcFreq);
}

inline bool PlausiblePtr(uintptr_t p)
{
	return p >= 0x10000 && p < 0x00007FFFFFFFFFFFULL && (p & 7) == 0;
}

// Queues one line for the audit log. Safe on any thread.
void AuditLine(const std::string& text);

std::string Fmt(const char* fmt, ...);

// Image range of the loaded module that contains p.
bool ModuleRange(const void* p, uintptr_t* base, uintptr_t* end);

// Exe detour through the audit's hook list (prologue check, [Audit] hook
// line, the Installed: counts).
bool AuditHookExe(const char* name, size_t rva, const unsigned char* expect, void* detour, void** orig);
// The same checks without installing: "ok", "shared" or a SKIP reason.
std::string AuditCheckExe(const char* name, size_t rva, const unsigned char* expect);

} // namespace audit

#include "AuditPhysX.h"

namespace audit
{

const size_t GW_PHYSICS    = 0x18;    // PhysicsActual* (a ThreadClass)
KLIB_ASSERT_OFFSET(GameWorld_physics, GW_PHYSICS);

inline float Nan()
{
	unsigned int bits = 0x7FC00000u;
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

extern LONGLONG g_qpcStart;   // QPC at Audit_Init

inline double SinceStart(LONGLONG t)
{
	return (double)(t - g_qpcStart) / (double)g_qpcFreq;
}

// Float metrics. Durations are ms; dt is ms; speed and camera are raw.
#define AUDIT_METRICS(X) \
	X(FRAME, "frame") X(PRE, "pre") X(START, "start") X(SCENE, "scene") \
	X(PASSES, "passes") X(ROTHER, "rOther") X(QUEUED, "queued") X(PRESENT, "present") \
	X(FEHEAD, "feHead") X(MAINLOOP, "ml") X(FETAIL, "feTail") X(POST, "post") X(GAP, "gap") \
	X(ML_AIJOIN, "aiJoin") X(ML_PATH, "path") X(ML_MESH, "mesh") X(ML_KILL, "kill") \
	X(ML_PHYSUT, "physUT") X(ML_CHARSUT, "charsUT") X(ML_SAVE, "save") X(ML_RAGDOLL, "ragdoll") \
	X(ML_PHYSKICK, "physKick") X(ML_ZONECAM, "zoneCam") X(ML_PLAYER, "player") X(ML_MISCA, "miscA") \
	X(ML_PARTICLES, "particles") X(ML_MISCB, "miscB") X(ML_ZONEMT, "zoneMT") X(ML_FACTORY, "factory") \
	X(ML_BIRDSJOIN, "birdsJoin") X(ML_CHARS, "chars") X(ML_FACTIONS, "factions") \
	X(ML_BIRDSKICK, "birdsKick") X(ML_GUI, "gui") X(ML_OTHER, "mlOther") \
	X(SUB_MOUSESCAN, "mouseScan") X(SUB_MOUSERAY, "mouseRay") X(SUB_CAMRAY, "camRay") \
	X(SUB_MOUSERAY2, "mouseRay2") X(SUB_CURRAY, "curRay") X(SUB_CURSORT, "curSort") X(SUB_CURCAST, "curCast") \
	X(SUB_ZCVAN, "zcVan") X(SUB_ZCMOD, "zcMod") X(SUB_ZCCONTENT, "zcContent") X(SUB_ZCSECT, "zcSect") \
	X(SD_CU, "cuMs") X(SD_CUPLAYER, "cuPlayerMs") X(SD_CUANIM, "cuAnimMs") X(SD_CP, "cpMs") \
	X(SD_CPPLAYER, "cpPlayerMs") X(SD_CPANIM, "cpAnimMs") X(SD_CHPERIODIC, "chPeriodic") \
	X(SD_CHFOUR, "chFour") X(SD_CHPOST, "chPost") X(SD_CHDEATH, "chDeath") X(SD_FCUPDATE, "fcUpdate") \
	X(SD_FCACTIVE, "fcActive") X(SD_FCPERIODIC, "fcPeriodic") X(SD_FMBUILD, "fmBuildMs") \
	X(SD_CPON, "cpOnMs") X(SD_CPVIS, "cpVisMs") X(SD_CHLIGHT, "chLight") X(SD_LZZONES, "lzZones") X(SD_LZBLD, "lzBld") X(SD_LZCHAR, "lzChar") \
	X(R_CULL, "rCull") X(R_SCENE, "rScene") X(R_SHADOW, "rShadow") X(R_QUEUE, "rQueue") \
	X(R_SUBMIT, "rSubmit") X(R_RSO, "rRso") X(R_SETPASS, "rSetPass") X(R_BIND, "rBind") \
	X(R_D3D, "rD3D") X(R_SYNC, "rSync") X(R_OLDANIM, "rOldAnim") X(R_LIGHTS, "rLights") \
	X(R_PASSOTHER, "rPassOther") \
	X(AI_WAKE, "aiWake") X(AI_RUN, "aiRun") X(AI_WINDOW, "aiWindow") X(AI_ZONE, "aiZone") \
	X(AI_CONTENT, "aiContent") X(AI_FACTIONS, "aiFactions") X(AI_ENV, "aiEnv") X(AI_VIS, "aiVis") \
	X(AI_TU, "aiTU") X(AI_TU4, "aiTU4") X(AI_TUP, "aiTUP") X(AI_FORCED, "aiForced") X(AI_TUMAX, "aiTUmax") \
	X(AI_L1TASK, "aiL1Task") X(AI_L1MOVE, "aiL1Move") X(AI_L1FLUSH, "aiL1Flush") X(AI_L1ANIM, "aiL1Anim") \
	X(AI_OTHER, "aiOther") X(AI_RESID, "aiResid") X(AI_REL, "relMs") X(AI_PLATU, "afPlatoonU") \
	X(BIRDS_WAKE, "birdsWake") X(BIRDS_RUN, "birdsRun") X(BIRDS_WINDOW, "birdsWindow") \
	X(PHYS_WAKE, "physWake") X(PHYS_RUN, "physRun") X(PHYS_LOCK, "physLock") X(PHYS_PRE, "physPre") \
	X(PHYS_SIM, "physSim") X(PHYS_POST, "physPost") \
	X(FX_UPD, "fxUpd") X(FX_UPDOFF, "fxUpdOff") X(FX_UPDMAIN, "fxUpdMain") X(FX_CENSUS, "fxCensus") \
	X(OM_OAFIRE, "rOaFire") X(OM_OAWAIT, "rOaWait") X(OM_OABUILD, "rOaBuild") X(OM_OATAIL, "rOaTail") \
	X(OM_OAWKSUM, "oaWkSum") X(OM_OAWKMAX, "oaWkMax") X(OM_SYNCWAIT, "syncWaitMs") X(OM_WAKEMAX, "ogreWakeMaxUs") \
	X(OM_WAKEMEAN, "ogreWakeMeanUs") X(OM_WORKMAX, "ogreWorkMaxUs") X(OM_WORKSUM, "ogreWorkSumUs") \
	X(OM_CRIT, "ogreCritUs") X(OM_MAINWAKE, "ogreMainWakeUs") X(OM_FXFORK, "fxFork") X(OM_FXPRE, "fxPre") \
	X(OM_FXDRAIN, "fxDrain") X(OM_FXJOBMAX, "fxJobMax") X(OM_FXJOBSUM, "fxJobSum") X(OM_FXWAKE, "fxWake") \
	X(OM_FXTAIL, "fxTail") X(OM_FPLOAD, "fpLoad") X(OM_FPLOADCACHE, "fpLoadCache") X(OM_FPTREE, "fpTreeLoad") \
	X(OM_FPGRASS, "fpGrass") X(OM_FPBUILD, "fpBuild") X(OM_FPSUB, "fpSub") X(OM_FPSUBWIND, "fpSubWind") \
	X(OM_FPPOOLWAIT, "fpPoolWait") X(OM_FPLOCKRO, "fpLockRO") X(OM_FPLOCKDST, "fpLockDst") X(OM_MSTRACE, "msTrace") \
	X(OM_MSINDOORS, "msIndoors") X(OM_MSNAVVALID, "msNavValid") X(OM_MSINDOORSFAST, "msIndoorsFast") \
	X(OM_MSTERRAIN, "msTerrain") X(OM_INDOORSOTHER, "indoorsOther") \
	X(ZONE_SM, "zoneSM") X(UNLOAD_MS, "unloadMs") \
	X(DT, "dt") X(SPEED, "speed") X(CAM_X, "camX") X(CAM_Y, "camY") X(CAM_Z, "camZ") \
	X(CAM_ALT, "camAlt") X(SUMERR, "sumErr")

#define AUDIT_ENUM_M(id, name) M_##id,

enum Metric { AUDIT_METRICS(AUDIT_ENUM_M) NUM_METRICS };

// mainLoop sections, in call order, map onto the ML_* metrics.
const int ML_FIRST = M_ML_AIJOIN;
const int ML_LAST  = M_ML_GUI;          // M_ML_OTHER is derived
const int ML_SECTIONS = ML_LAST - ML_FIRST + 1;

// Main-thread probes nested inside a mainLoop section (not part of its sum).
enum SubTick
{
	SUBT_MOUSESCAN, SUBT_MOUSERAY, SUBT_CAMRAY, SUBT_MOUSERAY2, SUBT_CURSORT,
	SUBT_ZCCONTENT, SUBT_ZCMISC, SUBT_ZCACT, SUBT_ZCSECT,
	SUBT_LIGHTS,
	SUBT_COUNT
};

// SteadyDetail probe ticks on the main thread, per frame (nested: no section's sum).
enum SdTick
{
	SDT_CU, SDT_CUPLAYER, SDT_CUANIM, SDT_CP, SDT_CPPLAYER, SDT_CPANIM,
	SDT_CHPERIODIC, SDT_CHFOUR, SDT_CHPOST, SDT_CHDEATH,
	SDT_FCUPDATE, SDT_FCACTIVE, SDT_FCPERIODIC,
	SDT_CPON, SDT_CPVIS, SDT_CHLIGHT, SDT_LZZONES, SDT_LZBLD, SDT_LZCHAR,
	SDT_COUNT
};

// One SceneManager::_renderPhase02 call (a compositor scene pass, or one CSM
// cascade), with the _cullPhase01 for the same camera and viewport attached.
// Times are exclusive of scene calls nested inside it.
const int MAX_RCALLS = 20;

struct RCall
{
	unsigned char  cam, vp, firstRq, lastRq;   // cam/vp: name-table ids (0 = unknown)
	unsigned char  flags, pad0, pad1, pad2;
	unsigned short draws, instDraws, setPasses, rsos, noRso, attachRso;
	unsigned int   instances;                  // numberOfInstances summed over the draws
	float          ms;                         // _renderPhase02, exclusive
	float          cullMs;                     // matching _cullPhase01
	float          submitMs;                   // _renderVisibleObjects, exclusive
	float          rsoMs;                      // renderSingleObject total
	float          setPassMs;                  // _setPass total
	float          bindMs;                     // D3D11 bindGpuProgramParameters (inside rso)
	float          d3dMs;                      // D3D11RenderSystem::_render (inside rso)
	float          syncMs;                     // main thread in Barrier::sync during the call
};

// Cursor-ray change class against the previous ray (origin, unit direction).
enum CursorClass { CUR_STILL, CUR_SMALL, CUR_LARGE, NUM_CURCLASSES };

// Collision-group histogram of cursor hits: groups 0-31, then "none" (-1 or
// out of range).
const int CUR_GROUPS = 33;

// Shadow rays of a split frame (CursorSplitEvery), in cast order after the
// real call. SR_REAL is the game's own call.
enum SplitRay { SR_REAL, SR_FULL, SR_STATIC, SR_DYNA, SR_DYNB, SR_DYNNOCHAR, NUM_SPLITRAYS };

struct CursorSplit
{
	float          ms[NUM_SPLITRAYS];     // SR_REAL: the probed call (cast + hit list + sort)
	float          realSortMs;            // the real call's qsort (NaN if not probed)
	unsigned short hits[NUM_SPLITRAYS];
	unsigned short chars[NUM_SPLITRAYS];  // of those, in the CursorCharGroups mask
	unsigned char  physAtStart, physAtEnd;
};

struct Config
{
	bool        enabled;
	double      summarySec;
	double      slowMs;
	int         slowMax;
	double      stallMs;
	bool        csvSeconds;
	bool        csvFrames;
	std::string tag;
	bool        boundaryRenderOneFrame;
	bool        ogreSplit;
	bool        draws;
	bool        threadBodies;
	bool        aiVis;
	bool        aiLists;        // per-list AI timing (virtual call-site probes)
	bool        renderDetail;   // scene-call breakdown and draws by class / mesh
	bool        renderDeep;     // + barrier, animation pre-pass, D3D11 bind/render timing
	int         drawTop;        // rows per [AUDIT-DRAWS] / [AUDIT-MESH] table
	bool        particles;      // effect census + ParticleUniverse::_update timing
	int         particleTop;    // rows per [AUDIT-FX] table
	bool        physxDetail;    // physics subphases, queues, query owner/phase sidecars
	bool        saveDetail;     // split SaveManager::saveGame into its stages
	int         saveTop;        // per-zone rows per [AUDIT-SAVE] block
	std::string disableSites;   // lower-case, comma separated
	bool        timerExperiment;
	bool        legacyFps;
	int         cursorSplitEvery;   // shadow rays on every Nth cursor-ray frame (0 = off)
	unsigned    cursorCharGroups;   // collision-group mask counted as character hits
	bool        listeners;          // per-class frame-listener timing
	bool        hullDiag;           // PhysX hull destroy-queue diagnostic
	bool        cpuSample;          // per-thread CPU time: _cpu.csv and [AUDIT-THREADS]
	bool        steadyDetail;       // character, faction and formation cost probes
	bool        offMainDetail;      // the off-main probes (OffMainDetail)
};


// The main thread writes kickSeq just before startRunning. The worker copies
// it at body entry, fills the rest, and publishes doneSeq last. The main
// thread reads the slot only after the join (AI, birds) or once the thread is
// idle again (physics), and only when doneSeq matches the kick it expects.
struct ThreadSlot
{
	volatile LONG kickSeq;
	LONGLONG      kickT;        // main: QPC just before startRunning
	LONG          runSeq;       // worker: kickSeq at body entry
	LONGLONG      bodyIn, bodyOut;
	LONGLONG      zone, content, factions, vis;   // AI-body probe ticks
	LONGLONG      env, forced;                    // weather + smells + 10 s timer; forced periodic updates
	LONGLONG      tu, tu4, tup, tuMax;            // AI list virtual calls (tuMax: longest single call)
	LONGLONG      l1Task, l1Move, l1Flush, l1Anim; // inside list 1 (threadedUpdate 0x5C71A0)
	LONGLONG      lock, pre, post;                // physics-body probe ticks
	LONGLONG      physPhaseTicks[PP_COUNT];       // physics phase wall time
	int           physQueued[PO_COUNT];           // queues at threadJunkPreBT entry
	int           physCalls[PO_COUNT];            // actual operations at probed call sites
	int           physHulls;                      // registered hulls at pre-step entry
	LONGLONG      rel, platU;                     // FactionRelations::update; one platoon's unloaded update (AI)
	int           relCalls, relNodes;             // relations updates and the map entries they walked
	int           hullClass[hullpose::PC_COUNT];  // hull pose submissions by class (physics)
	// CharBody::update time by the task class running it (task vtable).
	static const int MAX_TASKS = 24;
	const void*   taskVt[MAX_TASKS];
	LONGLONG      taskTicks[MAX_TASKS];
	int           taskCalls[MAX_TASKS];
	int           ntask;
	int           visCalls;
	int           l1, l4, lp;
	volatile LONG doneSeq;      // worker, written last
	LONG          collected;    // main: last doneSeq consumed
};

struct CurFrame
{
	bool     open;
	unsigned seq;
	unsigned flags;

	LONGLONG sub[SUBT_COUNT];
	int      ncalls;
	RCall    calls[MAX_RCALLS];
	int      instDraws, instances, setPasses, rsos, drawsOut, drawsNoRso, sceneCalls;
	int      syncs, oldAnims, binds;
	LONGLONG syncTicks, oldAnimTicks;          // QPC
	unsigned long long bindTsc, d3dTsc;         // TSC
	float    aiTU, aiTU4, aiTUP, aiTUmax;
	float    aiEnv, aiForced, aiL1Task, aiL1Move, aiL1Flush, aiL1Anim;
	float    physLock, physPre, physSim, physPost;

	LONGLONG R0, R1, T0, T1, Q0, Q1, E0, E1, M0, M1;
	bool     hR1, hT0, hT1, hQ0, hQ1, hE0, hE1, hM0, hM1;
	LONGLONG sgTop, sgNested, cmTicks, swTicks;
	int      cmDepth;

	LONGLONG ml[ML_SECTIONS];
	int      probeDepth;
	int      mouseScanDepth;                       // scopes indoor rays to cursor work
	LONGLONG meshT0;
	LONGLONG unloadTicks;
	float    zoneSMms;
	int      draws, shadowDraws, unloads;

	float    cam[3];
	bool     camValid;
	float    dtMs, speed;

	LONGLONG aiKickT, aiJoinT0, aiJoinT1, birdsJoinT0;
	float    aiWake, aiRun, aiWindow, aiZone, aiContent, aiFactions, aiVis, aiOther, aiResid;
	float    relMs, afPlatoonU;
	float    birdsWake, birdsRun, birdsWindow, physWake, physRun;
	int      aiL1, aiL4, aiLP, visCalls;

	int      chars, dead, full, setB, zoneState;
	LONGLONG sd[SDT_COUNT];                       // SteadyDetail probe ticks
	int      cuN, cuPlayerN, cpN, cpPlayerN, relCalls, relNodes;
	int      cpOnN, cpVisN, chPeriodicN;          // paused calls on screen / in visible-update mode; periodic updates
	int      chLightN, lzCalls, lzZoneN, lzBldN, lzLightN, lzCharN, lzCharLightN;
	uintptr_t lzOut, lzList, lzCharList;          // the light-level row in flight: its out list
	int      animParent;                          // the character update running the animation update

	bool     fxCensused;                          // the effect census ran this frame
	int      fx, fxSys, fxVis, fxStop, fxParts, fxNew, fxDel;
	unsigned long long fxCensusTsc;

	// Cursor ray (the `mouseRay` probe): per-frame sums over its calls.
	int      curCalls, curClass[NUM_CURCLASSES], curMoved, curBtn, curEdge, curMod, curPhys;
	int      curHits, curChar, ray2Calls;
	unsigned curInput;                            // CursorInputBit, OR-ed
	unsigned short curGroups[CUR_GROUPS];
	bool     curSplit;                            // shadow rays were cast this frame
	CursorSplit split;
	PhysRunSample phys;
	int      nphysq;
	PhysQuerySample physq[MAX_PHYS_QUERIES];
};

// Cursor-ray state across calls (main thread; see CursorEnter / CursorExit).
struct CursorState
{
	bool      layoutOk;       // traceAll's call sequence matched TRACE_CALLSEQ_BYTES
	bool      splitOn;        // CursorSplitEvery > 0, layout ok, not switched off since
	uintptr_t gw;             // GameWorld
	uintptr_t key;            // InputHandler `key`
	uintptr_t prevMLeft, prevMRight;
	HMODULE   physxCore;      // PhysXCore64.dll, looked up once per second until loaded
	uintptr_t ownerNpScene;   // NpScene whose core lock address was validated
	uintptr_t ownerCoreScene; // core Scene whose lock address was validated
	uintptr_t coreLockAddr;   // cached read-only RTL_CRITICAL_SECTION
	int       lockUndecided;  // samples that could not yet confirm the layout
	uintptr_t lockRejectedAt; // the address that failed the layout check
	volatile bool lockLayoutRejected; // sticky: owner sampling stays off; main thread logs it
	bool      lockRejectLogged;
	const char* splitOffWhy;  // set when a split frame found the scene API wrong
	bool      splitOffLogged;

	// The call in flight (mouseRay entry to exit).
	bool      inCall;
	bool      rayOk;          // origin / direction were readable
	bool      split;          // cast the shadow rays after this call
	uintptr_t result;         // lektor<PhysHitItem>*
	float     o[3], d[3];     // origin, unit direction (normalised as traceAll does)
	unsigned  group;          // the group mask the game passed (0xFFFFDE7F)
	unsigned  physStart;
	LONGLONG  sortAtEnter;    // g_cur.sub[SUBT_CURSORT] at entry
	int       queryIndex;      // g_cur.physq slot for the query in flight
	int       queryTag;

	// The previous ray and mouse position.
	bool      haveLast, havePos;
	float     lastO[3], lastD[3], lastPos[2];
	unsigned long long rays;  // cursor rays counted for the split cadence
};

extern Config         g_cfg;
extern CurFrame       g_cur;
extern CursorState    g_cursor;
extern ThreadSlot     g_phys;
extern DWORD          g_mainThreadId;
extern volatile DWORD g_aiThreadId;
extern volatile DWORD g_physThreadId;
extern volatile LONG  g_physPhase;
extern volatile LONG  g_physActiveSeq;

unsigned PhysicsRunning();   // 1 while the physics thread is running

// PhysX hull destroy-queue diagnostic (AuditHulls.cpp, hulls_*.cpp). Install
// returns the banner status: "on", "off" (a hook failed; all pass through) or
// "refused".
// A physics-thread fault inside the delete batch is written to
// <auditDir><runName>_hullcrash.txt.
const char* Hulls_Install(uintptr_t exeBase, uintptr_t exeEnd, uintptr_t gameWorld, bool physUTHooked,
                          const std::string& auditDir, const std::string& runName);
void Hulls_BeforeUpdateUT(void* physics);   // main thread, before PhysicsActual::updateUT
void Hulls_NoteZoneUnload(int x, int y);    // main thread, after the game unloaded a zone
void Hulls_OnFrameStarted();                // main thread, once per frame: prints

} // namespace audit


// Per-class frame-listener timer (AuditListeners.cpp). Main thread only.
bool Listeners_Init(bool enabled);   // false when off or Root::getSingletonPtr is missing
void Listeners_OnFrameStarted();     // from the exe's frameStarted, once per frame
