#include "zone/readiness/zone_readiness_ledger_bridge.h"
#include "zone/zone_ledger_core.h"   // ZoneReadinessClass; header-clean, safe below the step gate too

#if ZONEHAND_STEP >= 2

#include "zone/handoff/zone_prep_ledger.h"   // declares the shared g_zonePrepLedger instance

int ZoneReadinessBridgeClassOf(int gx, int gy)
{
	if (gx < 0 || gx >= ZONE_GRID_DIM || gy < 0 || gy >= ZONE_GRID_DIM)
		return ZONE_CLASS_NONE;
	return ZonePrepLedgerReadClass(&g_zonePrepLedger, gx, gy);
}

#else   // ZONEHAND_STEP < 2: no ledger yet, so every cell is unknown

int ZoneReadinessBridgeClassOf(int /*gx*/, int /*gy*/) { return ZONE_CLASS_NONE; }

#endif
