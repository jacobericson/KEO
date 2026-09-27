#include "fixes/world/destroy_list_defer.h"
#include "game/game.h"
#include <cstdio>     // _snprintf_s (VerifyPrologueByRva's allocation-free failure path)


// =========================================================================
// Game base + cached zone manager
// =========================================================================

uintptr_t gameBase = 0;
void* g_cachedZoneMgr = NULL;


// =========================================================================
// Function pointers (called, not hooked)
// =========================================================================

loadSingleZone_t        fn_loadSingleZone        = NULL;
flushPendingWork_t      fn_flushPendingWork       = NULL;
registerZoneSections_t  fn_registerZoneSections   = NULL;
setQueuesAreClear_t     fn_setQueuesAreClear      = NULL;
addToTrackingSet_t      fn_addToTrackingSet       = NULL;

resolveHandle_t fn_resolveHandle = NULL;
void*           g_handleTable    = NULL;

pathBuilderInit_t      fn_pathBuilderInit     = NULL;
pathBuilderFinalize_t  fn_pathBuilderFinalize = NULL;
readerUnlock_t         fn_readerUnlock        = NULL;
queueLockInit_t        fn_queueLockInit       = NULL;

navMeshCtor_t        fn_navMeshCtor         = NULL;
settingsCtor_t       fn_settingsCtor        = NULL;
simplSettingsCopy_t  fn_simplSettingsCopy   = NULL;
settingsDtorBody_t   fn_settingsDtorBody    = NULL;
processJobAlt_t      fn_processJobAlt       = NULL;
buildCollision_t     fn_buildCollision       = NULL;
partialFixup_t       fn_partialFixup         = NULL;
enqueueToProcQueue_t fn_enqueueToProcQueue   = NULL;
gameNew_t            fn_gameNew              = NULL;
gameDelete_t         fn_gameDelete           = NULL;
gameNewArr_t         fn_gameNewArr           = NULL;
gameDelArr_t         fn_gameDelArr           = NULL;

havokContextInit_t   fn_havokContextInit     = NULL;
havokGetManager_t    fn_havokGetManager      = NULL;
havokPostRegInit_t   fn_havokPostRegInit     = NULL;
havokCleanup_t       fn_havokCleanup         = NULL;
havokCtxCleanup_t    fn_havokCtxCleanup      = NULL;

lektorReserve_t      fn_lektorReserve        = NULL;
enqueuePathReq_t     fn_enqueuePathReq       = NULL;

// Preload pipeline zone-ready query (called, not hooked; game.h).
isZoneReady_t        fn_isZoneReady          = NULL;

// The save-load reset's own per-zone unload (called, not hooked; game.h).
unloadZoneFromReset_t fn_unloadZoneFromReset = NULL;

// First-time prediction (called, not hooked; game.h).
sfsGetSingleton_t    fn_sfsGetSingleton      = NULL;
sfsFileExists_t      fn_sfsFileExists        = NULL;

// Stand-in neighbour seeds (called, not hooked; game.h).
navMeshGetSector_t    fn_navMeshGetSector    = NULL;
navMeshGetFilename_t  fn_navMeshGetFilename  = NULL;
navMeshLoadZone_t     fn_navMeshLoadZone     = NULL;
navMeshDeleteSector_t fn_navMeshDeleteSector = NULL;
nmgLockZone_t         fn_nmgLockZone         = NULL;
nmgUnlockZone_t       fn_nmgUnlockZone       = NULL;
zmGetZoneMap_t        fn_zmGetZoneMap        = NULL;
sortedArrayGrow_t     fn_sortedArrayGrow     = NULL;
gameStringRelease_t   fn_gameStringRelease   = NULL;

// `mov [rsp+10h],rsi; push rdi; sub rsp,20h; mov eax,[rdx+8]; mov esi,r8d`
extern const unsigned char g_sortedArrayGrowBytes[16] =
	{ 0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x8B,0x42,0x08,0x41,0x8B,0xF0 };
// `push rbx; sub rsp,20h; cmp qword [rcx+18h],10h; mov rbx,rcx; jb +8`
extern const unsigned char g_gameStringReleaseBytes[16] =
	{ 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x83,0x79,0x18,0x10,0x48,0x8B,0xD9,0x72,0x08 };


// =========================================================================
// Hook original function pointers
// =========================================================================

