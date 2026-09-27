#include <cstdio>
#include "zone/zone_ledger.h"

#include "check.h"
static bool Near(double a, double b) { double d = a - b; return d < 0.001 && d > -0.001; }

static ZoneIdentity MakeId(unsigned epoch, int x, int y, unsigned incarnation)
{
	ZoneIdentity id;
	id.worldEpoch = epoch;
	id.cellX = (unsigned char)x;
	id.cellY = (unsigned char)y;
	id.contentIncarnation = incarnation;
	return id;
}

static void TestIdentity()
{
	ZoneIdentity a = MakeId(1, 5, 6, 2);
	ZoneIdentity b = MakeId(1, 5, 6, 2);
	ZoneIdentity sameCellOlder = MakeId(1, 5, 6, 1);
	ZoneIdentity sameCellNewer = MakeId(1, 5, 6, 3);
	ZoneIdentity otherCell = MakeId(1, 5, 7, 2);
	ZoneIdentity otherEpoch = MakeId(2, 5, 6, 2);

	Check(ZoneIdentityEqual(a, b), "identical identities compare equal");
	Check(!ZoneIdentityEqual(a, sameCellOlder), "incarnation differs: not equal");
	Check(!ZoneIdentityEqual(a, otherEpoch), "epoch differs: not equal");

	Check(ZoneIdentitySameCell(a, sameCellOlder), "same cell, ignoring incarnation");
	Check(ZoneIdentitySameCell(a, sameCellNewer), "same cell, newer incarnation");
	Check(!ZoneIdentitySameCell(a, otherCell), "different cell coordinate: not the same cell");
	Check(!ZoneIdentitySameCell(a, otherEpoch), "different world epoch: not the same cell");

	Check(ZoneIdentityIsStaleAttempt(a, sameCellOlder), "older incarnation at the same cell is stale");
	Check(!ZoneIdentityIsStaleAttempt(a, sameCellNewer), "newer incarnation is not stale");
	Check(!ZoneIdentityIsStaleAttempt(a, a), "the current identity is not stale against itself");
	Check(!ZoneIdentityIsStaleAttempt(a, otherCell), "a different cell is never stale against this one");
	Check(!ZoneIdentityIsStaleAttempt(a, otherEpoch), "a different world is never stale against this one");

	Check(ZoneCellIndex(0, 0) == 0, "index (0,0) = 0");
	Check(ZoneCellIndex(1, 0) == 64, "index matches AreaSector's y + (x<<6)");
	Check(ZoneCellIndex(-1, 5) == ZoneCellIndex(0, 5), "negative x clamps");
	Check(ZoneCellIndex(5, 999) == ZoneCellIndex(5, 63), "oversized y clamps");
}

