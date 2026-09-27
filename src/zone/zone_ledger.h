#pragma once
// Umbrella header for the zone-lifecycle data layer: identity and the state
// machine (zone_ledger_core.h) plus the three ledgers the design calls for
// (preparation ownership, navmesh claims/certificates, retention leases) and
// the adoption-latency accumulators. Each piece documents its own threading
// contract; this header exists only so a consumer can include one file.
//
// Pure logic: no game or KenshiLib headers, no allocation, host-tested under
// tools/tests/zone_ledger_units.cpp. Nothing in the plugin calls this yet.
//
// World-reset entry points are not identical across the three: only
// ZoneNavLedgerClearAll takes a `newWorldEpoch` argument, because only
// ledger 2 gates claims on a ledger-wide admission epoch (section 7).
// ZonePrepLedgerClearAll and ZoneRetentionLedgerClearAll take no epoch —
// they wipe unconditionally — because ledgers 1 and 3 carry the world
// epoch only inside each cell's own ZoneIdentity, not as a ledger-wide gate.

#include "zone/zone_ledger_core.h"
#include "zone/handoff/zone_prep_ledger.h"
#include "zone/geometry/zone_nav_ledger.h"
#include "zone/retention/zone_retention_ledger.h"
#include "zone/handoff/zone_adoption_latency.h"
