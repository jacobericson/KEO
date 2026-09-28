// bindings.h - Game address helper, build gate and function bindings.
// Included through game.h.

#ifndef KENSHI_ZONE_OPT_BINDINGS_H
#define KENSHI_ZONE_OPT_BINDINGS_H

#include "game/klib_bindings.h"
#include "base/core.h"

// =========================================================================
// Address helper
// =========================================================================

extern uintptr_t gameBase;
extern void* g_cachedZoneMgr;

inline void* GameAddr(size_t rva)
{
	return (void*)KlibAddress(gameBase, rva);
}


// =========================================================================
// Build gate table — defined in plugin/hook_manifest.cpp, from plugin/hook_manifest_rows.inc
// =========================================================================

extern const HookPrologue g_hookPrologues[];
extern const int          g_hookPrologueCount;

// Verify the row belonging to one RVA. Used by the lazy install sites, which
// run long after startPlugin's full pass. Returns false when no row exists.
bool VerifyPrologueByRva(uintptr_t rva);


// =========================================================================
// Function pointer typedefs (called, not hooked)
// =========================================================================

// 4 arguments, and the last is a FLOAT in xmm3, not an int: the implementation
// (0xA0D6A0) is
//   bool __fastcall(void* zoneEntry, __int64 unused, int timerIndex, float keepAliveSeconds)
// The 3-argument typedef left xmm3 undefined at every call. The game's own
// state-2 path zeroes it — processState2 does `xorps xmm6, xmm6` then
// `movaps xmm3, xmm6` before the call at 0xA0D928 — so 0.0f is the value to
// pass for "no keep-alive".
typedef bool  (*loadSingleZone_t)(void* zoneEntry, __int64 unused, int timerIndex,
                                  float keepAliveSeconds);
// Reused by the nest-validation guard (src/fixes/world/nest_validation.cpp) for its
// own original-function pointer to the same signature.
typedef void  (*finalizeZoneResources_t)(void* sectionEntry);
typedef int   (*flushPendingWork_t)();
typedef void  (*registerZoneSections_t)(void* sectionMgr, void* zoneEntry);
typedef void  (__fastcall *setQueuesAreClear_t)(void* physics, bool on);
// ZoneManager::addToTrackingSet(set, outPair, &value, &pValue). `outPair`
// takes the node pointer and, at +8, a byte that is 1 when the value was
// inserted and 0 when the set already held it. Both pointer arguments name
// the same ZoneMap*: one is hashed, the other is what the node stores.
typedef void* (__fastcall *addToTrackingSet_t)(void* set, void* outPair,
                                               void** key, void*** value);


// Handle resolution for selected character iteration
typedef void* (*resolveHandle_t)(void* table, void* handle);
extern void*           g_handleTable;

// NavMesh queue lock functions (acquire/release input queue lock at navMeshGen+152)
typedef void* (*pathBuilderInit_t)(void* outBuffer);
typedef void  (*pathBuilderFinalize_t)(void* lockAddr, void* initResult);
typedef void  (*readerUnlock_t)(void* lockAddr);

// NavMesh queue lock initializer (despite the name, this IS the CS init function —
// called 5 times from NavMeshGen_struct_ctor at +72/+104/+152/+200/+272 to initialize
// the CSes/locks embedded in the NavMeshGenerator struct). Needed for NMG cloning.
typedef void (*queueLockInit_t)(void* lockAddr);

// NavMesh active cache: called functions (zone optimization -- all builds)
typedef void (*processJobAlt_t)(void* thisNavMeshGen, void* job);
// 4 arguments, from the decompile of the implementation at 0x3CB700:
//   __int64 __fastcall(void** this, char* job, __int64 a3, double a4)
// a3 is never read in its body and a4 is overwritten before use inside
// stitch_buildCollision (0x3C5C10), so the two-argument typedef was harmless in
// practice — but it left r8 and xmm3 undefined at every call.
typedef __int64 (*buildCollision_t)(void* thisNavMeshGen, void* job, __int64 unused,
                                    double unusedF);