static void TestStateMachine()
{
	// Ordinary progress, one step at a time.
	Check(ZoneLifecycleCanTransition(ZONE_STATE_UNLOADED, ZONE_STATE_PRIVATE_SHELL), "Unloaded -> PrivateShell");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_SHELL, ZONE_STATE_PRIVATE_CONTENT), "Shell -> Content");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_CONTENT, ZONE_STATE_PRIVATE_NAV), "Content -> Nav");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_NAV, ZONE_STATE_READY_FOR_ADOPTION), "Nav -> ReadyForAdoption");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_READY_FOR_ADOPTION, ZONE_STATE_NATIVE_A), "ReadyForAdoption -> NativeA");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_A, ZONE_STATE_NATIVE_B_LOADING), "NativeA -> NativeBLoading");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_B_LOADING, ZONE_STATE_NATIVE_ACTIVE), "NativeBLoading -> NativeActive");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_ACTIVE, ZONE_STATE_RETIRING), "NativeActive -> Retiring");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_RETIRING, ZONE_STATE_UNLOADED), "Retiring -> Unloaded");

	// Real-demand takeover: any private stage straight to NativeA.
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_SHELL, ZONE_STATE_NATIVE_A), "takeover from PrivateShell");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_CONTENT, ZONE_STATE_NATIVE_A), "takeover from PrivateContent");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_NAV, ZONE_STATE_NATIVE_A), "takeover from PrivateNav");

	// Geometry invalidation recovery.
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_NAV, ZONE_STATE_GEOMETRY_INVALIDATED), "Nav -> Invalidated");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_READY_FOR_ADOPTION, ZONE_STATE_GEOMETRY_INVALIDATED), "ReadyForAdoption -> Invalidated");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_GEOMETRY_INVALIDATED, ZONE_STATE_PRIVATE_CONTENT), "Invalidated -> Content (retry)");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_GEOMETRY_INVALIDATED, ZONE_STATE_NATIVE_A), "Invalidated -> NativeA (native adopts mid-recovery)");

	// Private teardown: a cell the mod prepared and never handed over has to
	// be able to reach Retiring, or Release can never reclaim its slot.
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_SHELL, ZONE_STATE_RETIRING), "PrivateShell -> Retiring");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_CONTENT, ZONE_STATE_RETIRING), "PrivateContent -> Retiring");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_NAV, ZONE_STATE_RETIRING), "PrivateNav -> Retiring");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_READY_FOR_ADOPTION, ZONE_STATE_RETIRING), "ReadyForAdoption -> Retiring");
	Check(ZoneLifecycleCanTransition(ZONE_STATE_GEOMETRY_INVALIDATED, ZONE_STATE_RETIRING), "Invalidated -> Retiring");

	// Illegal: no step backward out of native ownership into a private stage.
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_ACTIVE, ZONE_STATE_PRIVATE_CONTENT), "NativeActive -> Content is illegal");
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_A, ZONE_STATE_PRIVATE_SHELL), "NativeA -> Shell is illegal");
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_NATIVE_B_LOADING, ZONE_STATE_PRIVATE_NAV), "NativeBLoading -> Nav is illegal");
	// Illegal: no skipping ahead past the immediate next stage.
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_SHELL, ZONE_STATE_PRIVATE_NAV), "Shell -> Nav skips Content");
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_UNLOADED, ZONE_STATE_NATIVE_A), "Unloaded cannot be taken over directly");
	// Illegal: resurrecting from Unloaded to anything but a fresh shell.
	Check(!ZoneLifecycleCanTransition(ZONE_STATE_UNLOADED, ZONE_STATE_READY_FOR_ADOPTION), "Unloaded -> ReadyForAdoption is illegal");
}

