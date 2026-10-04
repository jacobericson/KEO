// rva.h - Game addresses and generation-settings layout constants.
// Included through game.h.

#ifndef KEO_RVA_H
#define KEO_RVA_H

#include "game/klib_members.h"
#include "base/core.h"

// =========================================================================
// RVA constants (Kenshi Steam 1.0.65)
// =========================================================================

// From KenshiAddressLogger (runtime-resolved, authoritative)
const size_t RVA_SHOW_LOADING_MESSAGE = 0x6E9800;  // ForgottenGUI::showLoadingMessage(bool)

// From IDA Pro (same binary, not in KenshiLib exports)
const size_t RVA_IS_CONTENT_PENDING   = 0x3AB4F0;  // SectionManager::isContentPending (514 bytes)

// Hook targets
const size_t RVA_UPDATE_CAMERA_ZONE   = 0xA11DA0;  // ZoneManager::updateCameraZone (882 bytes)
const size_t RVA_STATE_MACHINE_DRIVER = 0xA0E950;  // ZoneManager::stateMachineDriver (1521 bytes)

// From KenshiAddressLogger (runtime-resolved, authoritative)
const size_t RVA_ADD_ORDER_SELECTED   = 0x7F9280;  // PlayerInterface::addOrderSelectedCharacters (1714 bytes)

// Called functions (thunk RVAs)
const size_t RVA_LOAD_SINGLE_ZONE    = 0x16243;   // thunk -> 0xA0D6A0 (405 bytes)
const size_t RVA_REGISTER_ZONE_SECTIONS = 0x2D7B8; // thunk -> 0x3ABF00 (923 bytes)
// Entry of SectionManager::finalizeZoneResources, the nest-validation guard's
// hook target (src/fixes/world/nest_validation.h): a hook patches the function's
// own bytes.
const size_t RVA_FINALIZE_ZONE_RES_ENTRY = 0x8F5100;
// TownList::destroy, called from inside the function above on a nest that
// fails validation. Hook target for the nest-validation guard's destroy
// counter (src/fixes/world/nest_validation.h): it has other, unrelated callers too,
// so the counter only attributes a call while it runs inside the guard's own
// call to the original above.
const size_t RVA_TOWNLIST_DESTROY    = 0x92A4F0;
const size_t RVA_FLUSH_PENDING_WORK  = 0x2E073;   // thunk -> 0x927540
// ZoneManager::activateZoneMap(ZoneMap*, iVector2 centre, int range,
// ZoneActivationType, float deactivationTimer): the single choke point every
// native activation reaches. Hooked, so this is the implementation.
const size_t RVA_ACTIVATE_ZONEMAP    = 0xA0E1B0;  // 584 bytes
// ZoneMap::update(ZoneMap*) -> bool, 276 bytes. Its one caller is
// ZoneManager::updateMainThread (0xA11B70), which walks a copy of the active
// zone set, so an erase inside the call is safe. The body swaps the back
// data, ticks the content and, unless loadingPhase > 1, decrements the three
// activation countdowns; when all three have run out it deactivates the cell,
// erases it from the set, notifies the overlay manager and returns false.
const size_t RVA_ZONEMAP_UPDATE      = 0xA0A960;
// The per-frame delta ZoneMap::update subtracts from those countdowns,
// written at the head of GameWorld::mainLoop_GPUSensitiveStuff. Real frame
// time, not the game-speed-scaled value beside it, so the countdowns run down
// at the same rate whatever the simulation speed.
const size_t RVA_GLOBAL_FRAME_TIME   = 0x2132730;
// The insert helper both tracking sets use: (set, outPair, &value, &pValue).
// Not a KenshiLib-named member; it is the out-of-line body behind
// ogre_unordered_set<ZoneMap*>::insert.
const size_t RVA_ADD_TO_TRACKING_SET = 0x386190;  // 471 bytes
// PhysicsInterface::setQueuesAreClear, 0x579460 (80 bytes): takes queuesClearMuto
// (+0x328) and writes +0x320. KenshiLib declares it, but its header RVA (0x579770)
// is from a different build, so bind this build's address directly.
const size_t RVA_SET_QUEUES_CLEAR    = 0x579460;

