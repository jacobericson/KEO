// hook_manifest_policy.h — the manifest's row columns and the want predicate.
// Variant-independent and host-safe: no game or Windows header.

#ifndef KENSHI_ZONE_OPT_HOOK_MANIFEST_POLICY_H
#define KENSHI_ZONE_OPT_HOOK_MANIFEST_POLICY_H

// A fatal row's mismatch refuses the plugin; a diagnostic row's refuses that
// site alone.
enum HookRowKind { HOOK_FATAL, HOOK_DIAGNOSTIC };

// Who installs the row: startPlugin itself, a module's Install function it
// calls, the first navmesh dispatch, the render levers or the settings panel.
enum HookInstaller { HOOK_BY_STARTUP, HOOK_BY_MODULE, HOOK_BY_LAZY, HOOK_BY_RENDER, HOOK_BY_GUI };

// When a row counts toward the banner's hook total. A row whose want holds
// adds exactly one; HOOK_WANT_UNCOUNTED rows are installed by paths that keep
// their own count.
enum HookWant
{
	HOOK_WANT_ALWAYS,
	HOOK_WANT_MOVEMENT_AWARE,     // movementAware
	HOOK_WANT_DESTROY_LIST,       // destroyListDiag or destroyListDefer
	HOOK_WANT_CACHING,            // caching
	HOOK_WANT_PATHFIND_DIAG,      // pathfindDiag
	HOOK_WANT_PATH_EXTRACT,       // pathfindDiag and pathExtractGuard
	HOOK_WANT_ADD_INSTANCE,       // pathfindDiag and sectionStamp, or navMeshLife
	HOOK_WANT_GATE_PASS,          // gatePassDiag
	HOOK_WANT_ESCAPE_PAUSE,       // escapePauseGuard
	HOOK_WANT_CORPSE_PIN,         // corpsePin
	HOOK_WANT_NEST_GUARD,         // nestValidationGuard
	HOOK_WANT_UNSTITCH_GUARD,     // unstitchGuard
	HOOK_WANT_GRAPH_VISITOR,      // graphVisitorGuard
	HOOK_WANT_GRAPH_EXPAND,       // graphExpandGuard
	HOOK_WANT_MESH_FACE,          // meshFaceGuard
	HOOK_WANT_NAVMESH_LIFE,       // navMeshLife
	HOOK_WANT_UNSTITCH_PROBE,     // unstitchProbe
	HOOK_WANT_SECTION_KEY_PROBE,  // sectionKeyProbe
	HOOK_WANT_GRAPH_HEURISTIC,    // graphHeuristicGuard or playerHierarchical != off
	HOOK_WANT_FIND_PATH_FULL,     // pathfindDiag or playerHierarchical != off
	HOOK_WANT_CLUSTER_CROSS_COST, // clusterCrossCost or plannerMode != off
	HOOK_WANT_UNCOUNTED
};

// The worker pool starts only when every row carrying this bit installed.
enum HookCapability { HOOK_CAP_NONE = 0, HOOK_CAP_WORKER_POOL = 1 };

// One field per INI key a want reads.
struct HookWantInputs
{
	bool destroyListDiag;
	bool destroyListDefer;
	bool escapePauseGuard;
	bool corpsePin;
	bool nestValidationGuard;
	bool unstitchGuard;
	bool graphVisitorGuard;
	bool graphExpandGuard;
	bool meshFaceGuard;
	bool navMeshLife;
	bool unstitchProbe;
	bool sectionKeyProbe;
	bool movementAware;
	bool caching;
	bool pathfindDiag;
	bool pathExtractGuard;
	bool sectionStamp;
	bool gatePassDiag;
	bool graphHeuristicGuard;
	bool playerHierarchical;  // playerHierarchical != off
	bool clusterCrossCost;
	bool planner;  // plannerMode != off
};

bool HookWantEval(HookWant want, const HookWantInputs& in);

// The config globals as they stand now. The two DEV-only keys read false in a
// PROD build, which carries neither the key nor the site.
HookWantInputs HookWantInputsFromConfig();

// Whether an install may patch its row. A row the caller re-verifies is
// admitted exactly when that check passes, whatever the gate said; any other
// row only when the startup gate passed it.
enum HookAdmit { HOOK_ADMIT = 0, HOOK_REFUSE_PROLOGUE };
HookAdmit HookInstallAdmit(bool reverify, bool reverifyOk, bool gatePassed);

#endif