static void TestPrepLedger()
{
	ZonePrepLedger l;
	ZonePrepLedgerInit(&l);

	Check(ZonePrepLedgerReadClass(&l, 3, 4) == ZONE_CLASS_NONE, "untouched cell classifies as NONE");

	Check(ZonePrepLedgerBegin(&l, 3, 4, 1, 0.0), "Begin succeeds on an unused cell");
	const ZonePrepEntry* e = ZonePrepLedgerGetConst(&l, 3, 4);
	Check(e->state == ZONE_STATE_PRIVATE_SHELL, "Begin lands in PrivateShell");
	Check(e->identity.contentIncarnation == 0, "first incarnation is 0");

	Check(!ZonePrepLedgerBegin(&l, 3, 4, 1, 1.0), "Begin refuses a slot already in use");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->illegalAttempts == 1, "the refused Begin counted as illegal");

	Check(ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_PRIVATE_CONTENT, 1.0), "Shell -> Content");
	Check(!ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_NATIVE_B_LOADING, 2.0), "Content -> NativeBLoading is illegal");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->illegalAttempts == 2, "the illegal transition counted too");

	Check(ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_PRIVATE_NAV, 2.0), "Content -> Nav");
	Check(ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_READY_FOR_ADOPTION, 3.0), "Nav -> ReadyForAdoption");

	ZoneIdentity ready[8];
	int n = ZonePrepLedgerCollectReady(&l, ready, 8);
	Check(n == 1, "one cell collected as ready");
	Check(ZoneIdentityEqual(ready[0], ZonePrepLedgerGetConst(&l, 3, 4)->identity), "the collected identity matches");

	ZonePrepLedgerSetPendingAdoption(&l, 3, 4, true);
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->pendingAdoption, "pendingAdoption set");
	Check(ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_NATIVE_A, 4.0), "ReadyForAdoption -> NativeA");
	Check(!ZonePrepLedgerGetConst(&l, 3, 4)->pendingAdoption, "admission clears pendingAdoption");

	n = ZonePrepLedgerCollectReady(&l, ready, 8);
	Check(n == 0, "nothing left ready after admission");

	// Independence: a ledger-1 transition never touches ledger 2 or 3 (they
	// are separate arrays); the design forbids Set-A entry from erasing
	// outstanding nav/retention work.
	ZoneNavLedger nav;
	ZoneNavLedgerInit(&nav, 1);
	ZoneIdentity id = ZonePrepLedgerGetConst(&l, 3, 4)->identity;
	Check(ZoneNavLedgerClaim(&nav, id, 3, 4), "nav claim unaffected by ledger 1's own transitions");
	ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_NATIVE_B_LOADING, 5.0);
	Check(ZoneNavLedgerGetConst(&nav, 3, 4)->claimState == NAV_CLAIM_ACTIVE, "ledger 2 entry survives ledger 1's NativeA -> NativeBLoading");

	// Observation drives the native-owned tail.
	ZonePrepLedgerObserveCell(&l, 3, 4, true, false, true, 5.0);   // already NativeBLoading; no-op re-entry
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->state == ZONE_STATE_NATIVE_B_LOADING, "observe does not regress a state it is already at");
	ZonePrepLedgerObserveCell(&l, 3, 4, true, true, true, 6.0);
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->state == ZONE_STATE_NATIVE_ACTIVE, "observe advances NativeBLoading -> NativeActive on +177");

	// The global bypass is a single flag, not per cell.
	Check(!ZonePrepLedgerGameOwnedBypass(&l), "bypass starts off");
	ZonePrepLedgerSetGameOwnedBypass(&l, true);
	Check(ZonePrepLedgerGameOwnedBypass(&l), "bypass reads back on");

	// Release refuses anything short of Retiring, and counts the attempt.
	unsigned illegalBefore = ZonePrepLedgerGetConst(&l, 3, 4)->illegalAttempts;
	Check(!ZonePrepLedgerRelease(&l, 3, 4), "release refuses a cell still NativeActive");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->state == ZONE_STATE_NATIVE_ACTIVE, "a refused release leaves the state unchanged");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->illegalAttempts == illegalBefore + 1, "the refused release counted as illegal");

	Check(ZonePrepLedgerSetState(&l, 3, 4, ZONE_STATE_RETIRING, 6.5), "NativeActive -> Retiring");
	Check(ZonePrepLedgerRelease(&l, 3, 4), "release succeeds once Retiring");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->state == ZONE_STATE_UNLOADED, "release clears back to Unloaded");
	Check(ZonePrepLedgerReadClass(&l, 3, 4) == ZONE_CLASS_NONE, "release clears the class word");
	Check(ZonePrepLedgerBegin(&l, 3, 4, 1, 7.0), "a released slot accepts a new Begin");
	Check(ZonePrepLedgerGetConst(&l, 3, 4)->identity.contentIncarnation == 1, "the new incarnation is bumped, not reset to 0");

	// World reset wipes everything, including the global bypass.
	ZonePrepLedgerClearAll(&l);
	Check(!ZonePrepLedgerGameOwnedBypass(&l), "ClearAll resets the global bypass");
	Check(!ZonePrepLedgerGetConst(&l, 3, 4)->inUse, "ClearAll wipes every entry");
	Check(ZonePrepLedgerReadClass(&l, 3, 4) == ZONE_CLASS_NONE, "ClearAll wipes the class word too");
}