// Handle resolution (called from hook to iterate selected characters)
const size_t RVA_RESOLVE_HANDLE      = 0x3E419;   // thunk to handle->object resolution
const size_t RVA_HANDLE_TABLE        = 0x2132F38; // global handle lookup table (qword_142132F38)
const size_t RVA_HANDLE_SENTINEL     = 0x2132F10; // stale handle sentinel (qword_142132F10)

// NavMesh queue lock functions (thunk RVAs)
const size_t RVA_PATH_BUILDER_INIT     = 0x3D136;
const size_t RVA_PATH_BUILDER_FINALIZE = 0x4F8AE;
const size_t RVA_READER_UNLOCK         = 0x2E843;

// NavMeshGen__queueLockAcquire implementation (CS init despite the name, 401 bytes).
// Called 5x in NavMeshGen_struct_ctor to initialize the 5 CSes at +72/+104/+152/+200/+272.
// Used by CloneNMG to reinit the CSes of a cloned NMG (memcpy'd CS state is undefined).
const size_t RVA_QUEUE_LOCK_INIT       = 0x25F350;

// NavMesh cache: function RVAs (zone optimization -- included in all builds)
const size_t RVA_PROCESS_JOB_ALT    = 0x3CBE60;  // NavMeshGenerator::processJobAlt (8645 bytes)
const size_t RVA_DISPATCH_JOB       = 0x3CE030;  // NavMeshGenerator::dispatchJob (691 bytes, impl)
const size_t RVA_BUILD_COLLISION    = 0x39810;   // thunk -> 0x3CB700 (925 bytes)
const size_t RVA_PARTIAL_FIXUP      = 0x1C418;   // sub_14001C418: type 1 partial boundary fixup
const size_t RVA_ENQUEUE_TO_PROC_QUEUE = 0x2EEBA; // enqueue job to processing queue (+184)

// Game CRT allocators (safe on bg thread)
const size_t RVA_GAME_NEW          = 0xED563A;
const size_t RVA_GAME_DELETE       = 0xED5628;
const size_t RVA_GAME_NEW_ARR      = 0xED5634;
const size_t RVA_GAME_DEL_ARR      = 0xED562E;

// Global scratch buffer (used by processJobAlt + buildCollision internally)
const size_t RVA_SCRATCH_BUFFER    = 0x212DE98;
const size_t RVA_SCRATCH_SIZE      = 0x1D2C0D0;

// Havok TLS allocator index
const size_t RVA_HAVOK_TLS_INDEX       = 0x21369C8;

// hkaiNavMesh constructor
const size_t RVA_NAVMESH_CTOR          = 0xD32E30;
// hkaiNavMeshGenerationSettings constructor (writes vtable, inits arrays/refcount)
const size_t RVA_SETTINGS_CTOR         = 0xDD99D0;

// NavMeshResult::populate -- wrapper calling realGenerate
const size_t RVA_NM_RESULT_POPULATE    = 0xE0AFF0;  // 49 bytes

// Offsets into the input geometry that processJobAlt builds and passes as
// NavMeshResult__populate's second argument. Three hkArray slots; the first
// (+0) is unused by the generator. Each hkArray is {data(8), size(4), capFlags(4)}.
//   vertices: data +16, count +24   (setup 0x3C1580 writes v4[6], reserves v4+4, stride 16)
//   triangles: data +32, count +40  (setup writes v4[10], reserves v4+8, stride 16)
// realGenerate (0xDFC060) tests `cmp dword ptr [r12+28h], 0` and, when zero,
// asserts "Passed in empty triMesh to generateNavMesh" and returns without
// generating — so count 0 here is exactly "empty input".
const int OFF_GEOM_VERTEX_COUNT   = 24;
const int OFF_GEOM_TRIANGLE_COUNT = 40;
// HavokNavMesh::realGenerate -- the actual heavy compute
const size_t RVA_REAL_GENERATE         = 0xDFC060;   // 61,317 bytes

