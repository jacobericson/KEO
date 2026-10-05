// zone_config.h - the zone module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"
#include "zone/geometry/zone_geometry_cert.h"



namespace zone {

// Starts as a copy of kZoneDefaults, then written by LoadConfig on the main
// thread before any hook installs. Two main-thread writers later only clear
// flags: CheckBuildGate (plugin_entry.cpp), when the build gate fails and no
// hook installs, clears deferralEnabled, preloadEnabled and
// movementAwareEnabled; InstallHooks (hook_manifest.cpp), with earlier hooks
// already live, clears preloadEnabled when the isContentPending hook fails.
// Read on any thread; a hook running during that clear reads the old or the
// new value of one bool.
struct ZoneConfig
{
	bool deferralEnabled;

	bool preloadEnabled;

	bool movementAwareEnabled;

	// saveLoadUnload: the save-load crash fix (preload_saveload.cpp,
	// hook_resetUnloadZones). At the game's save-load reset, after it unloads the
	// zones in Set A and Set B, unload every zone still holding a content (the
	// mod's, which are in neither set) and clear the mod's state there. On by
	// default in every build, PROD included. false = today's behaviour for
	// bisection: nothing unloaded and the state clear left to the ZM+8 edge; the
	// survivors are still counted and logged ("would unload").
	bool saveLoadUnloadEnabled;

	// escapePauseGuard: keep the escape menu's pause through a loader unpause
	// (zone_pause.cpp). On by default; false restores the vanilla clobber where a
	// finishing transition can resume the game behind an open escape menu.
	bool escapePauseGuardEnabled;

	// townGuard: refuse a town's coverage refresh that carries a nonpositive
	// timer (zone/handoff/zone_lifecycle_hooks.cpp, ZONEHAND_STEP >= 2). Such a refresh means
	// no cell in the coverage is leased, and the engine answers it with a fixed
	// default that renews the whole coverage from nothing. On by default; false
	// restores the vanilla behaviour for an A/B.
	bool townGuardEnabled;

	// zoneRetention: hold a cell the game took over from the mod past its native
	// expiry, for as long as the retention policy says (zone_retention.cpp,
	// ZONEHAND_STEP >= 3). On by default; false is the A/B control: every expiry
	// is the engine's, held only while a navmesh job still works on the cell
	// (zone_expiry_guard.cpp).
	bool zoneRetentionEnabled;

	// islandReadinessRule: per-caller readiness rule in hook_isContentPending.
	// Off by default; read by hook_isContentPending (readiness_hook.cpp).
	bool islandReadinessRuleEnabled;

	// readinessOverrides: false turns off the isContentPending deferral's
	// override. On by default; read by readiness_hook.cpp.
	bool readinessOverridesEnabled;

	// zoneLifeUnload: the mod unloads the zones it loaded once they are idle and
	// outside the retain radius. On by default.
	bool zoneLifeUnloadEnabled;

	// zoneCycleStats: at ZONEHAND_STEP >= 1, the per-loading-cycle measurement
	// (ZoneCycle: / ZoneCycleSum:), the private-lease sample (ZonePriv:) and the
	// ZoneLeak: setBsz= token. DEV default on, PROD default off; false makes the
	// measurement a no-op. Read on every sample.
	bool zoneCycleStatsEnabled;

	// zoneWedgeGuard: at ZONEHAND_STEP >= 1, the one-shot ZoneWedge: report when
	// a loading cycle dwells in one loadingPhase past ZC_PHASE_WEDGE_THRESHOLD_MS
	// (a permanent hang, not a slow load). On by default in DEV and PROD alike,
	// independent of zoneCycleStats: it fires only in an already-broken state and
	// a beta user's report is worthless if the build that hit it had it off.
	// Read on every sample.
	bool zoneWedgeGuardEnabled;

	// zoneGeometryMode: which geometry contract a prepared cell is adopted
	// under. `contentOnly` is the only accepted value: it asserts nothing about
	// any mesh, and the certificate machinery at the L1 store point counts its
	// verdicts without acting on them. The other mode needs verified coverage of
	// the engine's own geometry producers and is fenced at compile time as well
	// as refused here (zone_geometry_cert.h). Read at the store point and on the
	// periodic report.
	ZoneGeometryMode zoneGeometryMode;

	// Camera focus (camera_focus.h/camera_zone_hook.cpp): use the camera's orbit/follow
	// anchor instead of its own position for zone prediction, so a zoomed-out,
	// rearward-pitched camera still preloads the zone the squad is entering.
	bool   cfg_camFocusEnabled;       // false reverts to the raw camera position

	// Zone loading
	// loadSingleZone's 4th argument (xmm3) for the zones the mod preloads. The game
	// writes it to zoneEntry + 4*(timerIndex + 48) and it decides when that zone is
	// unloaded again. 0 takes the game's own per-timer default, which is what
	// processState2 passes; a positive value overrides it. Exists so this can be
	// A/B'd (0 vs 3600) without a rebuild.
	float  cfg_preloadKeepAliveSeconds;

	// Hook orchestration
	double cfg_camLogInterval;        // debug camera log interval

	float  cfg_camFocusMaxDist;       // soft cap from the nearest squad member; <= 0 = one zone width

	float  cfg_camFocusHardMult;      // hard reject beyond maxDist * this (no clamp, just fallback)

	// Hysteresis band (world units) for the prediction axis check in camera_zone_hook.cpp:
	// once a neighbour is predicted on an axis, that axis must fall back below
	// PRELOAD_THRESHOLD - this before releasing it; a new neighbour needs
	// PRELOAD_THRESHOLD + this to commit. Default 250 = 10% of the 2500-unit
	// threshold: enough to absorb ordinary jitter at the border without
	// meaningfully delaying a real crossing (zones are ~8192 units wide).
	float  cfg_camFocusHysteresis;

	// Zone lifecycle
	int    cfg_zoneLifeRetainRadius;  // Chebyshev radius (zones) around camera/player chars
	                                  // inside which a mod-loaded zone is never unloaded (1-4).
	                                  // It also sets the radius of the shared proximity map,
	                                  // which retention reads as its cheap filter, so raising
	                                  // it makes adopted cells stickier as well.

	double cfg_zoneLifeIdleSeconds;   // seconds a mod-loaded zone must sit outside the
	                                  // retain radius before it is unloaded (5-600)
};

extern ZoneConfig g_zoneCfg;
extern const ZoneConfig kZoneDefaults;
extern const ConfigKey g_zoneConfigKeys[];

} // namespace zone
