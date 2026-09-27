#include "zone/geometry/zone_nav_ledger.h"

static void ClearEntry(ZoneNavEntry* e)
{
	e->identity.worldEpoch = 0;
	e->identity.cellX = 0;
	e->identity.cellY = 0;
	e->identity.contentIncarnation = 0;
	e->claimGeneration = 0;
	e->certificateEpoch = 0;
	e->claimState = NAV_CLAIM_NONE;
	e->readerCount = 0;
	e->inUse = false;
	e->refusedAttempts = 0;
}

void ZoneNavLedgerInit(ZoneNavLedger* l, unsigned worldEpoch)
{
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
		ClearEntry(&l->entries[i]);
	l->geometryEpoch = 0;
	l->admissionEpoch = worldEpoch;
	l->admissionOpen = true;
}

ZoneNavEntry* ZoneNavLedgerGet(ZoneNavLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

const ZoneNavEntry* ZoneNavLedgerGetConst(const ZoneNavLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

unsigned ZoneNavLedgerBumpGeometryEpoch(ZoneNavLedger* l)
{
	return ++l->geometryEpoch;
}

unsigned ZoneNavLedgerGeometryEpoch(const ZoneNavLedger* l)
{
	return l->geometryEpoch;
}

void ZoneNavLedgerCloseAdmission(ZoneNavLedger* l)
{
	l->admissionOpen = false;
}

bool ZoneNavLedgerAdmissionOpen(const ZoneNavLedger* l)
{
	return l->admissionOpen;
}

void ZoneNavLedgerClearAll(ZoneNavLedger* l, unsigned newWorldEpoch)
{
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
		ClearEntry(&l->entries[i]);
	l->geometryEpoch = 0;
	l->admissionEpoch = newWorldEpoch;
	l->admissionOpen = true;
}

bool ZoneNavLedgerClaim(ZoneNavLedger* l, const ZoneIdentity& identity, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);

	if (!l->admissionOpen || identity.worldEpoch != l->admissionEpoch)
	{
		e->refusedAttempts++;
		return false;
	}

	// A removal already requested but not yet acknowledged
	// must finish before anything re-registers against this cell, or a
	// requeued removal can consume the new claim's creation as its no-op
	// and later delete the old sector with nothing installed in its place.
	if (e->inUse && e->claimState == NAV_CLAIM_REMOVAL_PENDING)
	{
		e->refusedAttempts++;
		return false;
	}

	e->identity = identity;
	e->claimState = NAV_CLAIM_ACTIVE;
	e->certificateEpoch = l->geometryEpoch;
	e->inUse = true;
	return true;
}

bool ZoneNavLedgerMarkResultReady(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);
	if (!e->inUse || e->claimState != NAV_CLAIM_ACTIVE)
		return false;
	e->claimState = NAV_CLAIM_RESULT_READY;
	return true;
}

bool ZoneNavLedgerIsCertificateCurrent(const ZoneNavLedger* l, int cellX, int cellY)
{
	const ZoneNavEntry* e = ZoneNavLedgerGetConst(l, cellX, cellY);
	return e->inUse && e->certificateEpoch == l->geometryEpoch;
}

bool ZoneNavLedgerInstall(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);
	if (!e->inUse || e->claimState != NAV_CLAIM_RESULT_READY)
		return false;
	if (!l->admissionOpen || e->identity.worldEpoch != l->admissionEpoch)
		return false;
	if (e->certificateEpoch != l->geometryEpoch)
		return false;
	e->claimState = NAV_CLAIM_INSTALLED;
	return true;
}

void ZoneNavLedgerInvalidate(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);
	if (!e->inUse)
		return;
	e->claimState = NAV_CLAIM_STALE;
	e->claimGeneration++;
}

void ZoneNavLedgerRequestRemoval(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavLedgerGet(l, cellX, cellY)->claimState = NAV_CLAIM_REMOVAL_PENDING;
}

bool ZoneNavLedgerAcknowledgeRemoval(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);
	if (!e->inUse || e->claimState != NAV_CLAIM_REMOVAL_PENDING)
	{
		e->refusedAttempts++;
		return false;
	}
	e->claimState = NAV_CLAIM_REMOVAL_ACKED;
	return true;
}

bool ZoneNavLedgerRemovalAcknowledged(const ZoneNavLedger* l, int cellX, int cellY)
{
	return ZoneNavLedgerGetConst(l, cellX, cellY)->claimState == NAV_CLAIM_REMOVAL_ACKED;
}

void ZoneNavLedgerAddReader(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavLedgerGet(l, cellX, cellY)->readerCount++;
}

void ZoneNavLedgerReleaseReader(ZoneNavLedger* l, int cellX, int cellY)
{
	ZoneNavEntry* e = ZoneNavLedgerGet(l, cellX, cellY);
	if (e->readerCount > 0)
		e->readerCount--;
}

int ZoneNavLedgerReaderCount(const ZoneNavLedger* l, int cellX, int cellY)
{
	return ZoneNavLedgerGetConst(l, cellX, cellY)->readerCount;
}

unsigned ZoneNavLedgerRefusedAttempts(const ZoneNavLedger* l, int cellX, int cellY)
{
	return ZoneNavLedgerGetConst(l, cellX, cellY)->refusedAttempts;
}

bool ZoneNavLedgerIsStaleClaim(const ZoneNavLedger* l, int cellX, int cellY,
                               const ZoneIdentity& claimIdentity, unsigned claimGeneration)
{
	const ZoneNavEntry* e = ZoneNavLedgerGetConst(l, cellX, cellY);
	if (!e->inUse)
		return true;
	if (!ZoneIdentityEqual(e->identity, claimIdentity))
		return true;
	return e->claimGeneration != claimGeneration;
}