// Havok thread init sequence (for worker thread TLS initialization)
const size_t RVA_HAVOK_CONTEXT_INIT    = 0xBA4770;
const size_t RVA_HAVOK_GET_MANAGER     = 0xBAF970;
const size_t RVA_HAVOK_POST_REG_INIT   = 0xBAECF0;
const size_t RVA_HAVOK_CLEANUP         = 0xBAED40;
const size_t RVA_HAVOK_CTX_CLEANUP     = 0xBA9580;

// =========================================================================
// Structure layouts (offsets, not addresses)
// =========================================================================

// hkaiNavMeshGenerationSettings +520 override entries (SortedArray, 240 B each).
// Built by NavMeshGenerator__initWorkBuffer (0x3C48D0) from a staging entry that
// OverrideSettings__initFromSettings (0xDD9280) fills.
const int OVR_OFF_VOLUME   = 0;    // hkRefPtr; NULL on all four base entries
const int OVR_OFF_MATERIAL = 8;    // int; -1 on the staging copy, 1..4 on the base entries
const int OVR_OFF_FLAG     = 12;   // byte, from settings+332
const int OVR_OFF_SLOPE    = 16;   // float walkable slope, radians
const int OVR_OFF_EMP      = 20;   // 56 bytes copied from settings+76
const int OVR_OFF_SIMPL    = 80;   // SimplificationSettings sub-object (160 bytes)


// =========================================================================
// RVAs, continued
// =========================================================================

// Collision builders, hooked by the collision-build lock (nm_buildlock.cpp):
// buildCollisionCS around each whole builder in the wide mode, the game's build
// mutex around buildCollision's racing regions in the narrow mode (the default).
// These are the implementations, NOT the thunks: RVA_BUILD_COLLISION above is
// the thunk fn_buildCollision calls, and hooking the implementation catches
// both that path and dispatchJob_orig's own calls. Both are reached only from
// dispatchJob_orig (0x3CE030), and both reach the game's build mutex
// (0x212DEB8) through buildSectionCollision (0x3CB140).
const size_t RVA_BUILD_COLLISION_IMPL          = 0x3CB700;  // 925 bytes
const size_t RVA_BUILD_COLLISION_INTERIOR_IMPL = 0x3CBAB0;  // 938 bytes

// The narrow collision-build lock.
const size_t RVA_NMG_STITCH_UNLOADED_ZONE  = 0x3CB140;  // buildSectionCollision: takes the build mutex itself
const size_t RVA_NMG_STITCH_WITH_INTERIORS = 0x3C5C10;  // the tail stitch over a section's instance list
const size_t RVA_BC_RET_LAST_NEIGHBOUR     = 0x3CB8F2;  // return from buildCollision's 4th stitchUnloadedZone call
const size_t RVA_BC_RET_TAIL_STITCH        = 0x3CB918;  // return from buildCollision's stitchWithInteriors call
// Function ends (start + size), for classifying return addresses.
const size_t RVA_BUILD_COLLISION_IMPL_END          = 0x3CBA9D;
const size_t RVA_BUILD_COLLISION_INTERIOR_IMPL_END = 0x3CBE5A;
const size_t RVA_NMG_STITCH_UNLOADED_ZONE_END      = 0x3CB6EC;

// Clone finalize guards (prevent operations on uninitialized OverrideSettings entries)
// hkaiNavMeshGenerationSettings destructor BODY. NOT the deleting destructor
// 0xDDABE0, which frees the object through the class allocator; ours came from
// HavokTlsAlloc and is freed by HavokTlsFree. Not 0xDD92F0 either, which is the
// unrelated per-entry OverrideSettings destructor.
const size_t RVA_SETTINGS_DTOR_BODY  = 0xDD9BC0;  // 643 bytes

