// nbr_seed_bindings.h - Neighbour seed addresses, layouts and function bindings.
// Included through game.h.

#ifndef KENSHI_ZONE_OPT_NBR_SEED_BINDINGS_H
#define KENSHI_ZONE_OPT_NBR_SEED_BINDINGS_H

#include "base/core.h"

// ---- Stand-in neighbour seeds (nm_nbr_seeds.cpp) ----
// Verified in the Steam 1.0.65 IDB; every KenshiLib-covered
// address below equals the Steam column of tools/kenshilib/brdump.py over the
// 0.5.1 tables and is bound in klib_bindings.cpp (all are 0.5.0 exports too).
//
// Hooked (installed with the lazy NavMesh hooks):
//   NavMeshGenerator::getSeedPointsFromAdjacentZone(const ZoneMap*, const iVector2&)
//   0x3C96E0 (0x482 bytes; sole caller generateTaskBT 0x3CBE60 through the thunk
//   0x27827, four calls in its type-0 branch with (-1,0) (1,0) (0,-1) (0,1),
//   i.e. W E S N; the return value is discarded there). Returns the number of
//   seeds it appended to settings->m_regionSeedPoints (NMG+0x100, WB+0xA0).
//   The iVector2 is {x, y}: y is the zone grid's second coordinate and the
//   world z axis. Prologue `mov rax,rsp; push rbp/rsi/rdi/r12..r15; lea rbp,
//   [rax-68h]`: position-independent over the 5 bytes a detour takes.
const size_t RVA_NMG_GET_SEED_POINTS_ADJ = 0x3C96E0;
typedef int (*nmgGetSeedPointsAdj_t)(void* nmg, const void* zone, const int* dir);

// Called, never hooked:
//   NavMesh::getSector(const iVector2&, bool create) 0x3AB1F0: map lookup under
//     NavMesh::mutex (+0x1E0, timed lock) that releases it before returning the
//     sector (or NULL with create=false). getSeedPointsFromAdjacentZone itself
//     calls it with create=false and reads the sector's `temp` byte after the
//     release, exactly as the classification here does.
// The stand-in prefetch, called, never hooked:
//   NavMesh::getFilename(const ZoneMap*, FileMode, bool write) 0x3A6230: static,
//     returns std::string by hidden pointer (rcx = the result, constructed by
//     the callee). FileMode BASE = 1 (`cmp r12d, 1` at 0x3A63F8) returns
//     "./data/newland//land/navtiles/tileX.Y.hkt", the shipped tile, whatever
//     the save holds; that branch never touches SaveFileSystem.
//   NavMesh::loadZone(const ZoneMap*, const std::string&) 0x3A7F10: loads a tile
//     file into a new NavMeshSector (+0x08 = the file's first hkaiNavMesh,
//     extra navmeshes into kzSubsections) that is in no world; NULL when the
//     file is missing.
//   NavMesh::deleteMesh(NavMeshSector*) 0x3AC8B0: frees such a sector (its
//     instance is NULL, so no world removal).
//   NavMeshGenerator::lockZone(const iVector2&, bool wait) 0x3C73B0 and
//     unlockZone(const iVector2&) 0x3BF560: both ignore their arguments and
//     take / release the global navmesh build mutex (a boost::shared_mutex at
//     0x212DEB8) exclusively; wait=true is a timed_lock with no deadline. The
//     game's other users on the path thread (NavMesh::update's section adds,
//     createZone, unloadZone, the generator's saves) only try it (wait=false)
//     and retry on a later pass. NavMeshGenerator::stitchUnloadedZone 0x3CB140
//     takes it blocking around exactly the getFilename / loadZone / deleteMesh
//     sequence below, on the NavMesh threads, inside the collision builders.
//   ZoneManager::getZoneMap(int x, int y) 0xA07C10: &worldMap[x][y], NULL
//     outside 0..63.
// Not covered by KenshiLib (explicit addresses, byte-checked before use):
//   SortedArray__grow 0xBA3AA0 (hkArrayUtil::_reserveMore): `void (allocator,
//     hkArrayBase*, int elemSize)`, the growth call getSeedPointsFromAdjacentZone
//     makes before each append (0x3C9A30) when size == capacity.
//   hkContainerHeapAllocator::s_alloc 0x21044D8: the allocator object it passes
//     (its vtable 0x1778F48 forwards to the calling thread's Havok heap).
//   The game's std::string release 0x69160 (thunk 0x146BE, 61k callers):
//     `if (_Myres >= 16) operator delete(_Bx._Ptr)`, then the empty state.
//     VS2010 x64 std::string is 40 bytes (_Bx +0, _Mysize +16, _Myres +24).
const size_t RVA_NAVMESH_GET_SECTOR      = 0x3AB1F0;
const size_t RVA_NAVMESH_GET_FILENAME    = 0x3A6230;
const size_t RVA_NAVMESH_LOAD_ZONE       = 0x3A7F10;
const size_t RVA_NAVMESH_DELETE_SECTOR   = 0x3AC8B0;
const size_t RVA_NMG_LOCK_ZONE           = 0x3C73B0;
const size_t RVA_NMG_UNLOCK_ZONE         = 0x3BF560;
// Return addresses of the four non-blocking lockZone calls, which is how a
// refusal is attributed to the loop that asked. All four pass blocking = 0.
// The first two ask with something behind them (NavMesh::update's exclusive
// change mutex, taken at 0x3AE66B; the generator's own pass), the last two ask
// from NavMesh::update's message drain, which holds nothing.
const size_t RVA_LOCKZONE_RET_UPDATE_ADD = 0x3AE6CC;
const size_t RVA_LOCKZONE_RET_GEN_SAVE   = 0x3C95A9;
const size_t RVA_LOCKZONE_RET_UNLOAD     = 0x3AD3B5;
const size_t RVA_LOCKZONE_RET_CREATE     = 0x3AE00B;
const size_t RVA_ZM_GET_ZONE_MAP         = 0xA07C10;
const size_t RVA_SORTED_ARRAY_GROW       = 0xBA3AA0;
const size_t RVA_HK_CONTAINER_HEAP_ALLOC = 0x21044D8;
const size_t RVA_HK_CONTAINER_HEAP_VTBL  = 0x1778F48;   // *(s_alloc) on this binary
const size_t RVA_GAME_STRING_RELEASE     = 0x69160;
const int    NAVMESH_FILE_BASE           = 1;           // NavMesh::FileMode BASE
const size_t GAME_STRING_SIZE            = 40;          // VS2010 x64 std::string

