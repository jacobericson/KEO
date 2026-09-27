#include "zone/handoff/zone_prep_ledger.h"

ZonePrepLedger g_zonePrepLedger;   // BSS-zeroed: every cell ZONE_CLASS_NONE, bypass false

static void ClearEntry(ZonePrepEntry* e)
{
	e->identity.worldEpoch = 0;
	e->identity.cellX = 0;
	e->identity.cellY = 0;
	e->identity.contentIncarnation = 0;
	e->state = ZONE_STATE_UNLOADED;
	e->nextInitStep = 0;
	e->pendingAdoption = false;
	e->inUse = false;
	e->everUsed = false;
	e->enteredStateAt = 0.0;
	e->illegalAttempts = 0;
}

// Release keeps `contentIncarnation` and `everUsed` so the next Begin bumps
// the incarnation forward instead of restarting the cell's identity at 0.
static void ReleaseEntry(ZonePrepEntry* e)
{
	unsigned keepIncarnation = e->identity.contentIncarnation;
	bool     keepEverUsed = e->everUsed;
	ClearEntry(e);
	e->identity.contentIncarnation = keepIncarnation;
	e->everUsed = keepEverUsed;
}

void ZonePrepLedgerInit(ZonePrepLedger* l)
{
	for (int i = 0; i < ZONE_GRID_CELLS; ++i)
	{
		ClearEntry(&l->entries[i]);
		l->classWord[i] = ZONE_CLASS_NONE;
	}
	for (int s = 0; s < ZONE_STATE_COUNT; ++s)
		l->countByState[s] = 0;
	l->countByState[ZONE_STATE_UNLOADED] = ZONE_GRID_CELLS;
	l->globalGameOwnedBypass = false;
}

ZonePrepEntry* ZonePrepLedgerGet(ZonePrepLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

const ZonePrepEntry* ZonePrepLedgerGetConst(const ZonePrepLedger* l, int cellX, int cellY)
{
	return &l->entries[ZoneCellIndex(cellX, cellY)];
}

bool ZonePrepLedgerBegin(ZonePrepLedger* l, int cellX, int cellY, unsigned worldEpoch, double now)
{
	ZonePrepEntry* e = ZonePrepLedgerGet(l, cellX, cellY);
	if (e->inUse && e->state != ZONE_STATE_UNLOADED)
	{
		e->illegalAttempts++;
		return false;
	}

	unsigned nextIncarnation = e->everUsed ? e->identity.contentIncarnation + 1 : 0;
	l->countByState[e->state]--;

	e->identity.worldEpoch = worldEpoch;
	e->identity.cellX = (unsigned char)cellX;
	e->identity.cellY = (unsigned char)cellY;
	e->identity.contentIncarnation = nextIncarnation;
	e->state = ZONE_STATE_PRIVATE_SHELL;
	e->nextInitStep = 0;
	e->pendingAdoption = false;
	e->inUse = true;
	e->everUsed = true;
	e->enteredStateAt = now;
	e->illegalAttempts = 0;

	l->countByState[ZONE_STATE_PRIVATE_SHELL]++;
	return true;
}

bool ZonePrepLedgerSetState(ZonePrepLedger* l, int cellX, int cellY, int toState, double now)
{
	ZonePrepEntry* e = ZonePrepLedgerGet(l, cellX, cellY);
	if (!e->inUse || !ZoneLifecycleCanTransition(e->state, toState))
	{
		e->illegalAttempts++;
		return false;
	}

	l->countByState[e->state]--;
	e->state = toState;
	e->enteredStateAt = now;
	l->countByState[toState]++;

	if (toState == ZONE_STATE_NATIVE_A)
		e->pendingAdoption = false;   // takeover/admission is exactly the request this flag records

	return true;
}

void ZonePrepLedgerSetPendingAdoption(ZonePrepLedger* l, int cellX, int cellY, bool pending)
{
	ZonePrepLedgerGet(l, cellX, cellY)->pendingAdoption = pending;
}

void ZonePrepLedgerSetNextInitStep(ZonePrepLedger* l, int cellX, int cellY, int step)
{
	ZonePrepLedgerGet(l, cellX, cellY)->nextInitStep = step;
}

void ZonePrepLedgerObserveCell(ZonePrepLedger* l, int cellX, int cellY, bool f176, bool f177, bool inSetB, double now)
{
	ZonePrepEntry* e = ZonePrepLedgerGet(l, cellX, cellY);
	if (!e->inUse)
		return;

	if (e->state == ZONE_STATE_NATIVE_A && inSetB)
		ZonePrepLedgerSetState(l, cellX, cellY, ZONE_STATE_NATIVE_B_LOADING, now);
	else if (e->state == ZONE_STATE_NATIVE_B_LOADING && f177)
		ZonePrepLedgerSetState(l, cellX, cellY, ZONE_STATE_NATIVE_ACTIVE, now);
	else if (e->state == ZONE_STATE_NATIVE_A && f177)
		// Rare but legal: a cohort mate's phase-4 publish can land on this
		// cell the same cycle it joined Set B, before an observe call saw
		// the intermediate frame. Step through NativeBLoading rather than
		// widen the transition table.
		if (ZonePrepLedgerSetState(l, cellX, cellY, ZONE_STATE_NATIVE_B_LOADING, now))
			ZonePrepLedgerSetState(l, cellX, cellY, ZONE_STATE_NATIVE_ACTIVE, now);

	(void)f176;
}

bool ZonePrepLedgerRelease(ZonePrepLedger* l, int cellX, int cellY)
{
	int idx = ZoneCellIndex(cellX, cellY);
	ZonePrepEntry* e = &l->entries[idx];
	if (!e->inUse || e->state != ZONE_STATE_RETIRING)
	{
		e->illegalAttempts++;
		return false;
	}

	l->countByState[e->state]--;
	ReleaseEntry(e);
	l->countByState[ZONE_STATE_UNLOADED]++;
	l->classWord[idx] = ZONE_CLASS_NONE;
	return true;
}

void ZonePrepLedgerClearAll(ZonePrepLedger* l)
{
	ZonePrepLedgerInit(l);
}

void ZonePrepLedgerPublishClass(ZonePrepLedger* l, int cellX, int cellY, int cls)
{
	l->classWord[ZoneCellIndex(cellX, cellY)] = (unsigned char)cls;
}

int ZonePrepLedgerReadClass(const ZonePrepLedger* l, int cellX, int cellY)
{
	return l->classWord[ZoneCellIndex(cellX, cellY)];
}

void ZonePrepLedgerSetGameOwnedBypass(ZonePrepLedger* l, bool on)
{
	l->globalGameOwnedBypass = on;
}

bool ZonePrepLedgerGameOwnedBypass(const ZonePrepLedger* l)
{
	return l->globalGameOwnedBypass;
}

int ZonePrepLedgerCollectReady(const ZonePrepLedger* l, ZoneIdentity* out, int cap)
{
	int n = 0;
	for (int i = 0; i < ZONE_GRID_CELLS && n < cap; ++i)
	{
		if (l->entries[i].inUse && l->entries[i].state == ZONE_STATE_READY_FOR_ADOPTION)
			out[n++] = l->entries[i].identity;
	}
	return n;
}

int ZonePrepLedgerCountInState(const ZonePrepLedger* l, int state)
{
	if (state < 0 || state >= ZONE_STATE_COUNT)
		return 0;
	return l->countByState[state];
}