showLoadingMessage_t    orig_showLoadingMessage    = NULL;
isContentPending_t      orig_isContentPending      = NULL;
updateCameraZone_t      orig_updateCameraZone      = NULL;
addOrderSelected_t      orig_addOrderSelected      = NULL;
dispatchJob_t           orig_dispatchJob           = NULL;
nmResultPopulate_t      orig_nmResultPopulate      = NULL;
realGenerate_t          orig_realGenerate          = NULL;
isInIsland_t            orig_isInIsland            = NULL;
getIsland_t             orig_getIsland             = NULL;
resetUnloadZones_t      orig_resetUnloadZones      = NULL;   // save-load reset
stopCharactersMovement_t orig_stopCharactersMovement = NULL; // player cancel hooks
addJobSelected_t        orig_addJobSelected        = NULL;
addTaskNearest_t        orig_addTaskNearest        = NULL;
csFindPath_t            orig_csFindPath            = NULL;
csCheckFaceConn_t       orig_csCheckFaceConn       = NULL;
findPathFull_t          orig_findPathFull          = NULL;
requestPath_t           orig_requestPath           = NULL;
pathReqSubmit_t         orig_pathReqSubmit         = NULL;
csFindPathFallback_t    orig_csFindPathFallback    = NULL;
contentStreamCallee0x8869_t orig_contentStreamCallee0x8869 = NULL;
addInstance_t               orig_addInstance               = NULL;

// Path-worker-pool instrumentation hooks + isPriorityPath (game.h's
// path-worker-pool block). Storage only; the hooks' orig_ pointers are filled
// by KenshiLib::AddHook in hook_manifest.cpp, not here.
contentStream_t      orig_contentStream      = NULL;
dequeueWork_t        orig_dequeueWork        = NULL;
enqueueThreadSafe_t  orig_enqueueThreadSafe  = NULL;
gatesUpdateCodes_t   orig_gatesUpdateCodes   = NULL;
isPriorityPath_t     fn_isPriorityPath       = NULL;


// =========================================================================
// InitGameBindings — resolve all function pointers from gameBase
// =========================================================================

