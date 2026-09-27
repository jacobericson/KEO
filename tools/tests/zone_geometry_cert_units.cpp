#include <cstdio>
#include "zone/geometry/zone_geometry_cert.h"
#include "zone/geometry/zone_nav_recovery.h"

#include "check.h"

static ZoneGeometrySnapshot Snap(unsigned epoch, unsigned depth)
{
	ZoneGeometrySnapshot s;
	s.epoch = epoch;
	s.mutationDepth = depth;
	return s;
}


// =========================================================================
// The capture fence
// =========================================================================

static void TestCaptureFence()
{
	// The quiet case: nothing moved before, during or after the read.
	ZoneGeometryCertificate quiet = ZoneGeometryCertificateCapture(Snap(7, 0), 12, 34);
	Check(quiet.valid, "a capture taken with nothing in flight is valid");
	Check(quiet.capturedEpoch == 7, "the capture records the epoch it saw");
	Check(ZoneGeometryCertificateCheck(quiet, Snap(7, 0), 12, 34) == ZONE_CERT_CURRENT,
	      "an unmoved epoch with nothing in flight is current");

	// A mutation running when the read began: the generation may have seen
	// it half applied, so the certificate is refused however the epoch
	// later reads -- including the case where it settles back to the same
	// value as far as this cell is concerned.
	ZoneGeometryCertificate raced = ZoneGeometryCertificateCapture(Snap(7, 1), 12, 34);
	Check(!raced.valid, "a capture taken while a mutation is in flight is invalid");
	Check(ZoneGeometryCertificateCheck(raced, Snap(7, 0), 12, 34) == ZONE_CERT_CAPTURE_RACED,
	      "a raced capture stays refused even when the store point is quiet");
	Check(ZoneGeometryCertificateCheck(raced, Snap(9, 0), 12, 34) == ZONE_CERT_CAPTURE_RACED,
	      "a raced capture is reported by its earliest fault, not by the epoch");

	// A mutation that began and completed inside the window moves the epoch.
	Check(ZoneGeometryCertificateCheck(quiet, Snap(8, 0), 12, 34) == ZONE_CERT_EPOCH_MOVED,
	      "a mutation completing during the read is caught by the epoch");

	// A mutation that began during the read and is still running shows a
	// depth at the store point, with the epoch not yet moved.
	Check(ZoneGeometryCertificateCheck(quiet, Snap(7, 1), 12, 34) == ZONE_CERT_STORE_RACED,
	      "a mutation still running at the store point is caught by the depth");

	// A mutation spanning the whole window shows a depth at both ends, and
	// the capture end is the one reported.
	ZoneGeometryCertificate spanned = ZoneGeometryCertificateCapture(Snap(7, 1), 12, 34);
	Check(ZoneGeometryCertificateCheck(spanned, Snap(7, 1), 12, 34) == ZONE_CERT_CAPTURE_RACED,
	      "a mutation spanning the window is caught at capture");

	// Nested mutations: the depth is a count, so an inner one completing
	// while an outer one runs still leaves a depth behind.
	Check(ZoneGeometryCertificateCheck(quiet, Snap(8, 1), 12, 34) == ZONE_CERT_EPOCH_MOVED,
	      "a moved epoch is reported ahead of a still-running mutation");
}

static void TestCertificateIdentity()
{
	ZoneGeometryCertificate none = ZoneGeometryCertificateNone();
	Check(ZoneGeometryCertificateCheck(none, Snap(0, 0), 3, 4) == ZONE_CERT_NO_CAPTURE,
	      "a result with no certificate is refused, not passed");
	Check(ZoneGeometryCertificateCheck(none, Snap(5, 0), 0, 0) == ZONE_CERT_NO_CAPTURE,
	      "the no-capture answer does not depend on the cell or the epoch");

	// A thread that generated for one cell and stored for another must not
	// carry its previous job's certificate across.
	ZoneGeometryCertificate other = ZoneGeometryCertificateCapture(Snap(7, 0), 12, 34);
	Check(ZoneGeometryCertificateCheck(other, Snap(7, 0), 12, 35) == ZONE_CERT_WRONG_CELL,
	      "a certificate for another cell is refused even when the epoch matches");
	Check(ZoneGeometryCertificateCheck(other, Snap(7, 0), 13, 34) == ZONE_CERT_WRONG_CELL,
	      "the cell check covers both axes");
}