typedef void (*partialFixup_t)(void* thisNavMeshGen, void* job);
typedef void (*enqueueToProcQueue_t)(void* queueAddr, void* job);
typedef void* (*navMeshCtor_t)(void* mem);
typedef void* (*settingsCtor_t)(void* mem);  // hkaiNavMeshGenerationSettings constructor
// SimplificationSettings::copy (0x3DA000). NOT a memcpy: it copies scalars, calls
// ExtraVertexSettings::copy for the +88 sub-object and hkStringPtr::copy for the
// +152 string, which allocates. Needed to duplicate an override entry's +80.
typedef void  (*simplSettingsCopy_t)(void* dst, void* src);
// hkaiNavMeshGenerationSettings dtor body (0xDD9BC0). Destroys the settings in
// place without freeing the object: it releases the +240 carvers and +256
// painters element by element, destroys +336 and +512, runs the +144 sub-struct
// destructor (which frees +160 and +176), and frees the +288 and +520 buffers
// only when they do NOT carry DONT_DEALLOCATE.
typedef void  (*settingsDtorBody_t)(void* settings);
typedef void* (*gameNew_t)(size_t size);
typedef void  (*gameDelete_t)(void* ptr);
typedef void* (*gameNewArr_t)(size_t size);
typedef void  (*gameDelArr_t)(void* ptr);


// Havok thread init: called functions (for worker thread TLS initialization)
// Step 3 (register) is a virtual call resolved at runtime, not stored as a global fn ptr.
typedef void  (*havokContextInit_t)(void* ctx128);
typedef void* (*havokGetManager_t)(int param);
typedef void  (*havokPostRegInit_t)(void* buf8, void* ctx128);
typedef void  (*havokCleanup_t)(void* buf8);
typedef void  (*havokCtxCleanup_t)(void* ctx128);


// Island routing: lektor<ZoneMap*> growth (thunk 0x16630 -> 0x37E3A0)
typedef void (*lektorReserve_t)(void* lektor, unsigned int newCapacity);

// Path request queue enqueue
typedef void (*enqueuePathReq_t)(void* queueBase, void** itemPtr);


// Havok::contentStreamCallee_0x8869 (path-result extraction loop). 4-arg
// fastcall. Our hook SEH-wraps it and on AV rolls back a4[2] so the game sees
// "no new edges produced" -> treats as no path, retries cleanly.
typedef unsigned __int64 (*contentStreamCallee0x8869_t)(void* manager,
                                                         unsigned int faceKey,
                                                         void* searchOutput,
                                                         unsigned int* resultBuf);
// hkaiStreamingCollection::addInstance. 5-arg fastcall; mutates m_instances.
// Hooked so we can timestamp the last mutation and gate our cache injection.
typedef void (*addInstance_t)(void* collection, __int64 sectionData,
                              __int64 param3, __int64 param4, int param5);



// =========================================================================
// Hook typedefs + original function pointers
// =========================================================================

typedef void (*showLoadingMessage_t)(void* thisPtr, bool on);
typedef bool (*isContentPending_t)(void* manager, void* zonePos);
typedef void (*updateCameraZone_t)(void* zoneMgr, void* cameraPos);
typedef void (*addOrderSelected_t)(void* thisPI, void* destIndoors, int task,
                                    void* subject, bool shift, bool addDontClear,
                                    const float* location);
typedef char (*dispatchJob_t)(void* thisNMG);


// 4 register arguments, no stack argument: the wrapper writes its own 5th
// (timeLowPart = 0) into the shadow space before tail-calling realGenerate
// (disassembly of 0xE0AFF0). The old 5-parameter declaration made the hook read
// uninitialized shadow space.
typedef void (*nmResultPopulate_t)(void* navData, void* localData, void* result, int param);

// Island routing hooks
typedef bool  (*isInIsland_t)(void* zoneA, void* zoneB);
typedef void* (*getIsland_t)(void* zoneMgr, void* zone, void* lektorOut);

