// hook_manifest_rows.inc — the hook manifest: one row per game site the mod
// writes to, in build-gate order.
//
//   HOOK_ROW(id, name, rva, kind, installer, want, caps, b0, ..., b15)
//
// id is HOOK_ followed by the RVA constant's name without RVA_, so a row is
// found from either name. There is no include guard: each reader defines
// HOOK_ROW, includes this list and undefines it.
//
// STANDING RULE: every site the mod writes to adds a row here; its installer
// reaches it through HookInstallRow. A site with no row is a site that
// installs blind on an unknown build. startPlugin verifies every row before
// installing anything, and the scatter byte-patch in formation.cpp shares
// addOrderSelected's row with the hook on the same function.
//
// Every byte was read from the Steam 1.0.65 IDB (mcp__ida__read_data, 16 bytes
// at the RVA) — never from a KenshiLib header comment, which
// carries a different build's addresses.
//
// The rows are compile-gated exactly like their install sites, so the count in
// the "Build gate: ok (N sites)" line is the number of sites this build can
// actually touch.
//
// Sites outside the manifest by design:
// - the render levers' module sites in OgreMain and the D3D11 DLL, which the
//   render module gate checks (their three exe rows are here, and the levers
//   install them);
// - the two mid-function patches, the PhysX query guard and the stitch byte
//   guard, which verify their own bytes exactly, because a prologue's
//   shared-site allowance is wrong mid-function;
// - the purecall recorder's data write into PhysXCore64's CRT, which is not an
//   exe site.

	// --- unconditional: hook_manifest.cpp's startup groups, or the module file a row names ---
	HOOK_ROW(HOOK_SHOW_LOADING_MESSAGE, "showLoadingMessage", RVA_SHOW_LOADING_MESSAGE, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
	HOOK_ROW(HOOK_IS_CONTENT_PENDING, "isContentPending", RVA_IS_CONTENT_PENDING, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,0x48)
	HOOK_ROW(HOOK_UPDATE_CAMERA_ZONE, "updateCameraZone", RVA_UPDATE_CAMERA_ZONE, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x56,0x57,0x41,0x54)
	// Hook target and, in FULL builds, the scatter patch's scan base.
	HOOK_ROW(HOOK_ADD_ORDER_SELECTED, "addOrderSelected", RVA_ADD_ORDER_SELECTED, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_MOVEMENT_AWARE, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x55,0x41,0x56,0x48,0x8D,0x68,0x98,0x48,0x81)
	// destroyListOE inserter. The row is unconditional because the install is
	// gated by INI keys, not by a compile step: destroyListDefer (default on) or
	// destroyListDiag can want it in any build, so the gate has to be able to
	// verify it in any build. It is NOT marked diagnostic any more: the hook now
	// carries the destroyListOE mitigation, and a build that silently ran without
	// it would leave the race the gate exists to prevent. A mismatch here is
	// fatal like every other non-diagnostic row.
	// Note the cost of that choice: the gate loop checks every row regardless of
	// whether the hook will be installed, so this row is fatal even in a session
	// that has both destroyListDefer and destroyListDiag set false and would
	// never have touched this site at all.
	// On an unrecognised binary that is the intended answer — refuse to run —
	// but it does mean the INI cannot be used to work around a mismatch here.
	HOOK_ROW(HOOK_DESTROYLIST_INSERT, "destroyListInsert", RVA_DESTROYLIST_INSERT, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_DESTROY_LIST, HOOK_CAP_NONE,
	         0x48,0x89,0x54,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE)
	// The save-load reset's Set A/B unload, sub_14036C1E0. The
	// hook unloads the mod's zones the reset would otherwise leave alive with a
	// wiped registry, so the row is
	// fatal like destroyListInsert's: a build that ran without it would bring
	// that crash back. Unconditional, like its install (the INI key
	// saveLoadUnload only decides whether the hook unloads or just counts).
	// `mov r11,rsp; mov [r11+8],rcx; push rbp/rsi/rdi/r12/r13/r14`.
	HOOK_ROW(HOOK_RESET_UNLOAD_ZONES, "resetUnloadZones", RVA_RESET_UNLOAD_ZONES, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x4C,0x8B,0xDC,0x49,0x89,0x4B,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56)
	// ZoneManager::processLoading: the escape-menu pause guard. `mov rax,rsp;
	// push rbp/r12/r13; lea rbp,[rax-5Fh]; sub rsp,0E0h`: position-independent.
	HOOK_ROW(HOOK_PROCESS_LOADING, "processLoading", RVA_PROCESS_LOADING, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ESCAPE_PAUSE, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xE0)
	// ActivePlatoon::calculateCurrentPos (src/fixes/world/corpse_pin.cpp): `mov
	// [rsp+18h],rbx; push rsi; sub rsp,40h; cmp dword ptr [rcx+58h],0`.
	// diagnostic = true: a mismatch turns the corpse-pin fix off; the squad
	// still behaves exactly as vanilla, which is the state the mod shipped in
	// before this fix existed.
	HOOK_ROW(HOOK_ACTIVEPLATOON_CALC_POS, "calculateCurrentPos", RVA_ACTIVEPLATOON_CALC_POS, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_CORPSE_PIN, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x20,0x56,0x48,0x83,0xEC,0x40,0x83,0x79,0x58,0x00,0x48,0x8B)

#if ZONEHAND_STEP >= 2
	// ZoneManager::activateZoneMap (src/zone/handoff/zone_lifecycle_hooks.cpp): seven
	// register pushes then `sub rsp,0A0h`, so the five bytes a detour takes
	// are three whole instructions and position-independent. Fatal, not
	// diagnostic: a build that adopts publishes no zone itself, so without
	// this detour a cell the mod holds is one the game can never take back.
	HOOK_ROW(HOOK_ACTIVATE_ZONEMAP, "activateZoneMap", RVA_ACTIVATE_ZONEMAP, HOOK_FATAL, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x81,0xEC,0xA0,0x00)
	// SectionManager::finalizeZoneResources (src/fixes/world/nest_validation.cpp):
	// `push rdi; sub rsp,50h; mov [rsp+var_38],-2; mov [rsp+arg_0],rbx`.
	// diagnostic = true: a mismatch turns the guard off; finalizeZoneResources
	// then runs exactly as before the guard existed -- vanilla-adjacent
	// behaviour, not a new mod-caused crash.
	HOOK_ROW(HOOK_FINALIZE_ZONE_RES_ENTRY, "finalizeZoneResources", RVA_FINALIZE_ZONE_RES_ENTRY, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_NEST_GUARD, HOOK_CAP_NONE,
	         0x40,0x57,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,0x48)
	// TownList::destroy (src/fixes/world/nest_validation.cpp): counts a destroy only
	// while it runs inside finalizeZoneResources's original, distinguishing
	// "nothing was destroyed" from "we did not look". diagnostic = true: a
	// mismatch just loses the counter, which never changes the destroy path.
	// `mov rax,rsp; push rsi/rdi/r12; sub rsp,0A0h; mov [rsp+var_88],-2`.
	HOOK_ROW(HOOK_TOWNLIST_DESTROY, "townListDestroy", RVA_TOWNLIST_DESTROY, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_NEST_GUARD, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x56,0x57,0x41,0x54,0x48,0x81,0xEC,0xA0,0x00,0x00,0x00,0x48,0xC7)
#endif

#if ZONEHAND_STEP >= 3
	// ZoneMap::update (src/zone/handoff/zone_lifecycle_hooks.cpp): `mov [rsp+8],rbx; push
	// rdi; sub rsp,20h`, so the five bytes a detour takes are one whole
	// instruction. diagnostic = true: without it a cell the game has taken
	// over expires the way any other member of the set does, which is vanilla
	// behaviour rather than a mod-caused fault. The churn that leaves is a
	// reason to say so loudly at the install site, not to refuse a plugin
	// whose navmesh cache and render levers are unaffected.
	HOOK_ROW(HOOK_ZONEMAP_UPDATE, "zoneMapUpdate", RVA_ZONEMAP_UPDATE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x81,0x88,0x00,0x00)
#endif

	// The cross-section un-stitch (src/fixes/stitch/unstitch_guard.cpp): the five
	// bytes a detour takes are one whole `mov [rsp+10h],rdx`. Present in every
	// build, because the fault it guards is a measured crash rather than a
	// diagnostic. diagnostic = true: a mismatch refuses the guard alone, which
	// leaves the teardown exactly as vanilla runs it.
	HOOK_ROW(HOOK_UNSTITCH_CROSS_SECTION, "unstitchCrossSection", RVA_UNSTITCH_CROSS_SECTION, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_UNSTITCH_GUARD, HOOK_CAP_NONE,
	         0x48,0x89,0x54,0x24,0x10,0x53,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8B,0xFA)

	// NavMeshGenerator::stitch (src/fixes/stitch/stitch_source.cpp): seven register
	// pushes; the five-byte patch splits the fourth, so the trampoline takes
	// the first four whole (six bytes), with no branch among them.
	// diagnostic = true: a mismatch loses the write-side record alone, and the
	// stitch runs exactly as vanilla.
	HOOK_ROW(HOOK_NMG_STITCH, "nmgStitch", RVA_NMG_STITCH, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24)

	// The A* node-cost step (src/fixes/search/graph_visitor_guard.cpp): a register
	// load then a test, so the bytes a detour takes are whole instructions
	// with no branch among them. Present in every build, because the fault it
	// guards is a measured crash rather than a diagnostic. diagnostic = true:
	// a mismatch means these are not the bytes the guard was written against,
	// so the guard must not arm -- but refusing the whole plugin over it would
	// take the navmesh cache and the render levers down to avoid one crash,
	// and without the guard the site behaves exactly as vanilla does.
	HOOK_ROW(HOOK_SEARCH_SET_NODE_COST, "searchSetNodeCost", RVA_SEARCH_SET_NODE_COST, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_GRAPH_VISITOR, HOOK_CAP_NONE,
	         0x4C,0x8B,0x59,0x48,0x41,0xF6,0x43,0x0E,0x03,0xF3,0x41,0x0F,0x11,0x1B,0x0F,0x85)

	// The A* expand step (src/fixes/search/graph_expand_guard.cpp): a single five-byte
	// register spill, so the bytes a detour takes are one whole instruction
	// with no branch in it. Present in every build, for the same reason as the
	// row above: the fault it guards is the recorded crash one cache slot over,
	// not a diagnostic. diagnostic = true on the same ruling -- a mismatch
	// means these are not the bytes the guard was written against, so the guard
	// must not arm, and with it absent the site behaves exactly as vanilla
	// does, whereas refusing the whole plugin would take the navmesh cache and
	// the render levers down with it.
	HOOK_ROW(HOOK_GRAPH_EXPAND_NODE, "graphExpandNode", RVA_GRAPH_EXPAND_NODE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_GRAPH_EXPAND, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41)

	// The cluster-graph node-position helper (src/fixes/search/graph_position_guard.cpp):
	// a register spill then a load, so the bytes a detour takes are whole
	// instructions with no branch among them. Present in every build, for the
	// same reason as the two rows above: the fault is the same absent-instance
	// shape, on a third copy of the cache, reached only when the cluster graph
	// is actually consulted. diagnostic = true, same ruling -- a mismatch means
	// these are not the bytes the guard was written against, so it must not
	// arm, and without it the site behaves exactly as vanilla does.
	HOOK_ROW(HOOK_GRAPH_POSITION_LOOKUP, "graphPositionLookup", RVA_GRAPH_POSITION_LOOKUP, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x4C,0x8B,0x09,0x49,0x8B,0xD8,0x44,0x8B,0xC2,0x4D,0x8B)

	// The hierarchical heuristic's three unchecked cluster-graph instance reads
	// (src/fixes/search/graph_heuristic_guard.cpp): each prologue is argument
	// spills and register loads (the seed's also one stack adjust), so the bytes
	// a detour takes are whole, position-independent instructions with no branch
	// among them. Wanted only while graphHeuristicGuard is set. diagnostic = true:
	// a mismatch refuses these guards alone, and without them the sites behave
	// exactly as vanilla does.
	HOOK_ROW(HOOK_GRAPH_HEURISTIC_GOAL_ADJACENT, "graphHeuristicGoalAdjacent", RVA_GRAPH_HEURISTIC_GOAL_ADJACENT, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_GRAPH_HEURISTIC, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57)
	HOOK_ROW(HOOK_GRAPH_HEURISTIC_CLUSTER_CENTRE, "graphHeuristicClusterCentre", RVA_GRAPH_HEURISTIC_CLUSTER_CENTRE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_GRAPH_HEURISTIC, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x4C,0x8B,0x51,0x08,0x44,0x8B,0xCA,0x49,0x8B,0xD8,0x41)
	HOOK_ROW(HOOK_GRAPH_HEURISTIC_COARSE_SEED, "graphHeuristicCoarseSeed", RVA_GRAPH_HEURISTIC_COARSE_SEED, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_GRAPH_HEURISTIC, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x8B)

	// The collection's graph-instance connect (src/fixes/search/cluster_cross_cost.cpp): the
	// prologue opens with `mov r11,rsp` and `mov [r11+10h],rdx`, so the bytes a detour takes
	// are two whole register-relative instructions with no branch. Wanted only while
	// clusterCrossCost is set. diagnostic = true: a mismatch refuses the cost fix alone, and
	// without it the connect behaves exactly as vanilla does.
	HOOK_ROW(HOOK_GRAPH_INSTANCE_CONNECT, "graphInstanceConnect", RVA_GRAPH_INSTANCE_CONNECT, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_CLUSTER_CROSS_COST, HOOK_CAP_NONE,
	         0x4C,0x8B,0xDC,0x49,0x89,0x53,0x10,0x49,0x89,0x4B,0x08,0x53,0x55,0x48,0x81,0xEC)

	// The per-face AABB step (src/fixes/streaming/mesh_face_guard.cpp): the five bytes a
	// detour takes are one whole `mov [rsp+18h],r8`. Present in every build,
	// because the fault it guards is a measured crash rather than a
	// diagnostic. diagnostic = true: a mismatch means these are not the bytes
	// the guard was written against, so the guard must not arm, and without it
	// the step behaves exactly as vanilla does.
	HOOK_ROW(HOOK_NAVMESH_FACE_AABB, "navMeshFaceAabb", RVA_NAVMESH_FACE_AABB, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_MESH_FACE, HOOK_CAP_NONE,
	         0x4C,0x89,0x44,0x24,0x18,0x53,0x56,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x98,0x00)

	// hkaiStreamingCollection::removeInstance (src/fixes/streaming/navmesh_life.cpp):
	// three argument spills, so the bytes a detour takes are whole
	// instructions and position-independent. diagnostic = true: the detour
	// only reads, and a mismatch costs the retire rows alone.
	HOOK_ROW(HOOK_REMOVE_INSTANCE, "removeInstance", RVA_REMOVE_INSTANCE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_NAVMESH_LIFE, HOOK_CAP_NONE,
	         0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,0x41)

	// NavMesh::createInstance (src/fixes/streaming/create_instance_guard.cpp): seven
	// register pushes then a lea, so the bytes a detour takes are whole
	// instructions and position-independent. Present in every build: the
	// fault it guards is a recorded crash. diagnostic = true: a mismatch
	// refuses the guard alone, and without it the site behaves as vanilla.
	HOOK_ROW(HOOK_NAVMESH_CREATE_INSTANCE, "createInstance", RVA_NAVMESH_CREATE_INSTANCE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24)

	// PhysicsActual::updateUT (src/fixes/physx/hull_queue_guard.cpp): `push rsi;
	// push r12; sub rsp,28h`, so the bytes a detour takes are whole
	// instructions and position-independent. The frame audit hooks the same
	// site when loaded, so it may pass as shared. Present in every build: the
	// fault it guards is a recorded crash. diagnostic = true: a mismatch
	// refuses the guard alone, and without it the flush runs as vanilla.
	HOOK_ROW(HOOK_PHYSICS_UPDATE_UT, "physicsUpdateUT", RVA_PHYSICS_UPDATE_UT, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x56,0x41,0x54,0x48,0x83,0xEC,0x28,0x48,0x8B,0xF1,0x48,0x81,0xC1,0xA0,0x01)
	// The five hull pushers, for the guard's caller attribution only: each
	// opens with the five-byte `mov [rsp+8],rbx`, one whole instruction; the
	// RIP-relative loads after it are not taken. The frame audit's HullDiag
	// hooks the same five. diagnostic = true: a mismatch loses a caller name.
	HOOK_ROW(HOOK_HULL_PUSH_HULL, "hullPushHull", RVA_HULL_PUSH_HULL, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0xA7,0x6E,0xC6)
	HOOK_ROW(HOOK_HULL_PUSH_ENTITY, "hullPushEntity", RVA_HULL_PUSH_ENTITY, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x51,0x20,0x48,0x8B)
	HOOK_ROW(HOOK_HULL_PUSH_SCYTHE, "hullPushScythe", RVA_HULL_PUSH_SCYTHE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48)
	HOOK_ROW(HOOK_HULL_PUSH_ROOT, "hullPushRoot", RVA_HULL_PUSH_ROOT, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0x07,0x70,0x95)
	HOOK_ROW(HOOK_HULL_PUSH_BASE, "hullPushBase", RVA_HULL_PUSH_BASE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0xA7,0x6F,0x95)

	// NavMeshGenerator::update (src/navmesh/scheduling/nm_adjacency.cpp): `mov rax,rsp` then
	// register pushes, so the bytes a detour takes are whole instructions and
	// position-independent. diagnostic = true, as for the guards above: a
	// mismatch refuses this detour alone, and without it no adjacency window
	// can close, so the exclusion only counts and says so in its heartbeat.
	HOOK_ROW(HOOK_NMG_UPDATE, "nmgUpdate", RVA_NMG_UPDATE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xD8)

#ifdef ZONEOPT_DEBUG
	// NavMesh::deleteInstance (src/fixes/stitch/unstitch_probe.cpp): five register
	// pushes then a lea, so the bytes a detour takes are whole instructions and
	// position-independent. DEV builds only -- a PROD build carries no row here
	// and no site to check. diagnostic = true: a mismatch refuses the probe
	// alone, and the probe only reads.
	HOOK_ROW(HOOK_NAVMESH_DELETE_INSTANCE, "deleteInstance", RVA_NAVMESH_DELETE_INSTANCE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_UNSTITCH_PROBE, HOOK_CAP_NONE,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x8D,0x6C,0x24,0xF0,0x48,0x81,0xEC)

	// The two section-key lookups (src/fixes/streaming/section_key_probe.cpp). Both
	// prologues are argument spills and register pushes, so the bytes a detour
	// takes are whole instructions and position-independent. DEV builds only.
	// diagnostic = true: a mismatch refuses that one site, and both only read.
	HOOK_ROW(HOOK_CLEARANCE_RESET_KEYS, "clearanceResetKeys", RVA_CLEARANCE_RESET_KEYS, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_SECTION_KEY_PROBE, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x18,0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x55)
	HOOK_ROW(HOOK_SECTION_CUT_LOOKUP, "sectionCutLookup", RVA_SECTION_CUT_LOOKUP, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_SECTION_KEY_PROBE, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x48,0x89,0x58,0x20,0x4C,0x89,0x40,0x18,0x89,0x50,0x10,0x48,0x89)

	// loadPhysXResource (src/diag/physx_pool_probe.cpp): `mov r11,rsp; push
	// rsi/rdi/r12; sub rsp,80h`, so the bytes a detour takes are whole
	// instructions. DEV builds only. RE_Kenshi detours this site before the
	// plugin loads, so the head here is normally its jump and the row passes
	// as shared on the untouched tail. diagnostic = true: a mismatch refuses
	// the counter alone, and the counter only counts.
	HOOK_ROW(HOOK_LOAD_PHYSX_RESOURCE, "loadPhysXResource", RVA_LOAD_PHYSX_RESOURCE, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x4C,0x8B,0xDC,0x56,0x57,0x41,0x54,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0xC7)
#endif

	// --- hook_manifest.cpp, island overlay ---
	HOOK_ROW(HOOK_ISINISLAND_IMPL, "isInIsland", RVA_ISINISLAND_IMPL, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x85,0xD2,0x74,0x0B,0x8B,0x42,0x20,0x39,0x41,0x20,0x75,0x03,0xB0,0x01,0xC3)
	HOOK_ROW(HOOK_GETISLAND_IMPL, "getIsland", RVA_GETISLAND_IMPL, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x57,0x48,0x83,0xEC,0x20,0x48)
	// --- hook_manifest.cpp, player cancel hooks ---
	// Bytes read from the IDB (ida MCP read_data/disassemble).
	// All three prologues are position-independent over the bytes a detour
	// takes (push rbx; sub rsp,20h / push rbx; push rbp; push r14 /
	// push rbx; push r12; push r14).
	// diagnostic = true: these only observe the player's cancels. A mismatch
	// refuses that hook alone; hook_manifest.cpp records which one installed
	// (IslandSetCancelHooksInstalled) so the deleted-order re-issue logic can turn
	// itself off instead of misreading a cancel it never saw.
	HOOK_ROW(HOOK_STOP_CHARACTERS_MOVEMENT, "stopCharactersMovement", RVA_STOP_CHARACTERS_MOVEMENT, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x83,0xB9,0x28,0x02,0x00,0x00,0x00,0x48,0x8B)
	HOOK_ROW(HOOK_ADD_JOB_SELECTED, "addJobSelected", RVA_ADD_JOB_SELECTED, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x48,0x8B,0xEA,0x33,0xD2,0x45)
	HOOK_ROW(HOOK_ADD_TASK_NEAREST, "addTaskNearest", RVA_ADD_TASK_NEAREST, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,0x4C)

	// --- hook_manifest.cpp, navmesh cache ---
	HOOK_ROW(HOOK_DISPATCH_JOB, "dispatchJob", RVA_DISPATCH_JOB, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_CACHING, HOOK_CAP_NONE,
	         0x48,0x89,0x6C,0x24,0x20,0x56,0x48,0x83,0xEC,0x30,0x48,0x8D,0xA9,0x88,0x00,0x00)

	// --- nm_lazy_hooks.cpp, installed on the first dispatch ---
	HOOK_ROW(HOOK_EDGE_PROCESS, "edgeProcess", RVA_EDGE_PROCESS, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x83,0xC1,0x50,0xE8,0x1F,0xF0)
	HOOK_ROW(HOOK_NM_RESULT_POPULATE, "nmResultPopulate", RVA_NM_RESULT_POPULATE, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x83,0xEC,0x68,0x4C,0x8B,0xD1,0x48,0x8D,0x4C,0x24,0x30,0xE8,0x1F,0xAF,0xFE)
	HOOK_ROW(HOOK_PROCESS_JOB_ALT, "processJobAlt", RVA_PROCESS_JOB_ALT, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D)
	// The two builders share their first 16 bytes; only the bodies differ.
	HOOK_ROW(HOOK_BUILD_COLLISION_IMPL, "buildCollision", RVA_BUILD_COLLISION_IMPL, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_WORKER_POOL,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24)
	HOOK_ROW(HOOK_BUILD_COLLISION_INTERIOR_IMPL, "buildCollisionInterior", RVA_BUILD_COLLISION_INTERIOR_IMPL, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24)
	// The narrow collision-build lock. diagnostic = true: a mismatch keeps the
	// wide cover (buildCollisionCS), which is always safe.
	HOOK_ROW(HOOK_NMG_STITCH_UNLOADED_ZONE, "stitchUnloadedZone", RVA_NMG_STITCH_UNLOADED_ZONE, HOOK_DIAGNOSTIC, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C)
	HOOK_ROW(HOOK_NMG_STITCH_WITH_INTERIORS, "stitchWithInteriors", RVA_NMG_STITCH_WITH_INTERIORS, HOOK_DIAGNOSTIC, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x56,0x41,0x54,0x41,0x55,0x48)
	HOOK_ROW(HOOK_NMG_LOCK_ZONE, "lockZone", RVA_NMG_LOCK_ZONE, HOOK_DIAGNOSTIC, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x83,0xEC,0x28,0x45,0x84,0xC0,0x74,0x20,0x48,0x8D,0x4C,0x24,0x48,0xE8,0x73)
	// `test rcx, rcx` + `jz` — the rel8 branch sits inside the 5 bytes MinHook
	// takes, so its trampoline relocates the branch. The gate only compares
	// bytes, so that changes nothing here, but it is the one site where the
	// trampoline does more than copy.
	HOOK_ROW(HOOK_NAVMESH_STOP, "navMeshStop", RVA_NAVMESH_STOP, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_WORKER_POOL,
	         0x48,0x85,0xC9,0x74,0x46,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xC6,0x81,0xC8)
	// NavMeshGenerator::getSeedPointsFromAdjacentZone. Bytes
	// read from the IDB (ida MCP disassemble); the same 16 bytes as
	// processJobAlt's row (both open with the same register saves).
	// diagnostic = true: a mismatch refuses this hook alone, which turns the
	// neighbour-seed feature off (instrumentation and stand-in) and reverts the
	// L2 settings hash to the step-0 value; the plugin installs normally.
	HOOK_ROW(HOOK_NMG_GET_SEED_POINTS_ADJ, "getSeedPointsAdjacent", RVA_NMG_GET_SEED_POINTS_ADJ, HOOK_DIAGNOSTIC, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D)

	// --- hook_manifest.cpp, pathfinding ---
	HOOK_ROW(HOOK_CS_FIND_PATH, "csFindPath", RVA_CS_FIND_PATH, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATHFIND_DIAG, HOOK_CAP_NONE,
	         0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x8D,0x6C,0x24,0x98,0x48,0x81)
	HOOK_ROW(HOOK_CS_CHECK_FACE_CONN, "csCheckFaceConn", RVA_CS_CHECK_FACE_CONN, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATHFIND_DIAG, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xF0,0x00,0x00,0x00,0x48)
	HOOK_ROW(HOOK_FIND_PATH_FULL, "findPathFull", RVA_FIND_PATH_FULL, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_FIND_PATH_FULL, HOOK_CAP_NONE,
	         0x48,0x89,0x4C,0x24,0x08,0x55,0x56,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8D,0xAC)
	HOOK_ROW(HOOK_REQUEST_PATH, "requestPath", RVA_REQUEST_PATH, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATHFIND_DIAG, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57)
	HOOK_ROW(HOOK_PATH_REQ_SUBMIT, "pathReqSubmit", RVA_PATH_REQ_SUBMIT, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATHFIND_DIAG, HOOK_CAP_NONE,
	         0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24)
	HOOK_ROW(HOOK_CS_FIND_PATH_FALLBACK, "csFindPathFallback", RVA_CS_FIND_PATH_FALLBACK, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATHFIND_DIAG, HOOK_CAP_NONE,
	         0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x8D,0xAC,0x24,0x60)
	HOOK_ROW(HOOK_CONTENT_STREAM_CALLEE_0X8869, "contentStreamCallee_0x8869", RVA_CONTENT_STREAM_CALLEE_0X8869, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_PATH_EXTRACT, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x53,0x55,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x70,0x0F,0x57)
	HOOK_ROW(HOOK_ADD_INSTANCE, "addInstance", RVA_ADD_INSTANCE, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ADD_INSTANCE, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57)

	// --- hook_manifest.cpp, path-worker-pool instrumentation ---
	// Bytes read from the IDB (ida MCP disassemble).
	// diagnostic = true: these four are the PathPool
	// instrumentation only, not a behaviour-affecting hook. A mismatch here
	// must turn PathPool off, not refuse the whole plugin -- each install
	// site (hook_manifest.cpp) re-checks its own row via VerifyPrologueByRva anyway,
	// so this row only decides whether a mismatch is fatal to the plugin.
	HOOK_ROW(HOOK_CONTENT_STREAM, "contentStream", RVA_CONTENT_STREAM, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0xB8)
	HOOK_ROW(HOOK_DEQUEUE_WORK, "dequeueWork", RVA_DEQUEUE_WORK, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48)
	HOOK_ROW(HOOK_ENQUEUE_THREAD_SAFE, "enqueueThreadSafe", RVA_ENQUEUE_THREAD_SAFE, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xB9,0x10,0x00)
	HOOK_ROW(HOOK_GATES_UPDATE_CODES, "gatesUpdateCodes", RVA_GATES_UPDATE_CODES, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_GATE_PASS, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xA8)
	HOOK_ROW(HOOK_GATES_FIND_PATH, "gatesFindPath", RVA_GATES_FIND_PATH, HOOK_DIAGNOSTIC, HOOK_BY_STARTUP, HOOK_WANT_GATE_PASS, HOOK_CAP_NONE,
	         0x40,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF)

	// --- render levers (src/render/) ---
	// diagnostic = true: a mismatch refuses that lever alone.
	HOOK_ROW(HOOK_WATER_OCCLUSION_LISTENER, "WaterOcclusionListener::frameRenderingQueued", RVA_WATER_OCCLUSION_LISTENER, HOOK_DIAGNOSTIC, HOOK_BY_RENDER, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x53,0x48,0x83,0xEC,0x20,0xC6,0x41,0x28,0x00,0x8B,0x05,0x70,0x94,0x75,0x01)
	HOOK_ROW(HOOK_CSM_SETUP_CASCADE, "CsmShadowMap::setupCascade", RVA_CSM_SETUP_CASCADE, HOOK_DIAGNOSTIC, HOOK_BY_RENDER, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48)
	HOOK_ROW(HOOK_PAGED_GEOMETRY_UPDATE, "PagedGeometry::update", RVA_PAGED_GEOMETRY_UPDATE, HOOK_DIAGNOSTIC, HOOK_BY_RENDER, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x89,0x4C,0x24,0x08,0x48,0x81,0xEC,0x28,0x01,0x00,0x00,0x48,0x8B,0x84,0x24)

	// --- settings panel (src/gui/) ---
	// diagnostic = true: a mismatch turns the panel off, not the plugin.
	HOOK_ROW(HOOK_OPTIONS_CREATE, "OptionsWindow::create", RVA_OPTIONS_CREATE, HOOK_DIAGNOSTIC, HOOK_BY_GUI, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24,0x60,0x49)
	HOOK_ROW(HOOK_OPTIONS_SAVE_OPTIONS, "OptionsWindow::saveOptions", RVA_OPTIONS_SAVE_OPTIONS, HOOK_DIAGNOSTIC, HOOK_BY_GUI, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x48,0x8D,0x68,0xC8,0x48,0x81,0xEC,0x20)