// NavMesh::stop. Clears +0x1C8, joins the path thread, deletes the manager and
// shuts the Havok memory system down — so the worker pool has to be retired
// before it runs. Prologue is `test rcx,rcx` + short `jz`, i.e. the first 5
// bytes contain a relative branch.
const size_t RVA_NAVMESH_STOP        = 0x3AAE90;

// The cross-section un-stitch a navmesh instance's teardown runs: it walks the
// dying section's streaming-set connection records and clears, on the opposite
// instance, the edges that pointed into this one. Prologue is a single
// `mov [rsp+10h],rdx`, so the five bytes a detour takes are one whole
// instruction. Hooked by src/fixes/stitch/unstitch_guard.cpp in every build.
const size_t RVA_UNSTITCH_CROSS_SECTION = 0xD9AB20;

// The two calls that walk makes, used by the guard's replacement walk with the
// arguments the native walk would have passed: releasing the dying instance's
// free-edge block lists, and removing one instanced edge from an instance.
const size_t RVA_GRAPHINST_RELEASE_FREE_BLOCKS   = 0xD273E0;
const size_t RVA_GRAPHINST_REMOVE_INSTANCED_EDGE = 0xD279A0;

// NavMeshGenerator::stitch: pairs two NavInstances' meshes and graphs and
// writes the cross-section connection records into both graphs' streaming
// sets. Its only writer path is Havok's addStreamingSetPair. Hooked by
// src/fixes/stitch/stitch_source.cpp in every build. The return addresses of its four
// call sites name which caller reached it.
const size_t RVA_NMG_STITCH               = 0x3C56F0;
const size_t RVA_STITCH_RET_UNLOADED      = 0x3CB461;  // stitchUnloadedZone: completed task or live sector
const size_t RVA_STITCH_RET_DISK          = 0x3CB64B;  // stitchUnloadedZone: loaded from disk, saved, freed
const size_t RVA_STITCH_RET_INTERIORS     = 0x3C5D0B;  // stitchWithInteriors
const size_t RVA_STITCH_RET_SPLICE        = 0x3CA5A4;  // splice

// NavMeshGenerator::update: the path thread's drain of the done queue, called
// once per NavMesh::update pass (0x3AE44E) holding nothing. Pass-through
// detour in src/navmesh/scheduling/nm_adjacency.cpp: after it returns, a
// published navmesh task absent from done has been drained.
const size_t RVA_NMG_UPDATE               = 0x3C8B70;

// The A* search state's "record this node's cost" step, with the nav-mesh
// heuristic inlined into it: it stores the cost, and for a node it has not
// estimated yet it turns the node's packed key into a world position through
// the section's graph instance and measures the distance to the goal. The
// instance comes from a pointer cached on the heuristic's holder at +16, and
// neither that pointer nor the position array reached through it is checked.
// Prologue is `mov r11,[rcx+48h]` + `test byte ptr [r11+0Eh],3`, so the five
// bytes a detour takes are whole, position-independent instructions.
// Hooked by src/fixes/search/graph_visitor_guard.cpp in every build.
const size_t RVA_SEARCH_SET_NODE_COST = 0xDA6370;

// The A* iteration that pops the cheapest node off the open set and expands
// it. Its preamble needs the popped node's own position, so it reads the node
// record and the node position out of the graph instance of the node's
// section, taken from the visitor's per-section cache at +8 -- the slot beside
// the one the node-cost site above uses, with the same missing checks. The
// prologue is a single five-byte `mov [rsp+18h], rbx`, which is exactly what a
// detour takes and is position-independent.
// Hooked by src/fixes/search/graph_expand_guard.cpp in every build.
const size_t RVA_GRAPH_EXPAND_NODE = 0xDA5860;

