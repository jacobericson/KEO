// fixes_config.h - the fixes module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"



namespace fixes {

// Starts as a copy of kFixesDefaults, then written by LoadConfig on the main
// thread before any hook installs; read-only afterwards, on any thread.
struct FixesConfig
{
	// pathExtractGuard: wrap the path-result extraction loop so a fault inside it
	// rolls the result back to "no new edges" instead of taking the process down.
	// Every rescue is counted, the first is logged. false = hook not installed.
	bool pathExtractGuardEnabled;

	// sectionStamp: hook every streaming-collection insertion and count it, so a
	// session log says whether the section table is taking inserts at all.
	// false = hook not installed.
	bool sectionStampEnabled;

	// navMeshUpdateGuard: wrap the navmesh/path thread's NavMesh::update pass in
	// SEH. A fault inside it kills the thread while it still holds the
	// SectionManager's changeMutex exclusively, and every navigability query
	// try-locks that mutex and gives up the instant the try fails -- so no
	// character can be ordered to move again for the rest of the session. The
	// guard records that fault and the lock state, and leaves the lock held: the
	// fault means the navmesh data is already damaged, and the lock is what keeps
	// the next reader out of it. RE_Kenshi's crash handler then offers an
	// emergency save and ends the process either way. It installs no hook; false
	// makes the existing wrapper a plain pass-through.
	bool navMeshUpdateGuardEnabled;

	// destroyListDiag: install the pass-through hook on GameWorld::destroyListOE's
	// sole inserter (the destroyListOE diagnostic, destroy_list_defer.h). It only records the calling
	// thread, but it is still a 5-byte patch into a hot engine function, so PROD
	// leaves it off and DEV turns it on. The invariant probe itself is always on in
	// every build and is not gated by this key.
	bool destroyListDiagEnabled;

	// destroyListDefer: the destroyListOE mitigation (destroy_list_defer.h). Off-main-thread inserts
	// into GameWorld::destroyListOE are queued and replayed on the main thread, so
	// the unsynchronised container has a single writer. On by default in every
	// build; setting it false leaves the hook installed as the plain diagnostic,
	// which is the A/B control. The hook is installed when either this or
	// destroyListDiag is on.
	bool destroyListDeferEnabled;

	// unstitchGuard: the bounds test the cross-section un-stitch never makes
	// (src/fixes/stitch/unstitch_guard.h). On by default and present in every build: the
	// fault it prevents is a measured, twice-reproduced crash, and with it on a
	// teardown whose records are all in bounds runs the game's own walk untouched.
	// Read when the guard installs, and never again.
	bool unstitchGuardEnabled;

	// stitchSourceLines: how many dropped sets the stitch-source instrument
	// (src/fixes/stitch/stitch_source.h) describes in the log, two lines each. Verbosity
	// only: the write-side record, the classification counters and the heartbeat
	// exist in every build whatever this says. 0 keeps the counters and prints no
	// per-drop lines. Read at startup.
	int cfg_stitchSourceLines;

	// graphVisitorGuard: the absent-instance test the A* heuristic never makes
	// (src/fixes/search/graph_visitor_guard.h). On by default and present in every build:
	// the fault it prevents is a measured mid-session crash, and with it on a
	// search whose sections all have their graph instances runs the game's own
	// code untouched. Read when the guard installs, and never again.
	bool graphVisitorGuardEnabled;

	// graphExpandGuard: the same absent-instance test one cache slot over, on the
	// A* step that expands a popped node (src/fixes/search/graph_expand_guard.h). On by
	// default and present in every build, for the reason above: the node-cost
	// guard keeps such a node in the search, so the expansion is where it arrives
	// next. Read when the guard installs, and never again.
	bool graphExpandGuardEnabled;

	// graphPositionGuard: the same absent-instance test on the cluster-graph
	// search's own node-position helper (src/fixes/search/graph_position_guard.h) --
	// reached only when clusterGraphBypass actually consults the graph (measure,
	// player or off), a call path neither guard above sits on. The detour
	// installs in every build regardless of this key; the key chooses only
	// whether a call that meets an absent instance is substituted (true, the
	// default) or merely classified and counted before the original runs
	// unchanged (false, an observe mode that keeps the fault's own base rate
	// visible in a control). Read when the guard installs, and never again.
	bool graphPositionGuardEnabled;

	// meshFaceGuard: the face-index and edge-run tests the per-face AABB step
	// never makes (src/fixes/streaming/mesh_face_guard.h). On by default and present in
	// every build: the fault it prevents is a measured crash recorded three times
	// at one instruction, and with it on a face whose edge run lands inside its
	// instance runs the game's own walk untouched. Read when the guard installs,
	// and never again.
	bool meshFaceGuardEnabled;

