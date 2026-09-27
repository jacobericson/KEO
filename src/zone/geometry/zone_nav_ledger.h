#pragma once
// Ledger 2: navmesh claims, input certificates and invalidation, one entry
// per grid cell.
//
// Threading contract: this module takes no locks of its own. `geometryEpoch`
// and `admissionOpen` are single-word, single-writer fields — written only
// by the main thread (a geometry-publication boundary, or the world-reset
// admission barrier of section 7) — and may be read from any thread without
// a lock. Every other field (a cell's claim state, certificate, reader
// count) is mutated from the NavMesh background thread or a worker, and the
// caller must hold the existing navmesh lock (`processJobCS`, per the
// project's lock-order rules) around any call that mutates one; this module
// does not know about that lock and cannot enforce it. Main-thread callers
// that only read a claim's state (to decide whether to wait, or to install a
// result) must take the same lock, for the same reason the existing L1/L2
// cache does.
//
// `claimGeneration` is independent of ZoneIdentity::contentIncarnation:
// geometry invalidation retries the mesh for content that is retained, so it
// bumps this counter, never the identity's incarnation.

#include "zone/zone_ledger_core.h"

enum ZoneNavClaimState
{
	NAV_CLAIM_NONE = 0,
	NAV_CLAIM_ACTIVE,           // claimed, generation in flight
	NAV_CLAIM_RESULT_READY,     // a worker produced a result, not yet installed
	NAV_CLAIM_INSTALLED,        // certificate accepted, mesh in
	NAV_CLAIM_STALE,            // superseded by invalidation; a late reader must discard
	NAV_CLAIM_REMOVAL_PENDING,  // native removal requested, not yet acknowledged
	NAV_CLAIM_REMOVAL_ACKED     // removal acknowledged; safe to register against settled inputs
};

struct ZoneNavEntry
{
	ZoneIdentity identity;
	unsigned     claimGeneration;
	unsigned     certificateEpoch;   // geometryEpoch captured when this claim was taken
	int          claimState;
	int          readerCount;        // memory-safety pin only; see ZoneNavLedgerAddReader below.
	                                  // Not the same fact as ledger 3's retention readerPinCount.
	bool         inUse;
	unsigned     refusedAttempts;    // ZoneNavLedgerClaim/AcknowledgeRemoval calls this entry refused;
	                                  // parity with ledger 1's ZonePrepEntry::illegalAttempts, so a
	                                  // caller that ignores a refusal's return value still leaves a
	                                  // reachable trace instead of a silent no-op
};

struct ZoneNavLedger
{
	ZoneNavEntry entries[ZONE_GRID_CELLS];
	unsigned     geometryEpoch;
	unsigned     admissionEpoch;     // the world epoch this ledger currently admits claims for
	bool         admissionOpen;
};

void ZoneNavLedgerInit(ZoneNavLedger* l, unsigned worldEpoch);

ZoneNavEntry*       ZoneNavLedgerGet(ZoneNavLedger* l, int cellX, int cellY);
const ZoneNavEntry* ZoneNavLedgerGetConst(const ZoneNavLedger* l, int cellX, int cellY);

// Main-thread only. Bumps the geometry epoch at a verified geometry
// publication/removal boundary; returns the new value.
unsigned ZoneNavLedgerBumpGeometryEpoch(ZoneNavLedger* l);
unsigned ZoneNavLedgerGeometryEpoch(const ZoneNavLedger* l);   // any thread

// Section 7's admission barrier: closed before a world reset begins, so a
// worker finishing a claim after the barrier closes is told (by
// ZoneNavLedgerAdmissionOpen returning false) to discard its output rather
// than install it. Main-thread only to close/clear; readable from any
// thread.
void ZoneNavLedgerCloseAdmission(ZoneNavLedger* l);
bool ZoneNavLedgerAdmissionOpen(const ZoneNavLedger* l);
void ZoneNavLedgerClearAll(ZoneNavLedger* l, unsigned newWorldEpoch);   // reopens admission at the new epoch

