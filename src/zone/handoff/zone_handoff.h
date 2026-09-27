#ifndef KENSHI_ZONE_OPT_ZONE_HANDOFF_H
#define KENSHI_ZONE_OPT_ZONE_HANDOFF_H

#include "base/config.h"

// Preparation ownership and adoption: the mod prepares a cell privately
// (+176 = 1, +177 = 0), then hands it to the game rather than publishing it
// itself. Two routes lead into the game's ownership — a cohort the mod
// admits when the cell is prepared and the loader is idle, and a takeover
// when real native demand reaches a cell the mod is still holding. Both end
// with the cell inside the pending-activation set, after which every
// remaining lifecycle step is the game's.
//
// Main thread only, all of it: the sets these functions mutate are the
// game's, and the ledger behind them is main-thread state.

#if ZONEHAND_STEP >= 2

// Called once at startup, before any detour can fire.
void ZoneHandoffInit();

// The world was replaced (save load, import, new game). Starts a new world
// epoch, so every record from the old one is recognisably stale.
void ZoneHandoffOnWorldReset();

// A mod preload just took a cell at +176. Clears the cell's three activation
// countdowns — the engine writes one before it discovers the cell is already
// held, and outside the active set that value never ticks, so a town reading
// its coverage would see a lease that can never expire — and opens the
// cell's preparation record.
void ZoneHandoffNoteLoaded(void* zoneEntry, int gx, int gy);

void ZoneHandoffNoteContentInitialized(int gx, int gy);
void ZoneHandoffNoteRegistered(int gx, int gy);

// The mod has let go of a cell it was preparing. Retires the record for a
// cell still in a private stage; an adopted cell is the game's and is
// retired by the tick below when the game has finished with it.
void ZoneHandoffNoteDropped(int gx, int gy);

// The world's own state, published every frame whatever else the frame
// does: the readiness classifier's global override is "the game is loading
// a save", which is exactly the condition under which the rest of the mod's
// per-frame work is skipped, so the two cannot share a call site.
void ZoneHandoffNoteWorldState(void* zoneMgr);

// The main-thread safe point, once per frame: advances prepared cells to
// ready, admits one cohort, follows adopted cells through the game's own
// bookkeeping and retires them when the game has let them go.
void ZoneHandoffTick(void* zoneMgr, double now);

// The world the records below belong to. One counter, read rather than
// mirrored: a second copy kept equal by convention would, on the one frame
// it was not, make every record look stale for the rest of the session with
// nothing in the log to say so.
unsigned ZoneHandoffWorldEpoch();

// The frame boundary for the admission flag below, published before any
// work the frame might skip.
void ZoneHandoffBeginFrame();

// True when a cohort was admitted earlier in this frame. Admission inserts a
// cohort into the pending set and starts a loading cycle; anything else that
// does heavy per-frame lifecycle work stands down for that frame rather than
// adding to it. Spacing rules bound how often such work happens, not whether
// two kinds of it land in the same frame, which is what this answers.
bool ZoneHandoffAdoptedThisFrame();

// True while the loading cycle now in progress (loadingPhase != 0) is one
// this frame flag ever fired for: it latches on the frame a cohort or a
// takeover is admitted and clears once the loader returns to idle. A render
// lever asking "is the purge running for a cycle the handoff mechanism
// raised, not a normal transition" reads this rather than the per-frame flag,
// since the phase that matters (3->4) is rarely the same frame as admission.
bool ZoneHandoffAdoptionCycleInFlight();

// The four main-side physics queue counts, all zero. The flag the engine
// keeps beside them is the back side's answer, lags a frame, and is cleared
// by any activation; the counts are what the loader's own phase tests.
bool ZoneHandoffPhysicsCountsClear();

// The two questions the activateZoneMap detour asks.
bool ZoneHandoffTownGuardRefuses(int activationType, float deactivationTimer);
bool ZoneHandoffTakeover(void* zoneMgr, void* zoneEntry, int activationType);

#else

inline void ZoneHandoffInit() {}
inline void ZoneHandoffOnWorldReset() {}
inline void ZoneHandoffNoteLoaded(void*, int, int) {}
inline void ZoneHandoffNoteContentInitialized(int, int) {}
inline void ZoneHandoffNoteRegistered(int, int) {}
inline void ZoneHandoffNoteDropped(int, int) {}
inline void ZoneHandoffNoteWorldState(void*) {}
inline void ZoneHandoffTick(void*, double) {}
inline unsigned ZoneHandoffWorldEpoch() { return 0; }
inline void ZoneHandoffBeginFrame() {}
inline bool ZoneHandoffAdoptedThisFrame() { return false; }
inline bool ZoneHandoffAdoptionCycleInFlight() { return false; }
inline bool ZoneHandoffPhysicsCountsClear() { return true; }

#endif // ZONEHAND_STEP >= 2

#endif // KENSHI_ZONE_OPT_ZONE_HANDOFF_H
