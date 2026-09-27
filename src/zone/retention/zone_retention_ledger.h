#pragma once
// Ledger 3: retention leases, one entry per grid cell. Main-thread-only: the
// policy tick and the ZoneMap::update prologue hold/release decision both
// run on the main thread, so nothing here needs a lock.
//
// This ledger models no town state at all: the town guard is a stateless
// rule at the activateZoneMap detour ("refuse an ACTIVATION_TOWN refresh
// carrying a nonpositive timer"), which needs no cohort tag, incarnation
// check or coverage inspection.
//
// It models proximity nowhere either. Whether a cell is near a live anchor
// changes on a one-second cadence for every cell at once, while every field
// below is per-adoption lease state; the proximity map lives with the
// lifecycle pass that rebuilds it and is applied by the caller.

#include "zone/zone_ledger_core.h"

struct ZoneRetentionEntry
{
	ZoneIdentity identity;
	double       minResidenceDeadline;      // policy-clock time; 0 = none set; per-adoption (cleared by Reset)
	double       graceDeadline;             // per-adoption (cleared by Reset)
	double       predictionLeaseDeadline;   // per-adoption (cleared by Reset)
	int          revisitBackoffLevel;       // 0/1/2 -> next grace 30/60/120s; SESSION-LIFETIME: survives Reset,
	                                         // only ZoneRetentionLedgerClearAll (world reset) zeroes it
	double       lastEvictedAt;             // policy-clock time; < 0 = never evicted; session-lifetime, same as the level above
	int          readerPinCount;            // retention-only safety pin; see ZoneRetentionLedgerAddReader below
	bool         inUse;
};

struct ZoneRetentionLedger
{
	ZoneRetentionEntry entries[ZONE_GRID_CELLS];
};

void ZoneRetentionLedgerInit(ZoneRetentionLedger* l);

// World-reset equivalent of Init: zeroes every cell, including the session-
// lifetime revisit-backoff ladder. Call this, not per-cell Reset, when the
// world itself replaces (save-load, import, new game, quit) — mirrors
// ZoneNavLedgerClearAll's role for ledger 2. Functionally identical to Init
// today; kept as a distinctly-named entry point so a world-reset call site
// reads as what it is rather than as a coincidental re-init.
void ZoneRetentionLedgerClearAll(ZoneRetentionLedger* l);

ZoneRetentionEntry*       ZoneRetentionLedgerGet(ZoneRetentionLedger* l, int cellX, int cellY);
const ZoneRetentionEntry* ZoneRetentionLedgerGetConst(const ZoneRetentionLedger* l, int cellX, int cellY);

// (Re)starts tracking a cell at adoption: identity and the three per-
// adoption deadlines (minResidence/grace/predictionLease) reset. These are
// the mod's own deadlines and have nothing to do with the cell's native
// activation countdowns, which this module cannot see (it holds no game
// headers) and never touches. `revisitBackoffLevel` and `lastEvictedAt`
// deliberately survive a Reset (see the struct comment above): a revisit is
// itself a Reset call, so zeroing them here would leave the backoff ladder
// unable to climb past its first step on exactly the repetition it exists to
// detect. readerPinCount also survives -- an outstanding reader is not lease
// state.
void ZoneRetentionLedgerReset(ZoneRetentionLedger* l, int cellX, int cellY, const ZoneIdentity& identity, double now);

void ZoneRetentionLedgerSetMinResidence(ZoneRetentionLedger* l, int cellX, int cellY, double deadline);
void ZoneRetentionLedgerSetGrace(ZoneRetentionLedger* l, int cellX, int cellY, double deadline);
// Cancel a prediction lease (replacement/cancellation) by passing `now` or earlier.
void ZoneRetentionLedgerSetPredictionLease(ZoneRetentionLedger* l, int cellX, int cellY, double deadline);

// Stops tracking a cell the game has finished with: the three per-adoption
// deadlines and `inUse` clear, while the backoff ladder, the eviction clock
// and any outstanding reader pin survive exactly as they do across a Reset.
// Without this the in-use count would only ever grow.
void ZoneRetentionLedgerRetire(ZoneRetentionLedger* l, int cellX, int cellY);

// Cells the ledger is tracking. A diagnostic, not the retention pressure
// signal: a tracked cell inside a live native lease is one nobody is holding.
int ZoneRetentionLedgerCountInUse(const ZoneRetentionLedger* l);

// `readerPinCount` governs this ledger's own hold/release decision only; it
// is not the same fact as ledger 2's ZoneNavLedgerReaderCount, which gates
// whether a claimed/installed navmesh result is still safe to free. A
// caller that pins a cell because it is reading the mesh should increment
// the nav ledger's count (memory safety); a caller that pins a cell because
// something else needs it to stay resident in Set B — an in-flight cohort
// admission, a mover's pending order into this cell — increments this one
// (an eviction veto). A single logical hold sometimes needs both pins, but
// nothing in either module ties them together: the caller decides which
// pin(s) its reason for holding requires.
void ZoneRetentionLedgerAddReader(ZoneRetentionLedger* l, int cellX, int cellY);
void ZoneRetentionLedgerReleaseReader(ZoneRetentionLedger* l, int cellX, int cellY);

// 30 / 60 / 120 seconds for backoff level 0 / 1 / >=2 (clamped).
double ZoneRetentionGraceSecondsForLevel(int level);

// True when `now` is within 60 seconds of `lastEvictedAt` (and the cell was
// ever evicted at all).
bool ZoneRetentionIsRapidRevisit(double lastEvictedAt, double now);

// Called when a cell is re-admitted (adopted or preloaded again): bumps the
// backoff level on a rapid revisit, capped at 2, and returns the resulting
// grace-seconds value the caller should apply via SetGrace(now + result).
// Leaves the level unchanged (and returns its current grace) otherwise. Call
// this before or after ZoneRetentionLedgerReset for the same re-admission —
// order does not matter, since Reset no longer touches the backoff fields.
double ZoneRetentionLedgerNoteRevisit(ZoneRetentionLedger* l, int cellX, int cellY, double now);

// Records that this cell was just evicted, for the next revisit check.
void ZoneRetentionLedgerNoteEvicted(ZoneRetentionLedger* l, int cellX, int cellY, double now);

// The native expiry test the ZoneMap::update prologue runs before asking the
// policy at all: every one of the three native countdowns is already <= 0,
// or would cross zero this frame (<= the frame delta the original
// subtracts). Pure arithmetic, no ledger involved.
bool ZoneRetentionNativeWouldExpireThisFrame(double camera, double player, double town, double frameDelta);

// max(minResidence, grace, predictionLease) among deadlines still in the
// future relative to `now`; 0.0 if the cell has no live deadline.
double ZoneRetentionKeepDeadline(const ZoneRetentionEntry* e, double now);

// False (release-eligible) only when every deadline has passed, no reader
// holds the cell, and the anchors were readable. Unreadable anchors, or any
// pinned reader, force a hold regardless of the deadlines. `anchorsReadable`
// is one global fact about the last rebuild, not a per-cell one.
bool ZoneRetentionPolicyWantsHold(const ZoneRetentionEntry* e, double now, bool anchorsReadable);

// Cells for which PolicyWantsHold is false, into a caller-provided buffer
// (no allocation). Ranking by distance/recency is the caller's job; this
// only tells it which cells are eligible at all.
int ZoneRetentionLedgerCollectEvictionCandidates(const ZoneRetentionLedger* l, double now, bool anchorsReadable,
                                                 ZoneIdentity* out, int cap);