static void TestPrivateTeardownReusesTheSlot()
{
	ZonePrepLedger l;
	ZonePrepLedgerInit(&l);

	Check(ZonePrepLedgerBegin(&l, 9, 9, 1, 0.0), "first private incarnation begins");
	Check(ZonePrepLedgerSetState(&l, 9, 9, ZONE_STATE_PRIVATE_CONTENT, 1.0), "content initialized");
	Check(ZonePrepLedgerSetState(&l, 9, 9, ZONE_STATE_RETIRING, 2.0), "the mod tears down what it never handed over");
	Check(ZonePrepLedgerRelease(&l, 9, 9), "the slot is released");
	Check(ZonePrepLedgerBegin(&l, 9, 9, 1, 3.0), "the cell can be prepared again");
	Check(ZonePrepLedgerGetConst(&l, 9, 9)->identity.contentIncarnation == 1,
	      "the second attempt is a new incarnation");
}

// The class word's writer and its readers are different translation units in
// the plugin, so what matters is that they name the same object. This pins
// the shared instance's linkage: it resolves, it starts clear, and a value
// published into it reads back through the accessor.
// Registration can reach the ledger with the record still at the shell
// stage, when the game's own tick finalized the content before the mod's
// pass did. Stepping through the content stage is legal; skipping it is not,
// and a refused skip would leave the cell at PrivateShell forever, where
// nothing collects it.
static void TestRegistrationFromEitherStage()
{
	ZonePrepLedger l;
	ZonePrepLedgerInit(&l);

	Check(!ZoneLifecycleCanTransition(ZONE_STATE_PRIVATE_SHELL, ZONE_STATE_PRIVATE_NAV),
	      "Shell -> Nav is still not a legal single step");

	Check(ZonePrepLedgerBegin(&l, 20, 21, 1, 0.0), "begin");
	Check(ZonePrepLedgerSetState(&l, 20, 21, ZONE_STATE_PRIVATE_CONTENT, 1.0), "step through content");
	Check(ZonePrepLedgerSetState(&l, 20, 21, ZONE_STATE_PRIVATE_NAV, 2.0), "then nav");
	Check(ZonePrepLedgerGetConst(&l, 20, 21)->state == ZONE_STATE_PRIVATE_NAV,
	      "a cell registered from the shell stage still lands on nav");
	Check(ZonePrepLedgerGetConst(&l, 20, 21)->illegalAttempts == 0,
	      "and it gets there without a refusal");
}

static void TestSharedPrepLedgerInstance()
{
	ZonePrepLedgerInit(&g_zonePrepLedger);
	Check(ZonePrepLedgerReadClass(&g_zonePrepLedger, 12, 34) == ZONE_CLASS_NONE,
	      "the shared ledger starts unclassified");
	ZonePrepLedgerPublishClass(&g_zonePrepLedger, 12, 34, ZONE_CLASS_ADOPTED);
	Check(ZonePrepLedgerReadClass(&g_zonePrepLedger, 12, 34) == ZONE_CLASS_ADOPTED,
	      "a class published into the shared ledger reads back");
	ZonePrepLedgerInit(&g_zonePrepLedger);
}