// The open set's pop: it reads the key at the front of the heap before it
// sifts, so the key is peekable without consuming the node. Called, never
// detoured, by the expand guard's firing arm, which has to consume the node
// the iteration will not expand. Its head bytes are checked at install.
const size_t RVA_OPENSET_POP_NEXT = 0xD2C260;

// The node-position helper the cluster-graph search calls to turn a packed
// key into a world position -- reached only when checkFaceConnectivity
// consults the original graph (clusterGraphBypass = measure, player or off),
// not through either A* search state above. It carries its own one-slot
// per-section instance cache on a small per-search context and, like the two
// sites above, dereferences the cached or freshly looked-up instance without
// checking it, to reach the position array at its own +0x30. Prologue is
// `mov [rsp+8],rbx` then a register load, five whole bytes with no branch.
// Hooked by src/fixes/search/graph_position_guard.cpp in every build.
const size_t RVA_GRAPH_POSITION_LOOKUP = 0xDA4470;

// The hierarchical heuristic's goal-adjacency test, which reads a section's cluster-graph instance
// unchecked. Hooked by src/fixes/search/graph_heuristic_guard.cpp while the guard is wanted.
const size_t RVA_GRAPH_HEURISTIC_GOAL_ADJACENT = 0xDA6A30;

// The hierarchical heuristic's cluster-centre lookup, the same unchecked instance read.
// Hooked by src/fixes/search/graph_heuristic_guard.cpp while the guard is wanted.
const size_t RVA_GRAPH_HEURISTIC_CLUSTER_CENTRE = 0xDA6690;

// The coarse-search seed, shared by the heuristic's init and the pathExists setup.
// Hooked by src/fixes/search/graph_heuristic_guard.cpp while the guard is wanted.
const size_t RVA_GRAPH_HEURISTIC_COARSE_SEED = 0xDA5FE0;

// The collection's graph-instance connect, called by addInstance for every registered graph:
// it copies each cross-tile link's stored cost into both instances' owned edges.
// Hooked by src/fixes/search/cluster_cross_cost.cpp while clusterCrossCost is on.
const size_t RVA_GRAPH_INSTANCE_CONNECT = 0xD9AD30;

// The per-face AABB step a navmesh instance's clearance reset runs: it fetches
// one face record and walks the consecutive edges the record names, bounding
// neither the face index nor either end of the edge run. Prologue is a single
// `mov [rsp+18h],r8`, so the five bytes a detour takes are one whole
// instruction. Hooked by src/fixes/streaming/mesh_face_guard.cpp in every build.
const size_t RVA_NAVMESH_FACE_AABB = 0xCFD610;

// The instance's own face accessor, called -- not hooked -- by the guard so
// the record it judges is the record the original will fetch.
const size_t RVA_NAVMESH_FACE_FROM_INDEX = 0xCF3180;

// The instance's own edge accessor, same shape as the face accessor above
// (index in range: original array; at or past it: the owned array), called
// by the guard to pre-scan the vertex index each edge of a face's run names.
const size_t RVA_NAVMESH_EDGE_FROM_INDEX = 0xCF3120;

// The min/max pair that step seeds its two accumulators from; the max half is
// this value's sign flip. Written when the engine starts and zero in the
// image, so it is read at runtime and never reproduced.
const size_t RVA_HKAI_AABB_SEED = 0x2104730;

// The two vtables the lifecycle rows and the face guard identify their objects
// by, so an object of another type reaching either site is handed on untouched
// instead of being measured against offsets that mean nothing there.
const size_t RVA_HKAI_NAVMESH_INSTANCE_VFTABLE = 0x17A9C38;
const size_t RVA_HKAI_NAVMESH_VFTABLE          = 0x17AA658;

// hkaiStreamingCollection::removeInstance. Takes the collection, the navmesh
// instance and its graph instance, and clears the instance's slot. Prologue is
// three argument spills, so the five bytes a detour takes are whole
// instructions. Hooked by src/fixes/streaming/navmesh_life.cpp.
const size_t RVA_REMOVE_INSTANCE = 0xD0DC30;