// Player-cancel hooks (island_cancel_hooks.cpp detours, hook_manifest.cpp installs). The player's own ways to end a move order, so the island tracker
// can tell a player cancel from an order the engine deleted. All three are
// KenshiLib-covered (0.5.0 and 0.5.1 export them; klib_bindings.cpp binds
// them); the Steam RVAs below are the brdump.py Steam column, confirmed
// against the binary. All run on the main thread's input/GUI
// paths: stopCharactersMovement's only caller is GameWorld::processKeys
// (0x7875C0, the stop key); addJobSelectedCharacters is called from
// PlayerInterface::newPlayerTaskSelectedCharacters (0x7F9AB0) and
// OrdersPanel::medicButton (0x7FA080); addTaskNearestSelectedCharacter
// through its thunk 0x311A6 from PlayerInterface::pickupItem (0x7FA800),
// characterSelected (0x7FA870), itemSelected (0x7FB8A0) and
// buildingSelected (0x7FBB40), seven call sites.
//
// PlayerInterface::stopCharactersMovement() (0x7F58D0, 0x167 bytes):
//   void __fastcall(PlayerInterface* this). For every selected character it
//   calls CharMovement::halt (vt+0x98), OrdersReceiver::clearOrders on
//   AI+0x20 (0x506A10) and CharBody::_endAction (vt+0x60).
// PlayerInterface::addJobSelectedCharacters(TaskType task, RootObject* subject,
//   bool shift, bool add, const Ogre::Vector3& location) (0x7F4EF0, 0x1E4
//   bytes): rcx this, edx task, r8 subject, r9b shift, [rsp+0x28] add,
//   [rsp+0x30] location (a pointer: the body loads it as one qword and hands it
//   to Character::addJob 0x5C8310; the typed IDB, config 56, declares it
//   `const Ogre::Vector3*`).
//   Calls Character::addJob(task, subject, shift, add, location) for every
//   selected player-owned character; Character::addJob (0x5C8310) runs
//   OrdersReceiver::clearOrders + clearJobs on AI+0x20 only when that `add`
//   argument (KenshiLib's Character::addJob calls it addDontClear) is false.
// PlayerInterface::addTaskNearestSelectedCharacter(Building* dest, TaskType t,
//   RootObject* subject, bool shift, const Ogre::Vector3& location,
//   bool noAnimals) (0x7FA2D0, 0x364 bytes): rcx this, rdx dest, r8d task,
//   r9 subject, [rsp+0x28] shift, [rsp+0x30] location (pointer),
//   [rsp+0x38] noAnimals. Returns at once when selectedCharactersUnconcious
//   (0x7F63B0) says so; for task 26 with a global flag set it forwards to
//   newPlayerTaskSelectedCharacters (0x7F9AB0) instead; otherwise it picks
//   the one eligible selected character nearest the subject and, unless
//   Character::checkPlayerOrderForProblems (0x5D0EB0) objects, calls
//   Character::addOrder(dest, task, subject, shift, clear = 1, location)
//   (0x5D1640) on that character only.
const size_t RVA_STOP_CHARACTERS_MOVEMENT = 0x7F58D0;
const size_t RVA_ADD_JOB_SELECTED         = 0x7F4EF0;
const size_t RVA_ADD_TASK_NEAREST         = 0x7FA2D0;
typedef void (*stopCharactersMovement_t)(void* thisPI);
typedef void (*addJobSelected_t)(void* thisPI, int task, void* subject, bool shift,
                                 bool add, const float* location);
typedef void (*addTaskNearest_t)(void* thisPI, void* dest, int task, void* subject,
                                 bool shift, const float* location, bool noAnimals);

typedef void (*realGenerate_t)(void* workBuffer, void* localData, void* hkaiNavMesh, int param, int timeLowPart);

typedef char (*csFindPath_t)(void* manager, unsigned int startFaceKey, void* startPos,
                              void* destPos, float radius, char param5, void* resultBuf);
typedef char (*csCheckFaceConn_t)(void* manager, unsigned int startFace, unsigned int destFace);
typedef void (*findPathFull_t)(void* streamingCollection, void* searchState, void* findPathOutput);
typedef void (*requestPath_t)(void* havokChar, float* destination, int priority);
typedef void (*pathReqSubmit_t)(void* sectionMgr, void* requestObj, bool highPriority);
typedef char (*csFindPathFallback_t)(void* manager, unsigned int startFaceKey, void* startPos,
                                      unsigned int destFaceKey, void* destPos, float radius,
                                      float param6, char param7, void* resultBuf);




