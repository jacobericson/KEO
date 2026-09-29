#include "zone/handoff/zone_prep_ledger.h"
#include "zone_prep_ledger_test_writer.h"

// Test-only stand-in for a separately compiled writer of the shared
// g_zonePrepLedger instance, calling the host suites' direct writer.
// Compiled as its own translation unit, seeing only zone_prep_ledger.h and
// that writer's declaration, so that a round-trip through it proves the
// linker resolved one instance of g_zonePrepLedger, not two.
void ZoneReadinessTestWriterPublish(int gx, int gy, int cls)
{
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, gx, gy, cls);
}