// NavInstance (KenshiLib NavInstance.h; klib_bindings.cpp asserts both):
const size_t OFF_NAVINST_MESH = 0x08;   // hkaiNavMesh*
const size_t OFF_NAVINST_TEMP = 0x45;   // bool temp: stale-hash tile awaiting regeneration
const size_t OFF_NAVINST_GRAPH      = 0x10;  // hkaiDirectedGraphExplicitCost*
const size_t OFF_NAVINST_GRAPH_INST = 0x18;  // hkaiDirectedGraphInstance*, NULL until createInstance
const size_t OFF_NAVINST_UID        = 0x3C;  // section uid

typedef void* (*navMeshGetSector_t)(void* navmesh, const int* coords, bool create);
typedef void* (*navMeshGetFilename_t)(void* retStr, const void* zone, int mode, bool write);
typedef void* (*navMeshLoadZone_t)(void* navmesh, const void* zone, const void* fileStr);
typedef void  (*navMeshDeleteSector_t)(void* navmesh, void* sector);
typedef bool  (*nmgLockZone_t)(void* nmg, const int* coords, bool wait);
typedef void  (*nmgUnlockZone_t)(void* nmg, const int* coords);
typedef void* (*zmGetZoneMap_t)(void* zoneMgr, int x, int y);
typedef void  (*sortedArrayGrow_t)(void* allocator, void* array, int elemSize);
typedef void  (*gameStringRelease_t)(void* str);

extern navMeshGetSector_t    fn_navMeshGetSector;
extern navMeshGetFilename_t  fn_navMeshGetFilename;
extern navMeshLoadZone_t     fn_navMeshLoadZone;
extern navMeshDeleteSector_t fn_navMeshDeleteSector;
extern nmgLockZone_t         fn_nmgLockZone;
extern nmgUnlockZone_t       fn_nmgUnlockZone;
extern zmGetZoneMap_t        fn_zmGetZoneMap;
extern sortedArrayGrow_t     fn_sortedArrayGrow;
extern gameStringRelease_t   fn_gameStringRelease;

// First 16 bytes of the two uncovered callees, read from the IDB. Not
// g_hookPrologues rows (that table lists the sites the mod writes to):
// checked with VerifyPrologue before the stand-in is enabled (nm_nbr_seeds.cpp).
extern const unsigned char g_sortedArrayGrowBytes[16];
extern const unsigned char g_gameStringReleaseBytes[16];
// ---- end stand-in neighbour seeds ----

#endif // KENSHI_ZONE_OPT_NBR_SEED_BINDINGS_H
