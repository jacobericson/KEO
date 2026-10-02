#ifndef KEO_ZONE_NAV_RECOVERY_H
#define KEO_ZONE_NAV_RECOVERY_H

// Recovery from geometry invalidation for a cell whose mesh is already
// installed: stop claims, let readers finish, discard the stale output,
// request native removal, wait for the acknowledgement, and only then let
// anything register against the cell again.
//
// Pure logic over the nav ledger -- no game headers, no lock, no clock of
// its own, and nothing here waits or sleeps. The machine advances at most
// one step per call, so a caller drives it from a per-frame tick; a step
// whose precondition is not met simply leaves the cell where it is.
//
// Why the driver exists at all, rather than the four calls being made in
// order by whoever noticed the invalidation: the ledger refuses a re-claim
// only once removal is *pending*. Between the invalidation and the removal
// request the entry is merely stale, and a claim there would succeed --
// after which the removal, requested against a cell that now holds a fresh
// claim, is exactly the "delete the old sector without replacement" the
// ordering exists to prevent. Closing that window is this module's job:
// ZoneNavRecoveryClaimAllowed refuses from the first step, not the second.

#include "zone/geometry/zone_nav_ledger.h"

enum ZoneNavRecoveryStep
{
	ZONE_RECOVERY_NONE = 0,
	ZONE_RECOVERY_QUIESCING,          // claims stopped; waiting for readers to release
	ZONE_RECOVERY_REMOVAL_REQUESTED,  // native removal asked for, not yet acknowledged
	ZONE_RECOVERY_REMOVAL_ACKED,      // acknowledged; registration against settled inputs is legal
	ZONE_RECOVERY_REREGISTER,         // handed back to whoever registers
	ZONE_RECOVERY_FAULTED             // a step did not complete in time; live state preserved
};

struct ZoneNavRecoveryEntry
{
	int    step;
	double enteredAt;
	bool   inUse;
};

struct ZoneNavRecovery
{
	ZoneNavRecoveryEntry entries[ZONE_GRID_CELLS];
	double   timeoutSec;
	unsigned begun;
	unsigned completed;
	unsigned faulted;
	unsigned blockedClaims;   // claims refused because the cell was mid-recovery
};

void ZoneNavRecoveryInit(ZoneNavRecovery* r, double timeoutSec);

// Step 1. Marks the ledger claim stale (which bumps its claim generation, so
// a worker finishing late discards its output) and closes the cell to new
// claims. Refuses a cell already in recovery.
bool ZoneNavRecoveryBegin(ZoneNavRecovery* r, ZoneNavLedger* l,
                          int cellX, int cellY, double now);

// Steps 2-3, one per call. `readersDone` is the caller's answer to "has every
// reader of the stale output finished" -- the ledger's reader count is the
// memory-safety pin behind it. `nativeRemovalAcked` is the engine's own
// acknowledgement that the sector is gone. Returns the step the cell is in
// after the call.
int ZoneNavRecoveryAdvance(ZoneNavRecovery* r, ZoneNavLedger* l,
                           int cellX, int cellY,
                           bool readersDone, bool nativeRemovalAcked, double now);

// Step 4's end: the caller has registered against settled inputs (or has
// decided not to). Clears the cell's recovery record and reopens it to
// claims. Refuses unless the cell reached ZONE_RECOVERY_REREGISTER.
bool ZoneNavRecoveryComplete(ZoneNavRecovery* r, int cellX, int cellY);

int ZoneNavRecoveryStepOf(const ZoneNavRecovery* r, int cellX, int cellY);

// False from the moment recovery begins until it completes, and false while
// the cell is faulted. Every claim path must ask; the counter records the
// ones that did and were refused.
bool ZoneNavRecoveryClaimAllowed(ZoneNavRecovery* r, int cellX, int cellY);

// The memory-safety answer: the stale output may be freed only once removal
// is acknowledged and no reader holds the ledger's pin. A faulted cell never
// answers true -- a timeout reports a fault and preserves live state; it
// never permits unsafe freeing.
bool ZoneNavRecoveryMayFreeResult(const ZoneNavRecovery* r, const ZoneNavLedger* l,
                                  int cellX, int cellY);

// The predicate behind "hold registration until removal is acknowledged".
// The hold itself is a blocking main-thread wait that only means anything
// once a build adopts against its own mesh, so it belongs to the change that
// unlocks that mode; this is the test it will ask.
bool ZoneNavRecoveryRegistrationHeld(const ZoneNavRecovery* r, int cellX, int cellY);

const char* ZoneNavRecoveryStepName(int step);

#endif // KEO_ZONE_NAV_RECOVERY_H