void InitGameBindings(uintptr_t base)
{
	gameBase = base;

	// Set up HavokTlsAlloc (core.h) without core depending on game.h
	SetHavokTlsParams(base, RVA_HAVOK_TLS_INDEX);

	// Same arrangement for the destroyListOE probe: core gets the resolved
	// GameWorld address and keeps no game knowledge of its own.
	SetDestroyListBase((uintptr_t)GameAddr(RVA_GLOBAL_GAMEWORLD));

	// Zone loading
	fn_loadSingleZone        = (loadSingleZone_t)       GameAddr(RVA_LOAD_SINGLE_ZONE);
	fn_flushPendingWork      = (flushPendingWork_t)      GameAddr(RVA_FLUSH_PENDING_WORK);
	fn_registerZoneSections  = (registerZoneSections_t)  GameAddr(RVA_REGISTER_ZONE_SECTIONS);
	fn_setQueuesAreClear     = (setQueuesAreClear_t)     GameAddr(RVA_SET_QUEUES_CLEAR);
	fn_addToTrackingSet      = (addToTrackingSet_t)      GameAddr(RVA_ADD_TO_TRACKING_SET);

	// Handle resolution
	fn_resolveHandle         = (resolveHandle_t)         GameAddr(RVA_RESOLVE_HANDLE);
	g_handleTable            = (void*)(base + RVA_HANDLE_TABLE);

	// NavMesh queue locks
	fn_pathBuilderInit       = (pathBuilderInit_t)      GameAddr(RVA_PATH_BUILDER_INIT);
	fn_pathBuilderFinalize   = (pathBuilderFinalize_t)  GameAddr(RVA_PATH_BUILDER_FINALIZE);
	fn_readerUnlock          = (readerUnlock_t)         GameAddr(RVA_READER_UNLOCK);
	fn_queueLockInit         = (queueLockInit_t)        GameAddr(RVA_QUEUE_LOCK_INIT);

	// NavMesh active cache
	fn_navMeshCtor           = (navMeshCtor_t)           GameAddr(RVA_NAVMESH_CTOR);
	fn_settingsCtor          = (settingsCtor_t)          GameAddr(RVA_SETTINGS_CTOR);
	fn_simplSettingsCopy     = (simplSettingsCopy_t)     GameAddr(RVA_SIMPL_SETTINGS_COPY);
	fn_settingsDtorBody      = (settingsDtorBody_t)      GameAddr(RVA_SETTINGS_DTOR_BODY);
	fn_processJobAlt         = (processJobAlt_t)        GameAddr(RVA_PROCESS_JOB_ALT);
	fn_buildCollision        = (buildCollision_t)        GameAddr(RVA_BUILD_COLLISION);
	fn_partialFixup          = (partialFixup_t)          GameAddr(RVA_PARTIAL_FIXUP);
	fn_enqueueToProcQueue    = (enqueueToProcQueue_t)    GameAddr(RVA_ENQUEUE_TO_PROC_QUEUE);
	fn_gameNew               = (gameNew_t)               GameAddr(RVA_GAME_NEW);
	fn_gameDelete            = (gameDelete_t)            GameAddr(RVA_GAME_DELETE);
	fn_gameNewArr            = (gameNewArr_t)            GameAddr(RVA_GAME_NEW_ARR);
	fn_gameDelArr            = (gameDelArr_t)            GameAddr(RVA_GAME_DEL_ARR);

	// Havok thread init
	fn_havokContextInit      = (havokContextInit_t)      GameAddr(RVA_HAVOK_CONTEXT_INIT);
	fn_havokGetManager       = (havokGetManager_t)       GameAddr(RVA_HAVOK_GET_MANAGER);
	fn_havokPostRegInit      = (havokPostRegInit_t)      GameAddr(RVA_HAVOK_POST_REG_INIT);
	fn_havokCleanup          = (havokCleanup_t)          GameAddr(RVA_HAVOK_CLEANUP);
	fn_havokCtxCleanup       = (havokCtxCleanup_t)       GameAddr(RVA_HAVOK_CTX_CLEANUP);

	// Island routing
	fn_lektorReserve         = (lektorReserve_t)         GameAddr(RVA_LEKTOR_RESERVE);

	fn_enqueuePathReq        = (enqueuePathReq_t)        GameAddr(RVA_ENQUEUE_PATH_REQ);


	fn_isPriorityPath        = (isPriorityPath_t)        GameAddr(RVA_IS_PRIORITY_PATH);

	// Readiness classification (readiness_hook.cpp).
	fn_lookupSection         = (lookupSection_t)         GameAddr(RVA_LOOKUP_SECTION);
	fn_boostUnlock           = (boostUnlock_t)           GameAddr(RVA_BOOST_UNLOCK);
	fn_boostUnlockShared     = (boostUnlockShared_t)     GameAddr(RVA_BOOST_UNLOCK_SHARED);

	// Preload pipeline zone-ready query.
	fn_isZoneReady           = (isZoneReady_t)           GameAddr(RVA_ZONE_IS_READY);

	// Save-load reset.
	fn_unloadZoneFromReset   = (unloadZoneFromReset_t)   GameAddr(RVA_UNLOAD_ZONE_FROM_RESET);

	// First-time prediction (zone_life.cpp)
	fn_sfsGetSingleton       = (sfsGetSingleton_t)       GameAddr(RVA_SFS_GET_SINGLETON);
	fn_sfsFileExists         = (sfsFileExists_t)         GameAddr(RVA_SFS_FILE_EXISTS);

	// Stand-in neighbour seeds (nm_nbr_seeds.cpp): covered by KenshiLib, so GameAddr
	// returns the registry's cross-checked address.
	fn_navMeshGetSector      = (navMeshGetSector_t)      GameAddr(RVA_NAVMESH_GET_SECTOR);
	// Also the narrow collision-build lock (nm_buildlock.cpp).
	fn_nmgLockZone           = (nmgLockZone_t)           GameAddr(RVA_NMG_LOCK_ZONE);
	fn_nmgUnlockZone         = (nmgUnlockZone_t)         GameAddr(RVA_NMG_UNLOCK_ZONE);
	fn_navMeshGetFilename    = (navMeshGetFilename_t)    GameAddr(RVA_NAVMESH_GET_FILENAME);
	fn_navMeshLoadZone       = (navMeshLoadZone_t)       GameAddr(RVA_NAVMESH_LOAD_ZONE);
	fn_navMeshDeleteSector   = (navMeshDeleteSector_t)   GameAddr(RVA_NAVMESH_DELETE_SECTOR);
	fn_zmGetZoneMap          = (zmGetZoneMap_t)          GameAddr(RVA_ZM_GET_ZONE_MAP);
	// Not in KenshiLib: GameAddr falls back to base + RVA; nm_nbr_seeds.cpp
	// byte-checks both before the stand-in is enabled.
	fn_sortedArrayGrow       = (sortedArrayGrow_t)       GameAddr(RVA_SORTED_ARRAY_GROW);
	fn_gameStringRelease     = (gameStringRelease_t)     GameAddr(RVA_GAME_STRING_RELEASE);
}