// =========================================================================
// The mode: both arms live, only one reachable in production
// =========================================================================

static void TestModeArms()
{
	// Arm 1 -- what every build ships with. Every verdict is counted and
	// none is acted on, so the store point decides exactly what it decided
	// before the certificate existed.
	Check(!ZoneGeometryRejectsStore(ZONE_GEOMETRY_CONTENT_ONLY, ZONE_CERT_CURRENT),
	      "content-only publishes a current certificate");
	Check(!ZoneGeometryRejectsStore(ZONE_GEOMETRY_CONTENT_ONLY, ZONE_CERT_EPOCH_MOVED),
	      "content-only publishes a stale result too: it asserts nothing about the mesh");
	Check(!ZoneGeometryRejectsStore(ZONE_GEOMETRY_CONTENT_ONLY, ZONE_CERT_NO_CAPTURE),
	      "content-only publishes an uncertified result");

	// Arm 2 -- unreachable from the production call site, which reads a mode
	// no configuration can set to this value. Exercised here so the arm is
	// tested code rather than dead code: the single constant whose flip
	// makes the production site reach it is ZONE_GEOMETRY_ALLOW_LATE_ADOPT.
	Check(!ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_CURRENT),
	      "late adoption publishes a current certificate");
	Check(ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_EPOCH_MOVED),
	      "late adoption refuses a result whose inputs moved");
	Check(ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_CAPTURE_RACED),
	      "late adoption refuses a result whose capture raced");
	Check(ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_STORE_RACED),
	      "late adoption refuses a result stored into a running mutation");
	Check(ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_NO_CAPTURE),
	      "late adoption refuses an uncertified result");
	Check(ZoneGeometryRejectsStore(ZONE_GEOMETRY_LATE_ADOPT, ZONE_CERT_WRONG_CELL),
	      "late adoption refuses another cell's certificate");

	Check(ZONE_GEOMETRY_ALLOW_LATE_ADOPT == 0, "the late-adoption mode is fenced off");
}

static void TestModeParsing()
{
	ZoneGeometryMode m = ZONE_GEOMETRY_LATE_ADOPT;
	Check(ZoneGeometryModeFromName("contentOnly", &m), "contentOnly parses");
	Check(m == ZONE_GEOMETRY_CONTENT_ONLY, "contentOnly parses to the content-only mode");

	m = ZONE_GEOMETRY_CONTENT_ONLY;
	Check(!ZoneGeometryModeFromName("lateAdopt", &m), "lateAdopt is refused by name");
	Check(m == ZONE_GEOMETRY_CONTENT_ONLY, "a refused name leaves the mode alone");
	Check(!ZoneGeometryModeFromName("", &m), "an empty value is refused");
	Check(!ZoneGeometryModeFromName(0, &m), "a null name is refused");

	// The reason split has to survive into a log line, or a session reports
	// one number nobody can act on.
	Check(ZoneCertStalenessName(ZONE_CERT_EPOCH_MOVED)[0] != '?', "every verdict has a name");
	Check(ZoneCertStalenessName(ZONE_CERT_STORE_RACED)[0] != '?', "the store-race verdict has a name");
}


// =========================================================================
// The invalidation recovery machine
// =========================================================================

static const int RX = 5, RY = 6;

static ZoneIdentity Ident(unsigned worldEpoch, unsigned incarnation)
{
	ZoneIdentity id;
	id.worldEpoch = worldEpoch;
	id.cellX = (unsigned char)RX;
	id.cellY = (unsigned char)RY;
	id.contentIncarnation = incarnation;
	return id;
}

