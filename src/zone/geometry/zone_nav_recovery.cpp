#include "zone/geometry/zone_nav_recovery.h"

void ZoneNavRecoveryInit(ZoneNavRecovery* r, double timeoutSec)
{
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
	{
		r->entries[i].step = ZONE_RECOVERY_NONE;
		r->entries[i].enteredAt = 0.0;
		r->entries[i].inUse = false;
	}
	r->timeoutSec = timeoutSec;
	r->begun = 0;
	r->completed = 0;
	r->faulted = 0;
	r->blockedClaims = 0;
}

static ZoneNavRecoveryEntry* Entry(ZoneNavRecovery* r, int cellX, int cellY)
{
	return &r->entries[ZoneCellIndex(cellX, cellY)];
}

static const ZoneNavRecoveryEntry* EntryConst(const ZoneNavRecovery* r, int cellX, int cellY)
{
	return &r->entries[ZoneCellIndex(cellX, cellY)];
}

static void EnterStep(ZoneNavRecoveryEntry* e, int step, double now)
{
	e->step = step;
	e->enteredAt = now;
	e->inUse = (step != ZONE_RECOVERY_NONE);
}

bool ZoneNavRecoveryBegin(ZoneNavRecovery* r, ZoneNavLedger* l,
                          int cellX, int cellY, double now)
{
	ZoneNavRecoveryEntry* e = Entry(r, cellX, cellY);
	if (e->inUse)
		return false;

	// Step 1: the claim for this incarnation stops here. Invalidate bumps
	// the ledger's claim generation, so a worker that finishes afterwards
	// reads its own claim as stale and discards its output instead of
	// installing it.
	ZoneNavLedgerInvalidate(l, cellX, cellY);
	EnterStep(e, ZONE_RECOVERY_QUIESCING, now);
	r->begun++;
	return true;
}

int ZoneNavRecoveryAdvance(ZoneNavRecovery* r, ZoneNavLedger* l,
                           int cellX, int cellY,
                           bool readersDone, bool nativeRemovalAcked, double now)
{
	ZoneNavRecoveryEntry* e = Entry(r, cellX, cellY);
	if (!e->inUse)
		return e->step;

	switch (e->step)
	{
	case ZONE_RECOVERY_QUIESCING:
		// Step 2 waits for the readers of the stale output, because the
		// removal is what makes freeing it legal.
		if (readersDone && ZoneNavLedgerReaderCount(l, cellX, cellY) == 0)
		{
			ZoneNavLedgerRequestRemoval(l, cellX, cellY);
			EnterStep(e, ZONE_RECOVERY_REMOVAL_REQUESTED, now);
		}
		else if (now - e->enteredAt > r->timeoutSec)
		{
			EnterStep(e, ZONE_RECOVERY_FAULTED, now);
			r->faulted++;
		}
		break;

	case ZONE_RECOVERY_REMOVAL_REQUESTED:
		// Step 3. The ledger refuses the acknowledgement unless a removal
		// really was requested, so its answer is checked rather than
		// assumed; a refusal here means the two sides disagree about the
		// cell's state, which is a fault, not something to retry.
		if (nativeRemovalAcked)
		{
			if (ZoneNavLedgerAcknowledgeRemoval(l, cellX, cellY))
				EnterStep(e, ZONE_RECOVERY_REMOVAL_ACKED, now);
			else
			{
				EnterStep(e, ZONE_RECOVERY_FAULTED, now);
				r->faulted++;
			}
		}
		else if (now - e->enteredAt > r->timeoutSec)
		{
			EnterStep(e, ZONE_RECOVERY_FAULTED, now);
			r->faulted++;
		}
		break;

	case ZONE_RECOVERY_REMOVAL_ACKED:
		// Step 4: registration against settled inputs is now legal. The
		// cell stays closed to claims until the caller says it is done,
		// so the reopening is one decision in one place.
		EnterStep(e, ZONE_RECOVERY_REREGISTER, now);
		break;

	default:
		break;   // REREGISTER waits for Complete; FAULTED never advances itself
	}

	return e->step;
}

bool ZoneNavRecoveryComplete(ZoneNavRecovery* r, int cellX, int cellY)
{
	ZoneNavRecoveryEntry* e = Entry(r, cellX, cellY);
	if (!e->inUse || e->step != ZONE_RECOVERY_REREGISTER)
		return false;
	EnterStep(e, ZONE_RECOVERY_NONE, 0.0);
	r->completed++;
	return true;
}

int ZoneNavRecoveryStepOf(const ZoneNavRecovery* r, int cellX, int cellY)
{
	return EntryConst(r, cellX, cellY)->step;
}

bool ZoneNavRecoveryClaimAllowed(ZoneNavRecovery* r, int cellX, int cellY)
{
	ZoneNavRecoveryEntry* e = Entry(r, cellX, cellY);
	if (!e->inUse)
		return true;
	r->blockedClaims++;
	return false;
}

bool ZoneNavRecoveryMayFreeResult(const ZoneNavRecovery* r, const ZoneNavLedger* l,
                                  int cellX, int cellY)
{
	const ZoneNavRecoveryEntry* e = EntryConst(r, cellX, cellY);
	if (!e->inUse)
		return false;
	if (e->step != ZONE_RECOVERY_REMOVAL_ACKED && e->step != ZONE_RECOVERY_REREGISTER)
		return false;
	return ZoneNavLedgerReaderCount(l, cellX, cellY) == 0;
}

bool ZoneNavRecoveryRegistrationHeld(const ZoneNavRecovery* r, int cellX, int cellY)
{
	const ZoneNavRecoveryEntry* e = EntryConst(r, cellX, cellY);
	if (!e->inUse)
		return false;
	return e->step == ZONE_RECOVERY_QUIESCING
	    || e->step == ZONE_RECOVERY_REMOVAL_REQUESTED
	    || e->step == ZONE_RECOVERY_FAULTED;
}

const char* ZoneNavRecoveryStepName(int step)
{
	switch (step)
	{
	case ZONE_RECOVERY_NONE:             return "none";
	case ZONE_RECOVERY_QUIESCING:        return "quiescing";
	case ZONE_RECOVERY_REMOVAL_REQUESTED:return "removalRequested";
	case ZONE_RECOVERY_REMOVAL_ACKED:    return "removalAcked";
	case ZONE_RECOVERY_REREGISTER:       return "reregister";
	case ZONE_RECOVERY_FAULTED:          return "faulted";
	}
	return "?";
}
