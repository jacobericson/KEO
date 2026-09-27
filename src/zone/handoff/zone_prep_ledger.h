#pragma once
// Ledger 1: preparation ownership and the next initialization step, one
// entry per grid cell. Main-thread-only: every field except `classWord` is
// read and written exclusively from the main thread (preparation runs
// incrementally there, and both lifecycle detours this design adds —
// activateZoneMap_ZoneMap and ZoneMap::update's prologue — run on the main
// thread too). `classWord` is the exception: written only by the main
// thread, readable from any thread with no lock (see ZoneReadinessClass in
// zone_ledger_core.h) — it is hook_isContentPending's per-cell input.
// `globalGameOwnedBypass` is main-thread-only like the rest: its sole reader
// is the cohort window's `worldQuiet` inside ZoneHandoffTick. The hook reads
// ZM+8 itself, fresh, at its own call site (zone_readiness_contract.h),
// because a once-per-frame mirror can lag a synchronous load for its whole
// span.
//
// The three states this ledger does not decide for itself — NativeA ->
// NativeBLoading -> NativeActive — are the game's own +176/+177/Set B
// bookkeeping. ZonePrepLedgerObserveCell is the only way into them; nothing
// else advances a cell out of NativeA.

#include "zone/zone_ledger_core.h"

struct ZonePrepEntry
{
	ZoneIdentity identity;
	int          state;             // ZoneLifecycleState
	int          nextInitStep;      // opaque resume point for incremental preparation
	bool         pendingAdoption;   // a real-demand request arrived after phase 2; do not force a phase rewind
	bool         inUse;
	bool         everUsed;          // true once this slot has held any incarnation; survives Release so the
	                                 // next Begin bumps contentIncarnation instead of restarting it at 0
	double       enteredStateAt;    // policy-clock time `state` was last entered
	unsigned     illegalAttempts;   // ZonePrepLedgerSetState calls rejected by the state machine
};

struct ZonePrepLedger
{
	ZonePrepEntry entries[ZONE_GRID_CELLS];
	unsigned char classWord[ZONE_GRID_CELLS];   // ZoneReadinessClass; see the file comment
	int           countByState[ZONE_STATE_COUNT];
	bool          globalGameOwnedBypass;        // ZM+8 (justLoadedAGame) mirror, main-thread-only: gates the cohort window
};

// The one instance every consumer shares. It lives here, in the ledger's own
// module, because the writer of the per-cell class word and its readers are
// different translation units: a second instance declared beside either of
// them would build, link and test clean while the readers saw nothing the
// writer wrote.
// It lives in BSS with no global-constructor ordering hazard, and its zero
// state is already the correct empty state: every cell ZONE_CLASS_NONE,
// bypass false.
extern ZonePrepLedger g_zonePrepLedger;

void ZonePrepLedgerInit(ZonePrepLedger* l);

// World-reset equivalent of Init: zeroes every cell and the global bypass
// flag. Call this, not per-cell Release, when the world itself replaces
// (save-load, import, new game, quit) — mirrors ZoneNavLedgerClearAll's role
// for ledger 2. Functionally identical to Init today; kept as a distinctly-
// named entry point so a world-reset call site reads as what it is.
void ZonePrepLedgerClearAll(ZonePrepLedger* l);

ZonePrepEntry*       ZonePrepLedgerGet(ZonePrepLedger* l, int cellX, int cellY);
const ZonePrepEntry* ZonePrepLedgerGetConst(const ZonePrepLedger* l, int cellX, int cellY);

// Begins a fresh incarnation shell for a cell: legal only when the slot is
// unused or its current state is Unloaded. Bumps contentIncarnation over
// whatever the slot last held (0 the first time), resets nextInitStep and
// pendingAdoption. Returns false, unchanged, if the slot is mid-lifecycle.
bool ZonePrepLedgerBegin(ZonePrepLedger* l, int cellX, int cellY, unsigned worldEpoch, double now);

// Applies a state-machine transition. Returns false and counts an illegal
// attempt, leaving the entry unchanged, when ZoneLifecycleCanTransition
// refuses it or the slot is not in use.
bool ZonePrepLedgerSetState(ZonePrepLedger* l, int cellX, int cellY, int toState, double now);

void ZonePrepLedgerSetPendingAdoption(ZonePrepLedger* l, int cellX, int cellY, bool pending);
void ZonePrepLedgerSetNextInitStep(ZonePrepLedger* l, int cellX, int cellY, int step);

// Advances the game-observed tail of the state machine to match the game's
// own flags for an adopted cell. Never invents a state the flags do not
// support, and is a no-op off the NativeA/NativeBLoading path.
void ZonePrepLedgerObserveCell(ZonePrepLedger* l, int cellX, int cellY, bool f176, bool f177, bool inSetB, double now);

// Clears a slot back to unused (Unloaded, no identity). Refuses (returns
// false, counts an illegal attempt, leaves the entry unchanged) unless the
// slot is currently ZONE_STATE_RETIRING, i.e. teardown has actually reached
// its terminal step; the next ZonePrepLedgerBegin then starts a new
// incarnation.
bool ZonePrepLedgerRelease(ZonePrepLedger* l, int cellX, int cellY);

// Publishes/reads the lock-free per-cell classification. Publish is main-
// thread only; read is safe from any thread.
void ZonePrepLedgerPublishClass(ZonePrepLedger* l, int cellX, int cellY, int cls);
int  ZonePrepLedgerReadClass(const ZonePrepLedger* l, int cellX, int cellY);

void ZonePrepLedgerSetGameOwnedBypass(ZonePrepLedger* l, bool on);
bool ZonePrepLedgerGameOwnedBypass(const ZonePrepLedger* l);


// Every cell currently ReadyForAdoption, written into a caller-provided
// buffer (no allocation). Returns the number written, capped at `cap`; the
// state's occupancy count (countByState) tells the caller whether the buffer
// was big enough without a second pass.
int ZonePrepLedgerCollectReady(const ZonePrepLedger* l, ZoneIdentity* out, int cap);

// countByState[state], for a caller that only needs the count (e.g. to tell
// whether ZonePrepLedgerCollectReady's cap truncated the ReadyForAdoption
// set it just filled).
int ZonePrepLedgerCountInState(const ZonePrepLedger* l, int state);