// NavMesh::createInstance(NavMesh*, NavInstance*). Builds a navmesh and graph
// instance for the NavInstance and queues it in addList, after removing and
// freeing every queued entry with the same uid -- including the argument
// itself when it is already queued. Prologue is seven register pushes then a
// lea, so the bytes a detour takes are whole instructions. Hooked by
// src/fixes/streaming/create_instance_guard.cpp in every build.
const size_t RVA_NAVMESH_CREATE_INSTANCE = 0x3ACD20;

// PhysicsActual::updateUT(this): flushes the main-thread message lists onto
// the physics thread's, hullsToDestroy among them. Main thread, only while
// the physics thread is idle. Prologue is two register pushes, sub rsp and
// movs, all position-independent. Hooked by src/fixes/physx/hull_queue_guard.cpp.
const size_t RVA_PHYSICS_UPDATE_UT = 0x4CD040;

// Vtable slot 1 of the hull classes: releases the object's render pieces
// and appends it to hullsToDestroy's main list. Each opens with the five-byte
// `mov [rsp+8],rbx`. Hooked, for the caller only, by hull_queue_guard.cpp.
const size_t RVA_HULL_PUSH_HULL   = 0x4CC210;   // PhysicsHullT
const size_t RVA_HULL_PUSH_ENTITY = 0x7DC170;   // SimplePhysXEntity family
const size_t RVA_HULL_PUSH_SCYTHE = 0x7DBF20;   // ScythePhysicsT
const size_t RVA_HULL_PUSH_ROOT   = 0x7DC0B0;
const size_t RVA_HULL_PUSH_BASE   = 0x7DC110;

#ifdef KEO_DEBUG
// NavMesh::deleteInstance. Tears one navmesh instance down and, when the
// instance is not queued in addList and its runtime id is non-negative, runs
// the cross-section un-stitch under changeMutex. DEV only: its one caller is
// the read-only probe in src/fixes/stitch/unstitch_probe.cpp.
const size_t RVA_NAVMESH_DELETE_INSTANCE = 0x3AB8A0;

// The two section-table lookups the world step makes from a packed key, both
// unchecked in the engine. DEV only: their one caller each is the read-only
// probe in src/fixes/streaming/section_key_probe.cpp.
//
// The clearance reset ("reset clearance cache" in the step's own timer
// labels): walks a list of packed keys and indexes the collection's instance
// array with each key's top 10 bits. Its context holds the collection at +56
// and a second pending key list at +80/+88; the caller's list arrives as an
// hkArray<int> in the fourth argument.
const size_t RVA_CLEARANCE_RESET_KEYS = 0xD617D0;

// The cut lookup: takes the collection and one packed key as arguments and
// makes the same indexed read in its first statements.
const size_t RVA_SECTION_CUT_LOOKUP   = 0xDCAEE0;

// loadPhysXResource: opens a collider resource and parses it into an NXU
// collection, whose every name goes through the engine's global string-intern
// pool. DEV only: its one caller is the pass-through counter in
// src/diag/physx_pool_probe.cpp. The site is already detoured by RE_Kenshi,
// which loads this plugin, so ours is installed on top and runs outermost.
const size_t RVA_LOAD_PHYSX_RESOURCE  = 0x7E4850;
#endif

const size_t RVA_SIMPL_SETTINGS_DTOR = 0x3DA120;  // 119 bytes, SimplificationSettings::dtor
const size_t RVA_SIMPL_SETTINGS_COPY = 0x3DA000;  // 224 bytes, SimplificationSettings::copy
                                                  // (scalars + ExtraVertexSettings + an
                                                  //  hkStringPtr, so never a plain memcpy)

const size_t RVA_EDGE_PROCESS        = 0xDD92F0;  // edgeProcess: per-entry cleanup in finalize

// Group cohesion: CharMovement::setDestination (called for arrival scatter)
const size_t RVA_SQRTF              = 0xED5F7E;  // sqrtf import (scatter patch anchor)

