#include "zone/handoff/zone_adoption_seam.h"

ZoneCertificateVerdict ZoneAdoptionCertificate()
{
	return ZONE_CERT_UNPROVED;
}

bool ZoneTownGuardRefuses(int activationType, float deactivationTimer)
{
	if (activationType != ZONE_ACTIVATION_TOWN)
		return false;
	// Negated comparison, so a NaN timer lands on "refuse".
	return !(deactivationTimer > 0.0f);
}

bool ZoneContentNeedsFinalize(bool terrainLoaded, bool activationFlagSet)
{
	return terrainLoaded && activationFlagSet;
}

bool ZoneContentIsFinalized(bool activationFlagSet)
{
	return !activationFlagSet;
}

ZoneAdmissionVerdict ZoneAdmissionDecide(const ZoneAdmissionInputs& in)
{
	if (!in.preparedAndReady)
		return ZONE_ADMIT_NO;
	if (!in.contentInitialized)
		return ZONE_ADMIT_NO;
	if (!in.flagsPrivate)
		return ZONE_ADMIT_NO;
	if (!in.meshReportedIn)
		return ZONE_ADMIT_NO;
	if (!in.reAdmissionAllowed)
		return ZONE_ADMIT_NO;
	return ZONE_ADMIT_CONTENT_ONLY;
}

bool ZoneCohortWindowOpen(const ZoneCohortWindow& w)
{
	return w.mainThread
	    && w.loaderIdle
	    && w.transitionQuiet
	    && w.worldQuiet
	    && w.centralZoneValid
	    && w.physicsQueuesClear;
}

bool ZoneReAdmissionAllowed(double now, double lastAdmittedAt, double minIntervalSec)
{
	if (lastAdmittedAt <= 0.0)
		return true;              // never admitted
	if (now < lastAdmittedAt)
		return true;              // clock reset under us; do not wedge the cell
	return now - lastAdmittedAt >= minIntervalSec;
}

ZoneTakeoverVerdict ZoneTakeoverDecide(const ZoneTakeoverInputs& in)
{
	if (!in.mainThread)
		return ZONE_TAKEOVER_NO;
	if (!in.modHoldsPrivately || !in.flagsPrivate)
		return ZONE_TAKEOVER_NO;
	// Phases 2 and 3 are the two that walk the pending-activation set, and
	// an insert can rehash it. A demand that arrives from inside one of those
	// walks — a building physicalizing in a cell the loader is initializing
	// reaches the town's activate, which reaches this function — is recorded
	// and answered on a later frame instead. Phase 3 also refuses natively;
	// phase 2 does not, which is exactly why the mod must.
	if (in.loaderWalkingSetA)
		return ZONE_TAKEOVER_DEFER;
	return ZONE_TAKEOVER_ADOPT;
}