namespace game {

// The game functions InitGameBindings resolves. Written once, on the main
// thread in startPlugin before hooks that use these bindings install;
// read-only afterwards, on any thread.
struct GameFunctions
{
	// Zone loading
	loadSingleZone_t         fn_loadSingleZone;
	flushPendingWork_t       fn_flushPendingWork;
	registerZoneSections_t   fn_registerZoneSections;
	setQueuesAreClear_t      fn_setQueuesAreClear;
	addToTrackingSet_t       fn_addToTrackingSet;

	// Handle resolution for selected character iteration
	resolveHandle_t          fn_resolveHandle;

	// NavMesh queue lock functions (acquire/release input queue lock at navMeshGen+152)
	pathBuilderInit_t        fn_pathBuilderInit;
	pathBuilderFinalize_t    fn_pathBuilderFinalize;
	readerUnlock_t           fn_readerUnlock;

	// NavMesh queue lock initializer
	queueLockInit_t          fn_queueLockInit;

	// NavMesh active cache: called functions (zone optimization -- all builds)
	navMeshCtor_t            fn_navMeshCtor;
	settingsCtor_t           fn_settingsCtor;
	simplSettingsCopy_t      fn_simplSettingsCopy;
	settingsDtorBody_t       fn_settingsDtorBody;
	processJobAlt_t          fn_processJobAlt;
	buildCollision_t         fn_buildCollision;
	partialFixup_t           fn_partialFixup;
	enqueueToProcQueue_t     fn_enqueueToProcQueue;
	gameNew_t                fn_gameNew;
	gameDelete_t             fn_gameDelete;
	gameNewArr_t             fn_gameNewArr;
	gameDelArr_t             fn_gameDelArr;

	// Havok thread init: called functions (for worker thread TLS initialization)
	havokContextInit_t       fn_havokContextInit;
	havokGetManager_t        fn_havokGetManager;
	havokPostRegInit_t       fn_havokPostRegInit;
	havokCleanup_t           fn_havokCleanup;
	havokCtxCleanup_t        fn_havokCtxCleanup;

	// Island routing: lektor<ZoneMap*> growth (thunk 0x16630 -> 0x37E3A0)
	lektorReserve_t          fn_lektorReserve;

	// Path request queue enqueue
	enqueuePathReq_t         fn_enqueuePathReq;
};

// The main-thread installs write these trampolines through HookInstallRow's
// out-argument. The first NavMesh bg dispatch's lazy install writes
// orig_nmResultPopulate. KenshiLib fills each slot before enabling its hook;
// a refused or unwanted install leaves it NULL. orig_realGenerate has no
// install and stays NULL. Detours read their own slot on whichever thread
// the game calls them.
struct HookOriginals
{
	// Path-result extraction and navmesh instance mutation
	contentStreamCallee0x8869_t  orig_contentStreamCallee0x8869;
	addInstance_t                orig_addInstance;

	// Zone, movement order and dispatch hooks
	showLoadingMessage_t         orig_showLoadingMessage;
	isContentPending_t           orig_isContentPending;
	updateCameraZone_t           orig_updateCameraZone;
	addOrderSelected_t           orig_addOrderSelected;
	dispatchJob_t                orig_dispatchJob;

	// Generation input diagnostics
	nmResultPopulate_t           orig_nmResultPopulate;

	// Island routing hooks
	isInIsland_t                 orig_isInIsland;
	getIsland_t                  orig_getIsland;

	// Player-cancel hooks
	stopCharactersMovement_t     orig_stopCharactersMovement;
	addJobSelected_t             orig_addJobSelected;
	addTaskNearest_t             orig_addTaskNearest;

	// No install; stays NULL
	realGenerate_t               orig_realGenerate;

	// Pathfinding hooks
	csFindPath_t                 orig_csFindPath;
	csCheckFaceConn_t            orig_csCheckFaceConn;
	findPathFull_t               orig_findPathFull;
	requestPath_t                orig_requestPath;
	pathReqSubmit_t              orig_pathReqSubmit;
	csFindPathFallback_t         orig_csFindPathFallback;
};

extern GameFunctions g_gameFn;
extern HookOriginals g_hookOrig;

} // namespace game

#endif // KENSHI_ZONE_OPT_BINDINGS_H