static void TestRecoveryOrdering()
{
	ZoneNavLedger ledger;
	ZoneNavLedgerInit(&ledger, 1);
	ZoneNavRecovery rec;
	ZoneNavRecoveryInit(&rec, 5.0);

	Check(ZoneNavLedgerClaim(&ledger, Ident(1, 1), RX, RY), "a fresh cell can be claimed");
	Check(ZoneNavLedgerMarkResultReady(&ledger, RX, RY), "the result is marked ready");
	Check(ZoneNavLedgerInstall(&ledger, RX, RY), "the result installs");

	// Step 1.
	Check(ZoneNavRecoveryBegin(&rec, &ledger, RX, RY, 0.0), "recovery begins");
	Check(!ZoneNavRecoveryBegin(&rec, &ledger, RX, RY, 0.0), "recovery refuses to begin twice");
	Check(ZoneNavRecoveryStepOf(&rec, RX, RY) == ZONE_RECOVERY_QUIESCING, "step 1 quiesces");
	Check(ZoneNavLedgerGetConst(&ledger, RX, RY)->claimState == NAV_CLAIM_STALE,
	      "step 1 marks the installed claim stale");

	// The window the ledger alone cannot close: between the invalidation and
	// the removal request the entry is merely stale, so the ledger would let
	// a claim through. The driver refuses from the first step.
	Check(ZoneNavRecoveryClaimAllowed(&rec, RX, RY) == false,
	      "a claim is refused while the cell is quiescing");
	Check(ZoneNavLedgerClaim(&ledger, Ident(1, 2), RX, RY),
	      "the ledger alone would still permit that claim -- which is why the driver must not");
	// Put the entry back where the machine left it: the line above is a
	// demonstration, not part of the sequence.
	ZoneNavLedgerInvalidate(&ledger, RX, RY);

	// Step 2 waits for the readers of the stale output.
	ZoneNavLedgerAddReader(&ledger, RX, RY);
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 1.0) == ZONE_RECOVERY_QUIESCING,
	      "a held reader pin keeps the cell quiescing");
	Check(!ZoneNavRecoveryMayFreeResult(&rec, &ledger, RX, RY),
	      "nothing may be freed while a reader holds the pin");
	ZoneNavLedgerReleaseReader(&ledger, RX, RY);

	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 1.0) == ZONE_RECOVERY_REMOVAL_REQUESTED,
	      "step 2 requests removal once the readers are done");
	Check(ZoneNavLedgerGetConst(&ledger, RX, RY)->claimState == NAV_CLAIM_REMOVAL_PENDING,
	      "the ledger records the pending removal");
	Check(!ZoneNavLedgerClaim(&ledger, Ident(1, 3), RX, RY),
	      "the ledger now refuses a claim on its own account too");
	Check(!ZoneNavRecoveryMayFreeResult(&rec, &ledger, RX, RY),
	      "nothing may be freed before the removal is acknowledged");

	// Step 3 does not advance on its own; the acknowledgement is the event.
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 2.0) == ZONE_RECOVERY_REMOVAL_REQUESTED,
	      "an unacknowledged removal holds the cell where it is");
	Check(ZoneNavRecoveryRegistrationHeld(&rec, RX, RY),
	      "registration is held until the removal is acknowledged");

	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, true, 3.0) == ZONE_RECOVERY_REMOVAL_ACKED,
	      "step 3 acknowledges the removal");
	Check(ZoneNavLedgerRemovalAcknowledged(&ledger, RX, RY), "the ledger agrees");
	Check(ZoneNavRecoveryMayFreeResult(&rec, &ledger, RX, RY),
	      "the stale output may be freed once removal is acknowledged and no reader holds it");
	Check(!ZoneNavRecoveryRegistrationHeld(&rec, RX, RY), "registration is no longer held");

	// Step 4, then the reopen.
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, true, 4.0) == ZONE_RECOVERY_REREGISTER,
	      "step 4 hands the cell to registration");
	Check(!ZoneNavRecoveryClaimAllowed(&rec, RX, RY),
	      "the cell stays closed to claims until recovery is completed");
	Check(ZoneNavRecoveryComplete(&rec, RX, RY), "recovery completes");
	Check(ZoneNavRecoveryClaimAllowed(&rec, RX, RY), "the cell reopens to claims");
	Check(ZoneNavRecoveryStepOf(&rec, RX, RY) == ZONE_RECOVERY_NONE, "the record is cleared");
	Check(rec.completed == 1 && rec.begun == 1 && rec.faulted == 0, "the counters agree");
}