// Pathfinding diagnostics: hook targets (contentStream bg thread)
const size_t RVA_CS_FIND_PATH       = 0x3AA950;
const size_t RVA_CS_CHECK_FACE_CONN = 0x3A5B00;
const size_t RVA_FIND_PATH_FULL     = 0xCE56D0;
const size_t RVA_CS_FIND_PATH_FALLBACK = 0x3AABF0;

// Squad path priority boost (main thread hooks)
const size_t RVA_REQUEST_PATH       = 0x145CB0;
const size_t RVA_PATH_REQ_SUBMIT    = 0x3AAEF0;
const size_t RVA_ENQUEUE_PATH_REQ   = 0x3B6110;
const size_t RVA_NAVMESH_GET_FACE_KEY_VEC4 = 0x3A1CB0;  // NavMesh::getFaceKey_hkVector4f(pos, rayLength): lock-free, the caller holds +0x200 shared

// Island routing. Verified against the IDB (kenshi_x64.exe 1.0.65).
//   ZoneMap::isInIsland      0xA07EB0 (19 bytes): `b && a->island == b->island`.
//                            Sole caller: CharMovement::setDestination (0x6607E0)
//                            via thunk 0x2FCE8 at 0x660B70. We hook the impl —
//                            the thunk's only path leads there.
//   ZoneManager::getIsland   0xA09AE0 (169 bytes): appends every Set B zone whose
//                            label equals t->island to a lektor<ZoneMap*>.
//                            Sole caller: ZoneMap::getActiveZoneIsland (0xA09B90),
//                            used by computeProjectedDest (0x3A39C0) and the
//                            smell picker (0x8F4A10).
//   lektor reserve           thunk 0x16630 -> 0x37E3A0 (grow to n, 0 => 10).
//   _calculateIslands        0xA09520: Set B only, on updateRendertimeThread.
//   isZoneStillLoading       0x3AC810: `zone && isContentPending(mgr, zone+24)`.
const size_t RVA_ISINISLAND_IMPL      = 0xA07EB0;
const size_t RVA_ISINISLAND_THUNK     = 0x2FCE8;   // documented; not hooked
const size_t RVA_GETISLAND_IMPL       = 0xA09AE0;
// getIsland's two callers' own return addresses (both jmp thunks into
// getActiveZoneIsland, so _ReturnAddress() inside the hook is the caller's,
// unless another plugin's detour sits in front of ours). Verified in IDA:
// NavMesh::getZoneEdge+0xC1 (the call at 0x3A3A7C) and
// sub_1408F4A10+0xD3 (the instruction after the smell picker's own
// getActiveZoneIsland call at 0x8F4ADE).
const size_t RVA_GETISLAND_RET_EDGE   = 0x3A3A81;
const size_t RVA_GETISLAND_RET_SMELL  = 0x8F4AE3;
const size_t RVA_LEKTOR_RESERVE       = 0x16630;   // thunk -> 0x37E3A0
const size_t RVA_CALCULATE_ISLANDS    = 0xA09520;  // documented; never called/hooked
const size_t RVA_GET_ZONE_EDGE = 0x3A39C0;  // NavMesh::getZoneEdge; emulated in island_stuck.cpp (IslandEmulateCrossing), hooked by src/planner/planner_hooks.cpp while plannerMode is set
const size_t RVA_SET_DESTINATION_VEC3      = 0x6607E0;  // CharMovement::setDestination_Vec3 (vtable slot 18 via 0x4E73D)
const size_t RVA_SETDEST_RET_EDGE_RECHECK  = 0x660EFD;  // returns from its getZoneEdge call at 0x660EF8 (arrival recheck)
const size_t RVA_SETDEST_RET_EDGE_COMPUTE  = 0x660F43;  // returns from its call at 0x660F3E (initial, rung, recompute)
const size_t RVA_NAVMESH_GET_CLOSEST_POINT = 0x3A21C0;  // NavMesh::getClosestPoint(point, radius, inset, filter, out, key)
const size_t RVA_CHARMOVEMENT_UPDATE      = 0x65F510;  // CharMovement::update(this, float dt), vtable slot 11, the AI thread's list 1; pre-call hooked by src/planner/planner_prearrival.cpp
const size_t RVA_CHARSTATS_CALC_SWIM_SPEED = 0x884FE0; // CharStats::calculateSwimSpeed(): pure reads; the GUI calls it on the main thread
const size_t RVA_CHARACTER_GET_WATER_LEVEL = 0x5C7540; // Character::getWaterLevel(): pure reads; 0 = NO_WATER
const size_t RVA_RACEDATA_IS_IMMUNE = 0x5E7290;  // RaceData::isImmune(WeatherAffecting): a hash-set find; no lock, no write
const size_t RVA_SECTIONMGR_LOOKUP_FROM_POSITION = 0x8F48A0;  // SectionManager::lookupFromPosition(pos): the biome record, arithmetic
const size_t RVA_GLOBAL_AREA_SECTOR_GRID = 0x2133098;  // sectionMgr: KenshiLib's SectionManager, the 64x64 AreaSector grid; not RVA_GLOBAL_SECTION_MGR
const size_t RVA_CHARACTER_ADD_JOB = 0x5C8310;  // Character::addJob(task, subject, shift, addDontClear, location): main thread
const size_t RVA_SPEED_GROUP_GET_SPEED = 0x7F4BA0;  // SpeedGroup::getSpeed(group, who): AI back thread and main thread
const size_t RVA_DOOR_HIT_FILTER_VTABLE    = 0x16C9188; // DoorHitFilter's vftable, the snap's filter
const size_t RVA_IS_ZONE_STILL_LOADING  = 0x3AC810; // documented