// Takes a claim for `identity`. Refuses (returns false, no state change,
// bumps the refused cell's refusedAttempts) when admission is closed,
// `identity.worldEpoch` does not match the ledger's current admission
// epoch, or the current entry is NAV_CLAIM_REMOVAL_PENDING (section 5 step
// 3: a requeued removal can consume a fresh claim's creation as its own
// no-op and later delete the old sector with nothing installed in its
// place, so a new claim must wait for ZoneNavLedgerAcknowledgeRemoval
// first). A refusal for the admission-epoch/closed-admission reasons is
// counted against the target cell even though that cell may be unused; see
// ZoneNavLedgerRefusedAttempts. On success, captures the current geometry
// epoch as the claim's certificate.
bool ZoneNavLedgerClaim(ZoneNavLedger* l, const ZoneIdentity& identity, int cellX, int cellY);

bool ZoneNavLedgerMarkResultReady(ZoneNavLedger* l, int cellX, int cellY);

// Installs a result. Refuses when the certificate is no longer current
// (ZoneNavLedgerIsCertificateCurrent is false) or admission has closed for
// this identity's epoch; the caller must discard instead.
bool ZoneNavLedgerInstall(ZoneNavLedger* l, int cellX, int cellY);

bool ZoneNavLedgerIsCertificateCurrent(const ZoneNavLedger* l, int cellX, int cellY);

// Geometry invalidation (section 5): marks the claim stale and bumps
// claimGeneration. Does not touch identity.contentIncarnation.
void ZoneNavLedgerInvalidate(ZoneNavLedger* l, int cellX, int cellY);

void ZoneNavLedgerRequestRemoval(ZoneNavLedger* l, int cellX, int cellY);
// Refuses (returns false, no state change, bumps refusedAttempts) unless the
// entry is currently NAV_CLAIM_REMOVAL_PENDING, i.e. a matching
// RequestRemoval actually ran.
bool ZoneNavLedgerAcknowledgeRemoval(ZoneNavLedger* l, int cellX, int cellY);
bool ZoneNavLedgerRemovalAcknowledged(const ZoneNavLedger* l, int cellX, int cellY);

// Calls to Claim or AcknowledgeRemoval this cell has refused, for a future
// stats line: a caller that ignores a refusal's return value is safe (the
// ledger made no state change) but otherwise leaves no trace, so this
// counter is the diagnostic of last resort. Parity with ledger 1's
// ZonePrepEntry::illegalAttempts.
unsigned ZoneNavLedgerRefusedAttempts(const ZoneNavLedger* l, int cellX, int cellY);

// Memory-safety pin for a claimed/installed navmesh result: increment while
// a thread is reading it, so ZoneNavLedgerInvalidate's caller knows the
// underlying mesh memory is not yet safe to free. This is a different fact
// from ledger 3's ZoneRetentionLedgerAddReader, which pins a cell against
// eviction for reasons that may have nothing to do with this memory (an
// in-flight cohort admission, a pending order). A caller reading the mesh
// itself should hold both when the cell must also stay resident; the two
// counts are not linked automatically.
void ZoneNavLedgerAddReader(ZoneNavLedger* l, int cellX, int cellY);
void ZoneNavLedgerReleaseReader(ZoneNavLedger* l, int cellX, int cellY);
int  ZoneNavLedgerReaderCount(const ZoneNavLedger* l, int cellX, int cellY);

// True when the ledger's current entry for this cell no longer matches the
// (identity, claimGeneration) pair a caller captured at claim time — a
// worker finishing late uses this to know whether to discard its output
// instead of installing it.
bool ZoneNavLedgerIsStaleClaim(const ZoneNavLedger* l, int cellX, int cellY,
                               const ZoneIdentity& claimIdentity, unsigned claimGeneration);