static void TestNavLedger()
{
	ZoneNavLedger l;
	ZoneNavLedgerInit(&l, 1);

	ZoneIdentity id = MakeId(1, 2, 3, 0);
	Check(ZoneNavLedgerClaim(&l, id, 2, 3), "claim succeeds while admission is open");
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->claimState == NAV_CLAIM_ACTIVE, "claim state is ACTIVE");
	Check(ZoneNavLedgerIsCertificateCurrent(&l, 2, 3), "a fresh claim's certificate is current");

	Check(ZoneNavLedgerMarkResultReady(&l, 2, 3), "mark result ready from ACTIVE");
	Check(ZoneNavLedgerInstall(&l, 2, 3), "install succeeds while the certificate is current");
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->claimState == NAV_CLAIM_INSTALLED, "state is INSTALLED");

	// A geometry mutation after install makes a *new* claim's certificate
	// current and the *old* installed one stale-by-epoch.
	unsigned before = ZoneNavLedgerGeometryEpoch(&l);
	unsigned after = ZoneNavLedgerBumpGeometryEpoch(&l);
	Check(after == before + 1, "geometry epoch increments by exactly one");
	Check(!ZoneNavLedgerIsCertificateCurrent(&l, 2, 3), "the installed claim's certificate is stale after a geometry bump");

	// Invalidation: bumps claimGeneration, marks STALE, leaves the identity
	// (in particular contentIncarnation) untouched -- content is retained.
	unsigned genBefore = ZoneNavLedgerGetConst(&l, 2, 3)->claimGeneration;
	unsigned incarnationBefore = ZoneNavLedgerGetConst(&l, 2, 3)->identity.contentIncarnation;
	ZoneNavLedgerInvalidate(&l, 2, 3);
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->claimState == NAV_CLAIM_STALE, "invalidate marks STALE");
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->claimGeneration == genBefore + 1, "invalidate bumps claimGeneration");
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->identity.contentIncarnation == incarnationBefore, "invalidate does not bump contentIncarnation");

	Check(ZoneNavLedgerIsStaleClaim(&l, 2, 3, id, genBefore), "a claim captured before the invalidation is now stale");
	Check(!ZoneNavLedgerIsStaleClaim(&l, 2, 3, id, genBefore + 1), "a claim captured after the invalidation is current");

	// AcknowledgeRemoval refuses without a prior RequestRemoval (still STALE here),
	// and counts the refusal for a future stats line.
	unsigned refusedBefore = ZoneNavLedgerRefusedAttempts(&l, 2, 3);
	Check(!ZoneNavLedgerAcknowledgeRemoval(&l, 2, 3), "acknowledge refuses with no removal pending");
	Check(ZoneNavLedgerRefusedAttempts(&l, 2, 3) == refusedBefore + 1, "the refused acknowledge is counted");

	ZoneNavLedgerRequestRemoval(&l, 2, 3);
	Check(!ZoneNavLedgerRemovalAcknowledged(&l, 2, 3), "removal pending is not yet acknowledged");

	// Section 5 step 3's hazard: a re-claim must not be allowed to land while
	// the removal is still unacknowledged. That refusal is counted too.
	refusedBefore = ZoneNavLedgerRefusedAttempts(&l, 2, 3);
	Check(!ZoneNavLedgerClaim(&l, id, 2, 3), "a re-claim is refused while removal is pending");
	Check(ZoneNavLedgerGetConst(&l, 2, 3)->claimState == NAV_CLAIM_REMOVAL_PENDING, "the refused claim did not disturb the pending removal");
	Check(ZoneNavLedgerRefusedAttempts(&l, 2, 3) == refusedBefore + 1, "the refused claim is counted");

	Check(ZoneNavLedgerAcknowledgeRemoval(&l, 2, 3), "acknowledge succeeds once removal was requested");
	Check(ZoneNavLedgerRemovalAcknowledged(&l, 2, 3), "removal now acknowledged");
	Check(ZoneNavLedgerClaim(&l, id, 2, 3), "a claim succeeds once removal is acknowledged");
	Check(ZoneNavLedgerRefusedAttempts(&l, 2, 3) == refusedBefore + 1, "a successful claim does not add another refusal");

	// Reader pin.
	Check(ZoneNavLedgerReaderCount(&l, 2, 3) == 0, "no readers yet");
	ZoneNavLedgerAddReader(&l, 2, 3);
	ZoneNavLedgerAddReader(&l, 2, 3);
	Check(ZoneNavLedgerReaderCount(&l, 2, 3) == 2, "two readers added");
	ZoneNavLedgerReleaseReader(&l, 2, 3);
	Check(ZoneNavLedgerReaderCount(&l, 2, 3) == 1, "one reader released");
	ZoneNavLedgerReleaseReader(&l, 2, 3);
	ZoneNavLedgerReleaseReader(&l, 2, 3);
	Check(ZoneNavLedgerReaderCount(&l, 2, 3) == 0, "reader count never goes negative");

	// Admission barrier (section 7): closing it refuses new claims and a
	// late install for the closed epoch, even with a current certificate.
	ZoneNavLedgerClaim(&l, MakeId(1, 5, 5, 0), 5, 5);
	ZoneNavLedgerMarkResultReady(&l, 5, 5);
	ZoneNavLedgerCloseAdmission(&l);
	Check(!ZoneNavLedgerAdmissionOpen(&l), "admission reads closed");
	Check(ZoneNavLedgerRefusedAttempts(&l, 6, 6) == 0, "a never-touched cell starts at 0 refusals");
	Check(!ZoneNavLedgerClaim(&l, MakeId(1, 6, 6, 0), 6, 6), "a new claim is refused while admission is closed");
	Check(ZoneNavLedgerRefusedAttempts(&l, 6, 6) == 1, "the admission-closed refusal is counted against the target cell");
	Check(!ZoneNavLedgerInstall(&l, 5, 5), "a late install is refused once admission has closed");

	ZoneNavLedgerClearAll(&l, 2);
	Check(ZoneNavLedgerAdmissionOpen(&l), "ClearAll reopens admission");
	Check(ZoneNavLedgerGeometryEpoch(&l) == 0, "ClearAll resets the geometry epoch");
	Check(!ZoneNavLedgerGetConst(&l, 2, 3)->inUse, "ClearAll wipes every entry");
	Check(ZoneNavLedgerRefusedAttempts(&l, 6, 6) == 0, "ClearAll (world reset) also wipes the refusal counters");
	Check(!ZoneNavLedgerClaim(&l, MakeId(1, 2, 3, 0), 2, 3), "an old-epoch identity is refused after ClearAll");
	Check(ZoneNavLedgerClaim(&l, MakeId(2, 2, 3, 0), 2, 3), "a current-epoch identity is accepted after ClearAll");
}

