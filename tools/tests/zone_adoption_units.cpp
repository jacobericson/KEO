#include <cstdio>
#include <cmath>
#include "zone/handoff/zone_adoption_seam.h"

#include "check.h"

static void TestCertificateSeam()
{
	Check(ZoneAdoptionCertificate() == ZONE_CERT_UNPROVED, "the certificate seam answers unproved");
	Check(ZONE_ADOPT_LATE_PATH == 0, "the late-adoption path is compiled out");

	// The structural half of the guarantee cannot be asserted from inside
	// the program: it is that no "proved" enumerator and no late admission
	// verdict exist to be named at all, so nothing can be written that
	// returns one. What is checkable here is that the only two verdicts the
	// admission rule can produce are refusal and the content-only handover.
	ZoneAdmissionInputs in;
	in.preparedAndReady = true;
	in.contentInitialized = true;
	in.meshReportedIn = true;
	in.flagsPrivate = true;
	in.reAdmissionAllowed = true;
	ZoneAdmissionVerdict v = ZoneAdmissionDecide(in);
	Check(v == ZONE_ADMIT_CONTENT_ONLY, "a fully ready cell is admitted content-only");
	Check(v != ZONE_ADMIT_NO, "a fully ready cell is not refused");
}

static void TestTownGuard()
{
	Check(ZoneTownGuardRefuses(ZONE_ACTIVATION_TOWN, 0.0f), "town refresh with a zero timer is refused");
	Check(ZoneTownGuardRefuses(ZONE_ACTIVATION_TOWN, -1.0f), "town refresh with a negative timer is refused");
	Check(!ZoneTownGuardRefuses(ZONE_ACTIVATION_TOWN, 1.0f), "a town's own activation (1.0) is allowed");
	Check(!ZoneTownGuardRefuses(ZONE_ACTIVATION_TOWN, 0.001f), "any positive town timer is allowed");

	// The camera's own activation passes 0.0 and means "use the default";
	// only the town type is guarded.
	Check(!ZoneTownGuardRefuses(ZONE_ACTIVATION_CAMERA, 0.0f), "camera activation with 0.0 is untouched");
	Check(!ZoneTownGuardRefuses(ZONE_ACTIVATION_PLAYER, 0.0f), "player activation with 0.0 is untouched");
	Check(!ZoneTownGuardRefuses(ZONE_ACTIVATION_CAMERA, -5.0f), "a negative camera timer is untouched");

	float nan = 0.0f;
	{
		// Build a NaN without depending on <limits> in this toolset.
		volatile float zero = 0.0f;
		nan = zero / zero;
	}
	Check(nan != nan, "the test built a NaN");
	Check(ZoneTownGuardRefuses(ZONE_ACTIVATION_TOWN, nan), "a NaN town timer refuses rather than passes");
}

static ZoneAdmissionInputs ReadyInputs()
{
	ZoneAdmissionInputs in;
	in.preparedAndReady = true;
	in.contentInitialized = true;
	in.meshReportedIn = true;
	in.flagsPrivate = true;
	in.reAdmissionAllowed = true;
	return in;
}

static void TestAdmission()
{
	Check(ZoneAdmissionDecide(ReadyInputs()) == ZONE_ADMIT_CONTENT_ONLY, "all conditions met: admit");

	ZoneAdmissionInputs in = ReadyInputs();
	in.preparedAndReady = false;
	Check(ZoneAdmissionDecide(in) == ZONE_ADMIT_NO, "a cell that is not ready is not admitted");

	in = ReadyInputs();
	in.contentInitialized = false;
	Check(ZoneAdmissionDecide(in) == ZONE_ADMIT_NO, "a bare shell is not admitted");

	in = ReadyInputs();
	in.meshReportedIn = false;
	Check(ZoneAdmissionDecide(in) == ZONE_ADMIT_NO, "a cell whose mesh is out is not admitted");

	in = ReadyInputs();
	in.flagsPrivate = false;
	Check(ZoneAdmissionDecide(in) == ZONE_ADMIT_NO, "a cell the mod does not hold is not admitted");

	in = ReadyInputs();
	in.reAdmissionAllowed = false;
	Check(ZoneAdmissionDecide(in) == ZONE_ADMIT_NO, "a cell inside its re-entry interval is not admitted");
}

static ZoneCohortWindow OpenWindow()
{
	ZoneCohortWindow w;
	w.mainThread = true;
	w.loaderIdle = true;
	w.transitionQuiet = true;
	w.worldQuiet = true;
	w.centralZoneValid = true;
	w.physicsQueuesClear = true;
	return w;
}

