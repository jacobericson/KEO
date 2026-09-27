#include "zone/retention/zone_retention_ledger.h"

static void ClearEntry(ZoneRetentionEntry* e)
{
	e->identity.worldEpoch = 0;
	e->identity.cellX = 0;
	e->identity.cellY = 0;
	e->identity.contentIncarnation = 0;
	e->minResidenceDeadline = 0.0;
	e->graceDeadline = 0.0;
	e->predictionLeaseDeadline = 0.0;
	e->revisitBackoffLevel = 0;
	e->lastEvictedAt = -1.0;
	e->readerPinCount = 0;
	e->inUse = false;
}

void ZoneRetentionLedgerInit(ZoneRetentionLedger* l)
{
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
		ClearEntry(&l->entries[i]);
}

void ZoneRetentionLedgerClearAll(ZoneRetentionLedger* l)
{
	ZoneRetentionLedgerInit(l);
}

ZoneRetentionEntry* ZoneRetentionLedgerGet(ZoneRetentionLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

const ZoneRetentionEntry* ZoneRetentionLedgerGetConst(const ZoneRetentionLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

void ZoneRetentionLedgerReset(ZoneRetentionLedger* l, int cellX, int cellY, const ZoneIdentity& identity, double now)
{
	// revisitBackoffLevel and lastEvictedAt are session-lifetime and
	// deliberately not touched here: a revisit is itself a Reset call, and
	// zeroing them on every Reset would make the backoff ladder unable to
	// escalate past its first step. Only ZoneRetentionLedgerClearAll (world
	// reset) zeroes them.
	ZoneRetentionEntry* e = ZoneRetentionLedgerGet(l, cellX, cellY);
	e->identity = identity;
	e->minResidenceDeadline = 0.0;
	e->graceDeadline = 0.0;
	e->predictionLeaseDeadline = 0.0;
	e->inUse = true;
	(void)now;
}

void ZoneRetentionLedgerSetMinResidence(ZoneRetentionLedger* l, int cellX, int cellY, double deadline)
{
	ZoneRetentionLedgerGet(l, cellX, cellY)->minResidenceDeadline = deadline;
}

void ZoneRetentionLedgerSetGrace(ZoneRetentionLedger* l, int cellX, int cellY, double deadline)
{
	ZoneRetentionLedgerGet(l, cellX, cellY)->graceDeadline = deadline;
}

void ZoneRetentionLedgerSetPredictionLease(ZoneRetentionLedger* l, int cellX, int cellY, double deadline)
{
	ZoneRetentionLedgerGet(l, cellX, cellY)->predictionLeaseDeadline = deadline;
}

void ZoneRetentionLedgerRetire(ZoneRetentionLedger* l, int cellX, int cellY)
{
	ZoneRetentionEntry* e = ZoneRetentionLedgerGet(l, cellX, cellY);
	e->minResidenceDeadline    = 0.0;
	e->graceDeadline           = 0.0;
	e->predictionLeaseDeadline = 0.0;
	e->inUse                   = false;
}

int ZoneRetentionLedgerCountInUse(const ZoneRetentionLedger* l)
{
	int n = 0;
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
		if (l->entries[i].inUse)
			n++;
	return n;
}

void ZoneRetentionLedgerAddReader(ZoneRetentionLedger* l, int cellX, int cellY)
{
	ZoneRetentionLedgerGet(l, cellX, cellY)->readerPinCount++;
}

void ZoneRetentionLedgerReleaseReader(ZoneRetentionLedger* l, int cellX, int cellY)
{
	ZoneRetentionEntry* e = ZoneRetentionLedgerGet(l, cellX, cellY);
	if (e->readerPinCount > 0)
		e->readerPinCount--;
}

double ZoneRetentionGraceSecondsForLevel(int level)
{
	if (level <= 0) return 30.0;
	if (level == 1) return 60.0;
	return 120.0;
}

bool ZoneRetentionIsRapidRevisit(double lastEvictedAt, double now)
{
	if (lastEvictedAt < 0.0)
		return false;
	double dt = now - lastEvictedAt;
	return dt >= 0.0 && dt <= 60.0;
}

double ZoneRetentionLedgerNoteRevisit(ZoneRetentionLedger* l, int cellX, int cellY, double now)
{
	ZoneRetentionEntry* e = ZoneRetentionLedgerGet(l, cellX, cellY);
	if (ZoneRetentionIsRapidRevisit(e->lastEvictedAt, now))
	{
		if (e->revisitBackoffLevel < 2)
			e->revisitBackoffLevel++;
	}
	return ZoneRetentionGraceSecondsForLevel(e->revisitBackoffLevel);
}

void ZoneRetentionLedgerNoteEvicted(ZoneRetentionLedger* l, int cellX, int cellY, double now)
{
	ZoneRetentionLedgerGet(l, cellX, cellY)->lastEvictedAt = now;
}

bool ZoneRetentionNativeWouldExpireThisFrame(double camera, double player, double town, double frameDelta)
{
	bool a = camera <= 0.0 || camera <= frameDelta;
	bool b = player <= 0.0 || player <= frameDelta;
	bool c = town   <= 0.0 || town   <= frameDelta;
	return a && b && c;
}

double ZoneRetentionKeepDeadline(const ZoneRetentionEntry* e, double now)
{
	double deadline = 0.0;
	if (e->minResidenceDeadline > now && e->minResidenceDeadline > deadline)
		deadline = e->minResidenceDeadline;
	if (e->graceDeadline > now && e->graceDeadline > deadline)
		deadline = e->graceDeadline;
	if (e->predictionLeaseDeadline > now && e->predictionLeaseDeadline > deadline)
		deadline = e->predictionLeaseDeadline;
	return deadline;
}

bool ZoneRetentionPolicyWantsHold(const ZoneRetentionEntry* e, double now, bool anchorsReadable)
{
	if (!e->inUse)
		return false;
	if (e->readerPinCount > 0)
		return true;
	if (!anchorsReadable)
		return true;
	return ZoneRetentionKeepDeadline(e, now) > now;
}

int ZoneRetentionLedgerCollectEvictionCandidates(const ZoneRetentionLedger* l, double now, bool anchorsReadable,
                                                 ZoneIdentity* out, int cap)
{
	int n = 0;
	for (int i = 0; i < ZONE_GRID_CELLS && n < cap; ++i)
	{
		const ZoneRetentionEntry* e = &l->entries[i];
		if (e->inUse && !ZoneRetentionPolicyWantsHold(e, now, anchorsReadable))
			out[n++] = e->identity;
	}
	return n;
}