	// createInstanceGuard: skips a NavMesh::createInstance call whose NavInstance
	// is already queued in addList by pointer with a live instance
	// (src/fixes/streaming/create_instance_guard.h); vanilla frees that object and queues
	// the freed pointer. The detour installs in every build regardless of this
	// key; the key chooses only whether such a call is skipped (true, the
	// default) or counted and passed to the original unchanged (false, observe).
	// Read when the guard installs, and never again.
	bool createInstanceGuardEnabled;

	// hullDoublePushGuard: drops a second queueing of one object on
	// PhysicsInterface::hullsToDestroy before PhysicsActual::updateUT flushes it
	// to the physics thread (src/fixes/physx/hull_queue_guard.h); vanilla deletes such
	// an object twice, the second time through freed memory. The detour installs
	// in every build regardless of this key; the key chooses only whether a
	// duplicate is dropped (true, the default) or counted and left in place
	// (false, observe). Read when the guard installs, and never again.
	bool hullDoublePushGuardEnabled;

	// stitchByteGuard: skips the one-byte `+0x50` store NavMeshGenerator::update
	// makes on the output of an interior stitch task, which lands one byte past
	// the 0x48-byte NavInstance on the next heap block
	// (src/fixes/stitch/stitch_byte_guard.h). The patch installs in every build whenever
	// its bytes verify and no path thread exists yet; the key chooses only
	// whether that store is skipped (true, the default) or kept and recorded
	// (false, observe). Read when the guard installs, and never again.
	bool stitchByteGuardEnabled;

	// navMeshLife: the section lifecycle rows at streaming-collection insert and
	// removal (src/fixes/streaming/navmesh_life.h). On by default in DEV and PROD alike, and
	// deliberately independent of every preload and pathfinding key: its counts
	// are only worth anything when the same build can be run with those keys on
	// and off and the two logs compared. Read at both sites.
	bool navMeshLifeEnabled;

	// unstitchProbe: the read-only detour on NavMesh::deleteInstance
	// (src/fixes/stitch/unstitch_probe.h). Off by default. The flag and key
	// compile in both builds; PROD reads the key and ignores it. Only the probe
	// that reads this flag is compiled under ZONEOPT_DEBUG, so a PROD session
	// cannot install it. Read when the probe installs and on every teardown; it
	// decides only whether the probe looks, never what the game does.
	bool unstitchProbeEnabled;

	// sectionKeyProbe: the read-only capture of the section-table lookups the
	// world step makes from a packed key (src/fixes/streaming/section_key_probe.h).
	// On by default. The flag and key compile in both builds; PROD reads the key
	// and ignores it. Only the probe that reads this flag, both its detours, is
	// compiled under ZONEOPT_DEBUG. On rather than off because what it records is
	// only worth anything if it is already running when a fault arrives, and a
	// session nobody remembered to arm records nothing. Read when the probe
	// installs, and never again.
	bool sectionKeyProbeEnabled;

	// physPurecallRecord: a forensic-only recorder on PhysXCore64.dll's own
	// pure-virtual-call handler slot (src/fixes/physx/purecall_record.h). A pure
	// virtual call there is a CRT abort, not an SEH exception -- the mod's crash
	// handler cannot see it -- so this instead captures the faulting thread, the
	// real call site and a timestamp into purecall_dump.txt just before abort()
	// runs. On by default in every build; it writes nothing (data write, not a
	// hook) unless PhysXCore64.dll's own bytes match this build's signature.
	bool physPurecallRecordEnabled;

	// physQueryGuard: the point-of-use guard on the result walk inside
	// GameWorld::getObjectsWithinBox (src/fixes/physx/physx_query_guard.h). The scene's
	// overlap query can hand back a shape whose block is already free; calling
	// getActor() through it lands in PhysX's _purecall, which aborts the process
	// with no record. The guard validates each entry immediately before that call
	// and skips a bad one. On by default in every build; false skips the install
	// entirely and leaves vanilla behaviour. Read once, at startup.
	bool physQueryGuardEnabled;

	// corpsePin: ActivePlatoon::calculateCurrentPos detour (src/fixes/world/corpse_pin.h).
	// A squad whose only member is a carried NPC corpse otherwise stays pinned at
	// the pickup spot forever; this repositions
	// it to the carrier instead. On by default; false skips the install
	// entirely, a true no-op (the build gate still verifies the site's row at
	// startup either way, like every other row). Read once, at hook install.
	bool corpsePinEnabled;

	// nestValidationGuard: at ZONEHAND_STEP >= 2, the finalizeZoneResources
	// detour (src/fixes/world/nest_validation.h). On by default; false is a true
	// no-op -- the original runs exactly as before the guard existed. Read on
	// every call.
	bool nestValidationGuardEnabled;

	// graphHeuristicGuard: installs the three guards inside the A* search's hierarchical
	// heuristic (graph_heuristic_guard.cpp); 0 by default. playerHierarchical other than off
	// installs them too. Read at startup only.
	int graphHeuristicGuardOn;
};

extern FixesConfig g_fixesCfg;
extern const FixesConfig kFixesDefaults;
extern const ConfigKey g_fixesConfigKeys[];

} // namespace fixes