// Global data RVAs
const size_t RVA_GLOBAL_SECTION_MGR  = 0x2133560;  // pauseState.navmesh (SectionManager*)
const size_t RVA_SAVE_FILE_SYSTEM = 0x212DC08;  // SaveFileSystem* singleton, read by src/planner/coarse_graph_base.cpp
// pauseState.physics (PhysicsInterface*, i.e. ou->physics). Fields we touch:
//   +0x1B0/+0x200/+0x2A8/+0x2F8  work-queue counts read by isReadyForSections
//   +0x320 (800)  bool _queuesClear  -- the ready flag loadSingleZone clears
//   +0x328 (808)  boost::shared_mutex queuesClearMuto, guards +0x320.
// Write +0x320 only through fn_setQueuesAreClear (it takes that lock).
const size_t RVA_PAUSESTATE_PHYSICS  = 0x21330C8;
const size_t RVA_GLOBAL_PLAYER       = 0x2133630;
// The GameWorld singleton itself, the base the two RVAs above sit inside. The
// IDB names it `pauseState` (2248 bytes, .data, 513 references). Confirmed as
// the destroyListOE fault object: a faulting frame's RSI was 0x7FF7424230B0, i.e.
// kenshi_x64 + 0x21330B0. Read-only, by the destroyListOE probe (fixes/world/destroy_list_defer.cpp).
const size_t RVA_GLOBAL_GAMEWORLD    = 0x21330B0;
const size_t OFF_GAMEWORLD_FRAME_SPEED_MULT = 0x700;  // float, the game-speed multiplier
KLIB_ASSERT_OFFSET(GameWorld_frameSpeedMult, OFF_GAMEWORLD_FRAME_SPEED_MULT);

// GameWorld::destroyListOE's sole inserter, sub_140799BE0 (197 bytes): it hides
// the MovableObject, then inserts it into the set at GameWorld+0x680. Hooked
// pass-through, for its caller's thread id only (destroyListOE diagnostic).
const size_t RVA_DESTROYLIST_INSERT  = 0x799BE0;



#endif // KEO_RVA_H