static void TestCohortWindow()
{
	Check(ZoneCohortWindowOpen(OpenWindow()), "every condition met: the window is open");

	ZoneCohortWindow w = OpenWindow(); w.mainThread = false;
	Check(!ZoneCohortWindowOpen(w), "off the main thread: closed");
	w = OpenWindow(); w.loaderIdle = false;
	Check(!ZoneCohortWindowOpen(w), "loader busy: closed");
	w = OpenWindow(); w.transitionQuiet = false;
	Check(!ZoneCohortWindowOpen(w), "transition in flight: closed");
	w = OpenWindow(); w.worldQuiet = false;
	Check(!ZoneCohortWindowOpen(w), "a load in progress: closed");
	w = OpenWindow(); w.centralZoneValid = false;
	Check(!ZoneCohortWindowOpen(w), "no central zone: closed");
	w = OpenWindow(); w.physicsQueuesClear = false;
	Check(!ZoneCohortWindowOpen(w), "physics queues busy: closed");
}

static void TestReAdmission()
{
	Check(ZoneReAdmissionAllowed(100.0, 0.0, 20.0), "a cell never admitted may be admitted");
	Check(!ZoneReAdmissionAllowed(100.0, 90.0, 20.0), "10s after the last admission: refused");
	Check(ZoneReAdmissionAllowed(100.0, 80.0, 20.0), "exactly the interval: allowed");
	Check(ZoneReAdmissionAllowed(100.0, 79.0, 20.0), "past the interval: allowed");
	Check(ZoneReAdmissionAllowed(5.0, 900.0, 20.0), "a clock that went backwards does not wedge the cell");
}

// The engine's rule, pinned by name because its polarity reads backwards:
// the finalize runs while the flag is SET and clears it, so a clear flag
// means "already finalized". A gate written the other way round is closed
// for every mod-loaded cell — which is silent, because nothing else in the
// mod reads this byte to decide anything.
static void TestContentFinalizePolarity()
{
	Check(ZoneContentNeedsFinalize(true, true), "terrain in and the flag still set: the finalize is owed");
	Check(!ZoneContentNeedsFinalize(true, false), "flag already cleared: the finalize has run, do not repeat it");
	Check(!ZoneContentNeedsFinalize(false, true), "terrain not in yet: not owed");
	Check(!ZoneContentNeedsFinalize(false, false), "neither: not owed");

	Check(ZoneContentIsFinalized(false), "a clear flag is the finalized state");
	Check(!ZoneContentIsFinalized(true), "a set flag is not the finalized state");

	// The two predicates answer opposite questions about the same byte, and
	// a cell the mod has just loaded always carries a set flag.
	const bool freshlyLoadedByTheMod = true;
	Check(!ZoneContentIsFinalized(freshlyLoadedByTheMod),
	      "a freshly loaded cell is not finalized");
	Check(ZoneContentNeedsFinalize(true, freshlyLoadedByTheMod),
	      "a freshly loaded cell with its terrain in is exactly the case the gate must let through");
}

static ZoneTakeoverInputs PrivateDemand()
{
	ZoneTakeoverInputs in;
	in.mainThread = true;
	in.modHoldsPrivately = true;
	in.flagsPrivate = true;
	in.loaderWalkingSetA = false;
	return in;
}

static void TestTakeover()
{
	Check(ZoneTakeoverDecide(PrivateDemand()) == ZONE_TAKEOVER_ADOPT, "real demand on a private cell adopts it");

	ZoneTakeoverInputs in = PrivateDemand(); in.mainThread = false;
	Check(ZoneTakeoverDecide(in) == ZONE_TAKEOVER_NO, "off the main thread: no takeover");

	in = PrivateDemand(); in.modHoldsPrivately = false;
	Check(ZoneTakeoverDecide(in) == ZONE_TAKEOVER_NO, "a cell the mod does not hold is not taken over");

	in = PrivateDemand(); in.flagsPrivate = false;
	Check(ZoneTakeoverDecide(in) == ZONE_TAKEOVER_NO, "a cell whose flags are not private is not taken over");

	in = PrivateDemand(); in.loaderWalkingSetA = true;
	Check(ZoneTakeoverDecide(in) == ZONE_TAKEOVER_DEFER,
	      "a loader walking the pending set defers rather than inserting into it");

	in = PrivateDemand(); in.loaderWalkingSetA = true; in.modHoldsPrivately = false;
	Check(ZoneTakeoverDecide(in) == ZONE_TAKEOVER_NO,
	      "a cell we do not hold is nothing to do, walk or no walk");
}

int main()
{
	TestCertificateSeam();
	TestTownGuard();
	TestAdmission();
	TestCohortWindow();
	TestReAdmission();
	TestTakeover();
	TestContentFinalizePolarity();

	return CheckExit("zone_adoption_units");
}
