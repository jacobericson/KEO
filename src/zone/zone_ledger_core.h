#pragma once
// Shared identity and state-machine core for the zone lifecycle ledgers
// (zone_prep_ledger.h, zone_nav_ledger.h, zone_retention_ledger.h). No game
// or KenshiLib headers, no allocation: every ledger built on this indexes a
// fixed 64x64 grid the way the engine indexes AreaSector, so a cell lookup
// never allocates. Threading contracts are documented per ledger, not here;
// this file is identity comparison and state-transition arithmetic only.

const int ZONE_GRID_DIM = 64;                               // cell coordinate range: 0..63 per axis
const int ZONE_GRID_CELLS = ZONE_GRID_DIM * ZONE_GRID_DIM;   // 4096

// A prepared or adopted cell's identity. Coordinates and reused pointers do
// not distinguish two attempts at the same cell: `worldEpoch` changes across
// world replacement (save-load, import, new game, quit); `contentIncarnation`
// changes only when content already torn down (fully `Unloaded`) is rebuilt
// from a fresh `PrivateShell` in the same world. A navmesh redo that retains
// content (geometry invalidation) does not bump either counter; it is
// tracked by its own generation counter in the nav ledger.
struct ZoneIdentity
{
	unsigned      worldEpoch;
	unsigned char cellX;
	unsigned char cellY;
	unsigned      contentIncarnation;
};

bool ZoneIdentityEqual(const ZoneIdentity& a, const ZoneIdentity& b);

// Same cell, same world, ignoring incarnation: true when `b` is some attempt
// (old or current) at the cell `a` names.
bool ZoneIdentitySameCell(const ZoneIdentity& a, const ZoneIdentity& b);

// `candidate` is a stale attempt at the cell `current` names: same world and
// cell, strictly older incarnation. False for a different cell or world, and
// false when the incarnations match (that is the current attempt, not stale).
bool ZoneIdentityIsStaleAttempt(const ZoneIdentity& current, const ZoneIdentity& candidate);

// Row-major index matching SectionManager::registerZoneCenter's AreaSector
// layout (y + (x << 6)). Coordinates outside 0..63 clamp; a caller passing
// engine-derived coordinates never sees a clamp.
int ZoneCellIndex(int cellX, int cellY);

enum ZoneLifecycleState
{
	ZONE_STATE_UNLOADED = 0,
	ZONE_STATE_PRIVATE_SHELL,
	ZONE_STATE_PRIVATE_CONTENT,
	ZONE_STATE_PRIVATE_NAV,
	ZONE_STATE_READY_FOR_ADOPTION,
	ZONE_STATE_NATIVE_A,
	ZONE_STATE_NATIVE_B_LOADING,
	ZONE_STATE_NATIVE_ACTIVE,
	ZONE_STATE_RETIRING,
	ZONE_STATE_GEOMETRY_INVALIDATED,
	ZONE_STATE_COUNT
};

const char* ZoneLifecycleStateName(int state);

// Whether the state machine allows `from` -> `to` directly.
//
// Ordinary progress is one step: PrivateShell -> PrivateContent -> PrivateNav
// -> ReadyForAdoption -> NativeA -> NativeBLoading -> NativeActive ->
// Retiring -> Unloaded -> PrivateShell (a new incarnation).
//
// Real-demand takeover (any private stage can yield to native demand)
// legalizes PrivateShell/PrivateContent/PrivateNav/ReadyForAdoption -> NativeA
// directly. The same PrivateContent -> NativeA edge also carries the
// content-only fallback (design section 5: hand off before navmesh
// registration in a region whose geometry-change coverage is unproven).
// Mechanically these are the same transition; nothing here distinguishes
// "pre-empted by real demand" from "never attempted generation on purpose"
// because the state machine does not need to. A caller that must tell the
// two apart (for logging, or to decide whether to retry generation later)
// has to track the reason itself alongside this ledger.
//
// GeometryInvalidated is a recovery state reachable from PrivateNav or
// ReadyForAdoption (a mesh attempt already installed or in flight). It
// returns to PrivateContent — content is retained, only the mesh attempt is
// discarded — or, when native demand adopts mid-recovery once removal is
// acknowledged, straight to NativeA.
//
// Every private stage may also go straight to Retiring: the mod can tear
// down a cell it prepared and never handed over (a guard refusal, its own
// unload pass, a first-time cell given back to the game), and Retiring ->
// Unloaded is the only route to Release, so without this edge such a cell's
// slot could never be reused.
//
// Nothing legalizes a step backward out of native ownership (NativeA,
// NativeBLoading, NativeActive) into a private stage: a cell that leaves
// native ownership does so through Retiring -> Unloaded and starts its next
// attempt as a new incarnation.
bool ZoneLifecycleCanTransition(int from, int to);

// Per-cell classification a non-owning reader may consult from any thread
// with no lock: written only by the main thread as a single aligned store of
// a plain int (never a struct, never multiple fields), so a reader on
// another thread sees either this frame's value or last frame's, never a
// torn one. This is `hook_isContentPending`'s classifier (main + AI + spawn
// threads all read it): a private cell hears the truthful readiness answer,
// an adopted cell is ready by construction, and a cell the mod never reached
// (still NONE) keeps today's bypass. There is no separate "game-owned"
// class: a writer for one is possible (the native activation detour already
// resolves gx/gy and reads +176/+177 for every activation, not only
// mod-held cells), but it would be behaviourally inert, since NONE and a
// hypothetical game-owned class would answer identically -- today's bypass
// -- so a writer would only add a ledger publish on the hottest detour for
// no change in outcome. The `justLoadedAGame` bypass is global and carried
// separately (see ZonePrepLedger::globalGameOwnedBypass).
enum ZoneReadinessClass
{
	ZONE_CLASS_NONE = 0,     // never touched, or fully retired: keeps today's bypass
	ZONE_CLASS_PRIVATE,      // mod-owned, not in Set A/B: hears the truth
	ZONE_CLASS_ADOPTED       // ready by construction: hears the truth
};