// =========================================================================
// Tracking-set membership (Set A / Set B) — see game.h
// =========================================================================

// One boost::unordered_set<ZoneMap*> at `set` (Set A or Set B). The list head
// is *(buckets + 8*bucketCount); each node carries next at +0 and the
// ZoneMap* at +16 (same walk as island_components.cpp's WalkSetB). The walk is bounded
// by the set's own size (+16 slack), and a size outside 1..4*ZONE_GRID_COUNT
// or a bucket count outside 1..2^24 reads as "not a member". Plain C, POD
// only: MSVC 2010 rejects __try in a function holding objects that need
// unwinding. GuardEnter keeps a fault out of the crash recorder.
static bool ZoneSetContains(uintptr_t set, uintptr_t zone)
{
	bool found = false;
	GuardEnter();
	__try
	{
		unsigned long long size = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_size_, OFF_SET_SIZE));
		if (size > 0 && size <= (unsigned long long)ZONE_GRID_COUNT * 4)
		{
			unsigned long long bucketCount = *(unsigned long long*)(KLIB_MEMBER(2, set, ZoneSetTable_bucket_count_, OFF_SET_BUCKET_COUNT));
			uintptr_t buckets = *(uintptr_t*)(KLIB_MEMBER(2, set, ZoneSetTable_buckets_, OFF_SET_BUCKETS));
			if (buckets && bucketCount > 0 && bucketCount < (1ull << 24))
			{
				uintptr_t node = *(uintptr_t*)(buckets + 8 * bucketCount);
				int iter = 0;
				int maxIter = (int)size + 16;
				while (node && iter++ < maxIter)
				{
					if (*(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_value_base_, OFF_SET_NODE_VALUE) == zone)
					{
						found = true;
						break;
					}
					node = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_next_, 0);
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		found = false;
	}
	GuardLeave();
	return found;
}

bool ZoneInSetA(void* zoneMgr, void* zone)
{
	if (!zoneMgr || !zone)
		return false;
	return ZoneSetContains(KLIB_MEMBER(2, zoneMgr, ZoneManager_processingNewActiveZones, OFF_ZM_SET_A),
	                       (uintptr_t)zone);
}

bool ZoneInSetB(void* zoneMgr, void* zone)
{
	if (!zoneMgr || !zone)
		return false;
	return ZoneSetContains(KLIB_MEMBER(2, zoneMgr, ZoneManager_activeZones, OFF_ZM_SET_B),
	                       (uintptr_t)zone);
}


// =========================================================================
// Build gate: hook and patch-site prologues
// =========================================================================

bool VerifyPrologueByRva(uintptr_t rva)
{
	for (int i = 0; i < g_hookPrologueCount; ++i)
	{
		if (g_hookPrologues[i].rva == rva)
			return VerifyPrologue(rva, g_hookPrologues[i].bytes, g_hookPrologues[i].name);
	}

	// No row: the standing rule in plugin/hook_manifest_rows.inc was not
	// followed. Refuse the install rather than write a jump into an unverified
	// address. This can run on the NavMesh bg thread (InstallNavMeshLazyHooks
	// calls VerifyPrologueByRva from inside hook_dispatchJob on its first call),
	// so the line is built in a fixed buffer and goes through LogMsgDeferrable,
	// like the lazy hook install lines in nm_lazy_hooks.cpp: LogMsg on the
	// main thread, queued for the main thread anywhere else. Message content
	// unchanged.
	char buf[128];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"Build gate: no prologue row for RVA 0x%llx \xE2\x80\x94 install refused",
		(unsigned long long)rva);
	LogMsgDeferrable(buf);
	return false;
}
