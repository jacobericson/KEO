#include <cstdio>
#include "zone/readiness/zone_readiness_contract.h"
#include "zone/zone_ledger_core.h"
#include "zone/handoff/zone_prep_ledger.h"   // g_zonePrepLedger, for the cross-TU round-trip below

// Compiled in zone_readiness_writer_stub.cpp, a separate translation unit
// that includes only zone_prep_ledger.h -- standing in for the future
// preparation/adoption writer.
extern void ZoneReadinessTestWriterPublish(int gx, int gy, int cls);

#include "check.h"

static const char* AnswerName(ZoneReadinessAnswer a)
{
	switch (a)
	{
	case ZONE_READY_ANSWER_ORIGINAL:     return "ORIGINAL";
	default:                             return "TODAY_BYPASS";
	}
}

static void ExpectDecide(int cellClass, bool bypass, ZoneReadinessAnswer expect, const char* what)
{
	ZoneReadinessAnswer got = ZoneReadinessContractDecide(cellClass, bypass);
	if (got != expect)
	{
		printf("FAIL %s (got %s, want %s)\n", what, AnswerName(got), AnswerName(expect));
		Check(false, what);
	}
}

static const char* BucketName(ZoneReadinessBucket b)
{
	switch (b)
	{
	case ZONE_READY_BUCKET_PRIVATE: return "PRIVATE";
	case ZONE_READY_BUCKET_ADOPTED: return "ADOPTED";
	case ZONE_READY_BUCKET_GLOBAL:  return "GLOBAL";
	default:                        return "NONE";
	}
}

static void ExpectBucket(int cellClass, bool bypass, ZoneReadinessBucket expect, const char* what)
{
	ZoneReadinessBucket got = ZoneReadinessContractBucket(cellClass, bypass);
	if (got != expect)
	{
		printf("FAIL %s (got %s, want %s)\n", what, BucketName(got), BucketName(expect));
		Check(false, what);
	}
}

