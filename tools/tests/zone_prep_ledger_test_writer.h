// zone_prep_ledger_test_writer.h - The class word's direct writer, declared for the host suites only.
// Production writes a cell's class only through the ledger's state changes.
#ifndef ZONEOPT_TOOLS_TESTS_ZONE_PREP_LEDGER_TEST_WRITER_H
#define ZONEOPT_TOOLS_TESTS_ZONE_PREP_LEDGER_TEST_WRITER_H

#include "zone/handoff/zone_prep_ledger.h"

void ZonePrepLedgerPublishClass(ZonePrepLedger* l, int cellX, int cellY, int cls);

#endif
