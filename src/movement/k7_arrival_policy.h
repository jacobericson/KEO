#ifndef KENSHI_ZONE_OPT_K7_ARRIVAL_POLICY_H
#define KENSHI_ZONE_OPT_K7_ARRIVAL_POLICY_H

// Destination-mesh-arrives-late stops: arms a re-issue wait on
// the rising edge of a tracked order's own end signature, far from its
// destination, while that destination's outdoor navmesh instance is not yet
// in the world or only just arrived, then fires the moment the instance is
// confirmed in the world and the character is confirmed still stopped --
// instead of waiting for K7TryDeletedReissue's own STOPPED_HYSTERESIS wait
// (delRefuse=h) and its "deleted" precondition. Pure arithmetic: no game
// headers, so it is host-tested directly (tools/tests/k7_arrival_units.cpp).
//
// k7_observe.cpp and k7_reissue.cpp are the only callers. They own every game read (position,
// character state, ClassifyZoneReadiness); this file only orders the numbers
// and booleans it hands in.

// Minimum distance-squared from the destination for an end signature to be
// worth arming a wait for (a short stop right at the goal is not this
// failure class). 1,000 units, squared.
extern const float K7_ARRIVAL_MIN_DIST_SQ;

// Maximum seconds an armed wait may run before it gives up, so a cell that
// never gets an instance -- or a fire condition some other gate keeps
// refusing -- cannot strand the entry forever.
extern const double K7_ARRIVAL_MAX_WAIT;

// How recently the destination cell must have read not-in-world, at the
// moment a signature fires while the cell now reads in-world, for that
// signature to still count as "this stop is the class-7 shape" (the leg was
// requested just before the cell streamed in, walked, and stopped just after
// it did).
extern const double K7_ARRIVAL_RECENT_TRANSITION;

// destCellClass is a ZR_* value from zone_readiness_classify.h, passed as a
// plain int so this file needs no game header. The caller always passes
// splitMap=false to ClassifyZoneReadiness, so the only values seen here are
// ZR_BUILDINGS_PENDING (2, the outdoor instance is in the world),
// ZR_NOT_IN_WORLD (3) and ZR_UNKNOWN (4).
enum
{
	K7_ARRIVAL_ZR_BUILDINGS_PENDING = 2,
	K7_ARRIVAL_ZR_NOT_IN_WORLD      = 3
};

// Arm condition: an end signature (reached or failed) just fired at dDestSq
// from the destination. True when destCellClass reads not-in-world right
// now, OR it reads in-world but was seen not-in-world within the last
// K7_ARRIVAL_RECENT_TRANSITION seconds (secsSinceNotIn, negative = never
// observed not-in-world this episode -- that branch is then never taken).
//
// No cell-distance bound (e.g. "the character's cell within 1 cell of the
// destination cell") is layered on top of dDestSq/secsSinceNotIn. A plain
// distance bound of 1.5 cells (6,912 world units) misses one episode outright
// (7.6k units), and a stop-cell Chebyshev-1 radius would cover the two
// episodes whose stop cell is known -- but a third episode's stop cell was
// never logged, so a radius bound cannot be shown to cover it either. The
// class-7 arm line (`span=`, k7_observe.cpp) settles this in the field
// instead of guessing a radius from incomplete data.
bool K7ArrivalShouldArm(float dDestSq, int destCellClass, double secsSinceNotIn);

// Arm on the signal's rising edge only (prevSig false, sigNow true), never
// while a formation is still gathering (its own gather-arrival move ends with
// exactly this signature), never while the game is paused (nothing is
// simulating, so a "stop" observed now proves nothing), and never re-arming
// an already-armed wait.
bool K7ArrivalArmEdge(bool prevSig, bool sigNow, bool gathering, bool paused, float dDestSq,
                      int destCellClass, double secsSinceNotIn);

enum K7ArrivalOutcome
{
	K7_ARRIVAL_WAIT = 0,   // keep waiting
	K7_ARRIVAL_FIRE,       // the destination cell is in the world: check the fire gate
	K7_ARRIVAL_EXPIRE      // maxWaitSec passed with no arrival: give up
};

// Per-poll outcome for an armed wait. FIRE is checked before EXPIRE, so a
// cell that arrives on the very poll the timer would have expired still
// fires rather than expiring.
K7ArrivalOutcome K7ArrivalPoll(int destCellClass, double waitedSec, double maxWaitSec);

enum K7ArrivalAction
{
	K7_ARR_NONE = 0,       // held: send nothing, leave the wait armed (expires at the cap, like REFUSE)
	K7_ARR_REFUSE,         // a gate refused this poll (expires at the cap)
	K7_ARR_LATCH_OBSERVE,  // every gate passed, but the key is false: latch the would-fire time, send nothing
	K7_ARR_SEND            // every gate passed and the key is true: send now
};

// The fire-time decision, once K7ArrivalPoll has already returned FIRE.
// held is k7PostDeathHold (Fix A) -- takes precedence over everything, even
// the key being true. Every other refusal gate (stillStopped, destMatch,
// zonesOk, charBlocked, cooldown, the budget) is checked next, REGARDLESS of
// `enabled`, so a k7ArrivalTrigger=false latch never overstates savedMs by
// latching a poll `true` would have refused anyway. Only once every refusal
// gate has passed does `enabled` (k7ArrivalTriggerEnabled: true sends, false
// only latches/observes) decide SEND vs. LATCH_OBSERVE.
//
// stillStopped, destMatch and zonesOk are, respectively, the still-stopped
// re-check, K7TryDeletedReissue's own destination-match gate (d/n) and its
// zone-accessibility gate (z). charBlocked folds the
// carried/inSomething/melee/no-AI/enemies/threats/hold-position reads into
// one flag (each already its own K7TryDeletedReissue gate; the caller does not need
// to tell them apart). cooldown is the tracker's 2s IslandRecentlyReissued
// check; reissueCount/maxReissues is the shared 8-send budget.
K7ArrivalAction K7ArrivalFireGate(bool enabled, bool held, bool stillStopped, bool destMatch,
                                  bool zonesOk, bool charBlocked, bool cooldown,
                                  int reissueCount, int maxReissues);

#endif // KENSHI_ZONE_OPT_K7_ARRIVAL_POLICY_H