static void TestRetentionLedger()
{
	ZoneRetentionLedger l;
	ZoneRetentionLedgerInit(&l);

	ZoneIdentity id = MakeId(1, 4, 4, 0);
	ZoneRetentionLedgerReset(&l, 4, 4, id, 0.0);

	// No deadlines set: nothing to hold for once the anchor is known.
	Check(!ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 10.0, true),
	      "no live deadline, anchors readable: release-eligible");

	ZoneRetentionLedgerSetMinResidence(&l, 4, 4, 20.0);
	Check(ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 10.0, true),
	      "before the min-residence deadline: held");
	Check(!ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 20.0001, true),
	      "just past the min-residence deadline: release-eligible");

	// Unknown anchor overrides an otherwise-expired deadline.
	Check(ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 999.0, false),
	      "unreadable anchors deny eviction regardless of deadlines");

	// A reader pin overrides everything.
	ZoneRetentionLedgerAddReader(&l, 4, 4);
	Check(ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 999.0, true),
	      "a pinned reader overrides an expired deadline and readable anchors");
	ZoneRetentionLedgerReleaseReader(&l, 4, 4);
	Check(!ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 4, 4), 999.0, true),
	      "release-eligible once the reader releases");

	// keepDeadline is the max of the live (future) deadlines.
	ZoneRetentionLedgerSetMinResidence(&l, 4, 4, 5.0);
	ZoneRetentionLedgerSetGrace(&l, 4, 4, 15.0);
	ZoneRetentionLedgerSetPredictionLease(&l, 4, 4, 10.0);
	Check(Near(ZoneRetentionKeepDeadline(ZoneRetentionLedgerGetConst(&l, 4, 4), 0.0), 15.0), "keepDeadline is the max of the three");
	// A cancelled prediction lease (deadline <= now) does not count.
	ZoneRetentionLedgerSetPredictionLease(&l, 4, 4, 999.0);
	ZoneRetentionLedgerSetGrace(&l, 4, 4, 0.0);
	Check(Near(ZoneRetentionKeepDeadline(ZoneRetentionLedgerGetConst(&l, 4, 4), 0.0), 999.0), "keepDeadline still finds the surviving prediction lease");
	Check(Near(ZoneRetentionKeepDeadline(ZoneRetentionLedgerGetConst(&l, 4, 4), 1000.0), 0.0), "keepDeadline is 0 once every deadline has passed");

	// Revisit backoff: rapid, then not, boundary at exactly 60s.
	ZoneRetentionLedgerNoteEvicted(&l, 4, 4, 100.0);
	Check(ZoneRetentionIsRapidRevisit(100.0, 160.0), "exactly 60s later is still a rapid revisit (inclusive)");
	Check(!ZoneRetentionIsRapidRevisit(100.0, 160.0001), "just past 60s is not a rapid revisit");
	Check(!ZoneRetentionIsRapidRevisit(-1.0, 100.0), "never evicted is never a rapid revisit");

	double grace1 = ZoneRetentionLedgerNoteRevisit(&l, 4, 4, 150.0);   // within 60s of 100.0
	Check(Near(grace1, 60.0), "first rapid revisit raises grace to 60s");
	// A per-cell Reset (as at every ordinary re-admission) must NOT collapse
	// the ladder back to its default -- section 6 requires escalation across
	// repeated recurrence within a world, and a revisit is itself exactly
	// the re-admission Reset is documented to run at.
	ZoneRetentionLedgerReset(&l, 4, 4, id, 150.0);
	Check(ZoneRetentionLedgerGetConst(&l, 4, 4)->revisitBackoffLevel == 1, "Reset does not clear the backoff level (bug regression)");
	ZoneRetentionLedgerNoteEvicted(&l, 4, 4, 200.0);
	double grace2 = ZoneRetentionLedgerNoteRevisit(&l, 4, 4, 210.0);
	Check(Near(grace2, 120.0), "a second rapid revisit (across an intervening Reset) still raises grace to 120s");
	ZoneRetentionLedgerReset(&l, 4, 4, id, 210.0);
	ZoneRetentionLedgerNoteEvicted(&l, 4, 4, 300.0);
	double grace3 = ZoneRetentionLedgerNoteRevisit(&l, 4, 4, 310.0);
	Check(Near(grace3, 120.0), "grace is capped at 120s");

	// Only a world-level ClearAll clears the ladder, not a per-cell Reset.
	Check(ZoneRetentionLedgerGetConst(&l, 4, 4)->revisitBackoffLevel != 0, "the ladder is still nonzero before a world reset");
	ZoneRetentionLedgerClearAll(&l);
	Check(Near(ZoneRetentionGraceSecondsForLevel(ZoneRetentionLedgerGetConst(&l, 4, 4)->revisitBackoffLevel), 30.0),
	      "ClearAll (world reset) clears the backoff level back to the 30s default");
	Check(ZoneRetentionLedgerGetConst(&l, 4, 4)->lastEvictedAt < 0.0, "ClearAll also clears lastEvictedAt");

	// Retire stops the tracking without touching the session-lifetime fields.
	ZoneRetentionLedgerReset(&l, 7, 7, MakeId(1, 7, 7, 0), 0.0);
	ZoneRetentionLedgerSetGrace(&l, 7, 7, 500.0);
	ZoneRetentionLedgerNoteEvicted(&l, 7, 7, 10.0);
	ZoneRetentionLedgerAddReader(&l, 7, 7);
	Check(ZoneRetentionLedgerCountInUse(&l) == 1, "one tracked cell before Retire");
	ZoneRetentionLedgerRetire(&l, 7, 7);
	Check(ZoneRetentionLedgerCountInUse(&l) == 0, "Retire drops the cell from the in-use count");
	Check(!ZoneRetentionPolicyWantsHold(ZoneRetentionLedgerGetConst(&l, 7, 7), 0.0, true),
	      "a retired cell is never held, live grace or not");
	Check(Near(ZoneRetentionLedgerGetConst(&l, 7, 7)->lastEvictedAt, 10.0), "Retire keeps the eviction clock");
	Check(ZoneRetentionLedgerGetConst(&l, 7, 7)->readerPinCount == 1, "Retire keeps an outstanding reader pin");
	ZoneRetentionLedgerReleaseReader(&l, 7, 7);

	// The native expiry test is pure arithmetic, independent of the ledger.
	Check(ZoneRetentionNativeWouldExpireThisFrame(0.0, -1.0, 0.2, 0.5), "all three at or under the frame delta: would expire");
	Check(!ZoneRetentionNativeWouldExpireThisFrame(5.0, -1.0, 0.0, 0.5), "one countdown still well above the frame delta: would not expire");
	Check(ZoneRetentionNativeWouldExpireThisFrame(0.5, 0.5, 0.5, 0.5), "exactly at the frame delta counts as expiring");

	// Collection respects the cap and matches the per-cell predicate.
	ZoneRetentionLedgerReset(&l, 1, 1, MakeId(1, 1, 1, 0), 0.0);
	ZoneRetentionLedgerReset(&l, 2, 2, MakeId(1, 2, 2, 0), 0.0);
	ZoneIdentity out[8];
	int n = ZoneRetentionLedgerCollectEvictionCandidates(&l, 0.0, true, out, 8);
	Check(n >= 2, "both freshly-reset cells (no deadlines) are eviction candidates");
	n = ZoneRetentionLedgerCollectEvictionCandidates(&l, 0.0, true, out, 1);
	Check(n == 1, "the cap is respected even when more cells qualify");
}

