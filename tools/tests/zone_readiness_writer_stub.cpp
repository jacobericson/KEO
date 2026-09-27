#include "zone/handoff/zone_prep_ledger.h"

// Test-only stand-in for "the writer" (a separately compiled unit -- the
// preparation/adoption code -- that will call ZonePrepLedgerPublishClass on
// the shared g_zonePrepLedger instance). Compiled as its own translation
// unit, seeing only zone_prep_ledger.h, so that a round-trip through it
// proves the linker resolved one instance of g_zonePrepLedger, not two.
void ZoneReadinessTestWriterPublish(int gx, int gy, int cls)
{
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, gx, gy, cls);
}