static void TestRecoveryOutOfOrderInjection()
{
	// Injection 1: acknowledge a removal that was never requested. The
	// ledger refuses and counts it; nothing changes state.
	ZoneNavLedger ledger;
	ZoneNavLedgerInit(&ledger, 1);
	Check(ZoneNavLedgerClaim(&ledger, Ident(1, 1), RX, RY), "claim taken");
	unsigned before = ZoneNavLedgerRefusedAttempts(&ledger, RX, RY);
	Check(!ZoneNavLedgerAcknowledgeRemoval(&ledger, RX, RY),
	      "an acknowledgement with no request is refused");
	Check(ZoneNavLedgerRefusedAttempts(&ledger, RX, RY) == before + 1,
	      "the refusal leaves a trace");
	Check(ZoneNavLedgerGetConst(&ledger, RX, RY)->claimState == NAV_CLAIM_ACTIVE,
	      "the refused acknowledgement changed nothing");

	// Injection 2: the engine claims to have acknowledged a removal the
	// ledger has no record of. The machine faults rather than pretending the
	// sector is gone.
	ZoneNavRecovery rec;
	ZoneNavRecoveryInit(&rec, 5.0);
	Check(ZoneNavRecoveryBegin(&rec, &ledger, RX, RY, 0.0), "recovery begins");
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 1.0) == ZONE_RECOVERY_REMOVAL_REQUESTED,
	      "removal requested");
	// Take the pending state away behind the machine's back.
	ZoneNavLedgerGet(&ledger, RX, RY)->claimState = NAV_CLAIM_STALE;
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, true, 2.0) == ZONE_RECOVERY_FAULTED,
	      "a disagreement between the two sides faults instead of advancing");
	Check(!ZoneNavRecoveryMayFreeResult(&rec, &ledger, RX, RY),
	      "a faulted cell never permits freeing");
	Check(!ZoneNavRecoveryClaimAllowed(&rec, RX, RY), "a faulted cell stays closed to claims");
	Check(ZoneNavRecoveryRegistrationHeld(&rec, RX, RY), "a faulted cell holds registration");
	Check(!ZoneNavRecoveryComplete(&rec, RX, RY), "a faulted cell cannot be completed away");
	Check(rec.faulted == 1, "the fault is counted");
}

static void TestRecoveryTimeout()
{
	ZoneNavLedger ledger;
	ZoneNavLedgerInit(&ledger, 1);
	ZoneNavRecovery rec;
	ZoneNavRecoveryInit(&rec, 5.0);
	Check(ZoneNavLedgerClaim(&ledger, Ident(1, 1), RX, RY), "claim taken");
	Check(ZoneNavRecoveryBegin(&rec, &ledger, RX, RY, 0.0), "recovery begins");

	// A reader that never lets go. The machine gives up on schedule and
	// preserves the live state rather than freeing under it.
	ZoneNavLedgerAddReader(&ledger, RX, RY);
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 4.9) == ZONE_RECOVERY_QUIESCING,
	      "the cell is still quiescing just inside the timeout");
	Check(ZoneNavRecoveryAdvance(&rec, &ledger, RX, RY, true, false, 5.2) == ZONE_RECOVERY_FAULTED,
	      "the cell faults just outside it");
	Check(!ZoneNavRecoveryMayFreeResult(&rec, &ledger, RX, RY),
	      "the timeout never grants permission to free");
	Check(rec.faulted == 1, "the timeout is counted as a fault");

	// And the blocked-claim counter records that something asked.
	unsigned blockedBefore = rec.blockedClaims;
	ZoneNavRecoveryClaimAllowed(&rec, RX, RY);
	Check(rec.blockedClaims == blockedBefore + 1, "a refused claim is counted");
}

int main()
{
	TestCaptureFence();
	TestCertificateIdentity();
	TestModeArms();
	TestModeParsing();
	TestRecoveryOrdering();
	TestRecoveryOutOfOrderInjection();
	TestRecoveryTimeout();

	return CheckExit("zone_geometry_cert_units");
}