int main()
{
	// The full class x bypass matrix, exhaustively: 3 classes x 2 bypass
	// states. While ZM+8 is set, every class hears the original -- the
	// global bypass wins over the per-cell class, checked first.
	ExpectDecide(ZONE_CLASS_NONE,       true, ZONE_READY_ANSWER_ORIGINAL, "bypass set: unclassified cell hears the original");
	ExpectDecide(ZONE_CLASS_PRIVATE,    true, ZONE_READY_ANSWER_ORIGINAL, "bypass set: private cell hears the original");
	ExpectDecide(ZONE_CLASS_ADOPTED,    true, ZONE_READY_ANSWER_ORIGINAL, "bypass set: adopted cell hears the original");

	// Bypass clear: the per-class contract applies. A cell the mod never
	// reached is the game's own to load and keeps today's bypass.
	ExpectDecide(ZONE_CLASS_PRIVATE,    false, ZONE_READY_ANSWER_ORIGINAL,     "private cell hears the original");
	ExpectDecide(ZONE_CLASS_ADOPTED,    false, ZONE_READY_ANSWER_ORIGINAL,     "adopted cell hears the original");
	ExpectDecide(ZONE_CLASS_NONE,       false, ZONE_READY_ANSWER_TODAY_BYPASS, "unclassified cell keeps today's bypass");

	// A class value the enum does not have is treated the same as
	// unclassified, not as a crash or an assumption about a class that does
	// not exist -- both with and without the global bypass.
	ExpectDecide(99, false, ZONE_READY_ANSWER_TODAY_BYPASS, "out-of-range class keeps today's bypass (bypass clear)");
	ExpectDecide(99, true,  ZONE_READY_ANSWER_ORIGINAL,     "out-of-range class still hears the original under the bypass");
	ExpectDecide(-1, false, ZONE_READY_ANSWER_TODAY_BYPASS, "negative class keeps today's bypass (bypass clear)");

	// Decisive check for single-instance wiring: publish a class from one
	// translation unit (zone_readiness_writer_stub.cpp, standing in for the
	// preparation/adoption writer) and read it back through the same read
	// path ZoneReadinessBridgeClassOf uses (ZonePrepLedgerReadClass on
	// g_zonePrepLedger -- ZoneReadinessBridgeClassOf itself needs config.h,
	// unreachable from a host test, so this exercises everything past its
	// trivial bounds-check wrapper). A grep for one definition (zone_prep_
	// ledger.cpp) and one extern (zone_prep_ledger.h) is necessary but not
	// sufficient; this is the test that would fail if a second g_zonePrep
	// Ledger were declared anywhere and the linker picked one per TU.
	Check(ZonePrepLedgerReadClass(&g_zonePrepLedger, 5, 6) == ZONE_CLASS_NONE,
		"cross-TU: an unwritten cell starts unknown");
	ZoneReadinessTestWriterPublish(5, 6, ZONE_CLASS_ADOPTED);
	Check(ZonePrepLedgerReadClass(&g_zonePrepLedger, 5, 6) == ZONE_CLASS_ADOPTED,
		"cross-TU: a publish from the writer stub is read back through the shared instance");
	ZoneReadinessTestWriterPublish(5, 6, ZONE_CLASS_PRIVATE);
	Check(ZonePrepLedgerReadClass(&g_zonePrepLedger, 5, 6) == ZONE_CLASS_PRIVATE,
		"cross-TU: a second publish overwrites through the same shared instance");

	// The bucket predicate: which class the contract saw. Both directions of
	// the private/adopted pair are asserted, so swapping the two arms fails
	// here instead of mislabelling a counter in a session nobody can re-run.
	ExpectBucket(ZONE_CLASS_PRIVATE, false, ZONE_READY_BUCKET_PRIVATE, "private cell counts private");
	ExpectBucket(ZONE_CLASS_ADOPTED, false, ZONE_READY_BUCKET_ADOPTED, "adopted cell counts adopted");
	ExpectBucket(ZONE_CLASS_NONE,    false, ZONE_READY_BUCKET_NONE,    "unclassified cell counts nothing");
	ExpectBucket(99,                 false, ZONE_READY_BUCKET_NONE,    "out-of-range class counts nothing");
	ExpectBucket(ZONE_CLASS_PRIVATE, true,  ZONE_READY_BUCKET_GLOBAL,  "bypass set: private cell counts global");
	ExpectBucket(ZONE_CLASS_ADOPTED, true,  ZONE_READY_BUCKET_GLOBAL,  "bypass set: adopted cell counts global");
	ExpectBucket(ZONE_CLASS_NONE,    true,  ZONE_READY_BUCKET_GLOBAL,  "bypass set: unclassified cell counts global");

	// Injection end to end through the shared ledger: publish a class from
	// the writer stub's translation unit, read it back the way the hook
	// reads it, and count it. This is the path whose counters read zero
	// across two sessions; a cell written as private has to count private.
	ZoneReadinessTestWriterPublish(9, 11, ZONE_CLASS_PRIVATE);
	Check(ZoneReadinessContractBucket(ZonePrepLedgerReadClass(&g_zonePrepLedger, 9, 11), false)
		== ZONE_READY_BUCKET_PRIVATE, "injected private cell counts private");
	ZoneReadinessTestWriterPublish(9, 11, ZONE_CLASS_ADOPTED);
	Check(ZoneReadinessContractBucket(ZonePrepLedgerReadClass(&g_zonePrepLedger, 9, 11), false)
		== ZONE_READY_BUCKET_ADOPTED, "injected adopted cell counts adopted");
	Check(ZoneReadinessContractBucket(ZonePrepLedgerReadClass(&g_zonePrepLedger, 12, 12), false)
		== ZONE_READY_BUCKET_NONE, "an untouched cell counts nothing");

	return CheckExit("zone_readiness_contract_units");
}
