#include "zone/reset/zone_reset_gate.h"

ZoneResetGate g_zoneResetGate;

namespace zone_reset_gate_detail
{
	// The slice exists only so a waiter re-reads the stop.
	const DWORD ZONE_RESET_WAIT_SLICE_MS = 50;

	bool SiteInTable(ZoneResetSite site)
	{
		return (int)site >= 0 && (int)site < ZONE_RESET_SITE_COUNT;
	}

	bool StopSeen(bool (*stopSeen)())
	{
		return stopSeen ? stopSeen() : false;
	}

	// The stop is read before the gate, so a lower that lands while the stop
	// is being read is seen.
	ZoneResetAdmission Decide(const ZoneResetGate* g, ZoneResetSite site, bool (*stopSeen)())
	{
		bool stop = StopSeen(stopSeen);
		bool up   = ZoneResetGateUp(g);
		return ZoneResetAdmit(up, stop, site);
	}
}
using namespace zone_reset_gate_detail;

// `parked` is the caller's and is left as it is.
bool ZoneResetGateInit(ZoneResetGate* g)
{
	InterlockedExchange(&g->inProgress, 0);
	InterlockedExchange(&g->deferredTotal, 0);
	for (int i = 0; i < ZONE_RESET_SITE_COUNT; ++i)
		InterlockedExchange(&g->deferred[i], 0);
	InterlockedExchange(&g->raises, 0);
	g->over = CreateEvent(NULL, TRUE, TRUE, NULL);   // manual reset, signalled
	return g->over != NULL;
}

void ZoneResetGateRaise(ZoneResetGate* g)
{
	InterlockedExchange(&g->deferredTotal, 0);
	for (int i = 0; i < ZONE_RESET_SITE_COUNT; ++i)
		InterlockedExchange(&g->deferred[i], 0);
	// Reset before the gate reads up: a waiter that sees it up waits on an
	// unsignalled event.
	if (g->over)
		ResetEvent(g->over);
	InterlockedExchange(&g->inProgress, 1);
	InterlockedIncrement(&g->raises);
}

void ZoneResetGateLower(ZoneResetGate* g)
{
	// Down before the event is set: a waiter that wakes reads it down.
	InterlockedExchange(&g->inProgress, 0);
	if (g->over)
		SetEvent(g->over);
}

bool ZoneResetGateUp(const ZoneResetGate* g)
{
	return InterlockedCompareExchange(const_cast<volatile LONG*>(&g->inProgress), 0, 0) != 0;
}

LONG ZoneResetGateRaises(const ZoneResetGate* g)
{
	return InterlockedCompareExchange(const_cast<volatile LONG*>(&g->raises), 0, 0);
}

void ZoneResetGateNoteDeferred(ZoneResetGate* g, ZoneResetSite site)
{
	InterlockedIncrement(&g->deferredTotal);
	if (SiteInTable(site))
		InterlockedIncrement(&g->deferred[site]);
}

long ZoneResetGateDeferredTotal(const ZoneResetGate* g)
{
	return InterlockedCompareExchange(const_cast<volatile LONG*>(&g->deferredTotal), 0, 0);
}

long ZoneResetGateDeferredAt(const ZoneResetGate* g, ZoneResetSite site)
{
	if (!SiteInTable(site))
		return 0;
	return InterlockedCompareExchange(const_cast<volatile LONG*>(&g->deferred[site]), 0, 0);
}

// A waiter that read the gate up just before the lower finds `over` already
// set and returns at once: the event is manual reset, so no wake is lost.
ZoneResetWait ZoneResetGateWait(ZoneResetGate* g, ZoneResetSite site, bool (*stopSeen)())
{
	if (!ZoneResetGateUp(g))
		return ZONE_RESET_WAIT_NONE;
	if (ZoneResetAdmit(true, StopSeen(stopSeen), site) == ZONE_RESET_DEFER_STOP)
		return ZONE_RESET_WAIT_STOPPED;

	ZoneResetGateNoteDeferred(g, site);
	if (g->parked)
		SetEvent(g->parked);

	for (;;)
	{
		if (g->over)
			WaitForSingleObject(g->over, ZONE_RESET_WAIT_SLICE_MS);
		else
			Sleep(ZONE_RESET_WAIT_SLICE_MS);

		ZoneResetAdmission a = Decide(g, site, stopSeen);
		if (a == ZONE_RESET_DEFER_STOP)
			return ZONE_RESET_WAIT_STOPPED;
		if (a == ZONE_RESET_ADMIT)
			return ZONE_RESET_WAIT_WAITED;
	}
}

ZoneResetWait ZoneResetGateWaitSince(ZoneResetGate* g, ZoneResetSite site,
	bool (*stopSeen)(), LONG raisesAtClaim)
{
	bool waited = false;
	for (;;)
	{
		if (StopSeen(stopSeen))
			return ZONE_RESET_WAIT_STOPPED;
		LONG countBefore = ZoneResetGateRaises(g);
		ZoneResetWait w = ZoneResetGateWait(g, site, stopSeen);
		if (w == ZONE_RESET_WAIT_STOPPED)
			return w;
		if (w == ZONE_RESET_WAIT_WAITED)
			waited = true;
		if (StopSeen(stopSeen))
			return ZONE_RESET_WAIT_STOPPED;
		LONG countAfter = ZoneResetGateRaises(g);
		bool up = ZoneResetGateUp(g);
		LONG countStable = ZoneResetGateRaises(g);
		if (StopSeen(stopSeen))
			return ZONE_RESET_WAIT_STOPPED;
		if (up || countBefore != countAfter || countAfter != countStable)
			continue;
		return waited || countStable != raisesAtClaim
			? ZONE_RESET_WAIT_WAITED : ZONE_RESET_WAIT_NONE;
	}
}

ZoneResetGateScope::ZoneResetGateScope(ZoneResetGate* g, void (*raiseUnder)(ZoneResetGate*),
	void (*afterLowerFn)(), void (*lowerUnderFn)(ZoneResetGate*))
	: gate(g), afterLower(afterLowerFn), lowerUnder(lowerUnderFn)
{
	if (raiseUnder)
		raiseUnder(g);
	else
		ZoneResetGateRaise(g);
}

ZoneResetGateScope::~ZoneResetGateScope()
{
	if (lowerUnder)
		lowerUnder(gate);
	else
		ZoneResetGateLower(gate);
	if (afterLower)
		afterLower();
}