static void TestAdoptionLatency()
{
	ZoneAdoptionLatency a;
	ZoneAdoptionLatencyInit(&a);

	Check(Near(ZoneAdoptionLatencyP99ReadyToActive(&a), 0.0), "p99 of an empty accumulator is 0");

	Check(ZoneAdoptionLatencySampleCount(&a) == 0, "an empty accumulator reports n=0, distinct from a real zero");

	ZoneAdoptionLatencyRecord(&a, 0.0, 4.0, 10.0);
	Check(Near(ZonePercentileValue(&a.readyToAdmitted, 50), 4.0), "single sample: readyToAdmitted leg");
	Check(Near(ZonePercentileValue(&a.admittedToActive, 50), 6.0), "single sample: admittedToActive leg");
	Check(Near(ZonePercentileValue(&a.readyToActive, 0), 10.0), "single sample: pct 0 is the one value");
	Check(Near(ZonePercentileValue(&a.readyToActive, 100), 10.0), "single sample: pct 100 is the one value");
	Check(Near(ZoneAdoptionLatencyP99ReadyToAdmitted(&a), 4.0), "accessor matches the readyToAdmitted leg");
	Check(Near(ZoneAdoptionLatencyP99AdmittedToActive(&a), 6.0), "accessor matches the admittedToActive leg");
	Check(ZoneAdoptionLatencySampleCount(&a) == 1, "one recorded journey: n=1");

	ZoneAdoptionLatency b;
	ZoneAdoptionLatencyInit(&b);
	for (int i = 0; i < ZC_PCT_CAP; ++i)
		ZoneAdoptionLatencyRecord(&b, 0.0, 0.0, (double)i);
	Check(b.readyToActive.n == ZC_PCT_CAP, "exactly at the cap: nothing dropped yet");
	Check(b.readyToActive.dropped == 0, "no drops at the cap");
	Check(Near(b.readyToActive.max, (double)(ZC_PCT_CAP - 1)), "max is the largest sample at the cap");

	ZoneAdoptionLatencyRecord(&b, 0.0, 0.0, 9999.0);
	Check(b.readyToActive.n == ZC_PCT_CAP, "one past the cap: sample count does not grow");
	Check(b.readyToActive.dropped == 1, "one past the cap: counted as dropped");
	Check(Near(b.readyToActive.max, 9999.0), "max still reflects a dropped sample");
}

int main()
{
	TestIdentity();
	TestStateMachine();
	TestPrepLedger();
	TestPrivateTeardownReusesTheSlot();
	TestSharedPrepLedgerInstance();
	TestRegistrationFromEitherStage();
	TestNavLedger();
	TestRetentionLedger();
	TestAdoptionLatency();

	return CheckExit("zone_ledger_units");
}
