#include "zone/retention/zone_retention_policy.h"

float ZoneRetentionHoldValue(float frameDelta)
{
	float d = frameDelta > 0.0f ? frameDelta : 0.0f;
	return d + ZONE_RETENTION_HOLD_MARGIN;
}

ZoneRetentionPrecheck ZoneRetentionPrecheckCell(const ZoneRetentionCellInputs& in)
{
	// A cell the ledger does not track is not the policy's to hold: the hook
	// leaves it to the expiry guard.
	if (!in.tracked)
		return ZONE_RETENTION_PRE_PASS;

	// The three holds no pressure may override: a state we cannot judge, a
	// reader that would be left reading a freed cell, and the window that
	// stops a cell being handed over and taken away in the same breath.
	if (in.anchorsUnknown || in.readerPinned || in.minResidenceLive)
		return ZONE_RETENTION_PRE_HOLD;

	// Discretionary holds, and the proximity map with them: the map is stamped
	// at the configured retain radius, so under pressure it cannot stand in
	// for the hard core and the live check below answers instead.
	if (!in.underPressure && (in.mapRetained || in.discretionaryLive))
		return ZONE_RETENTION_PRE_HOLD;

	// Pacing before the live anchor read, so the expensive read happens at
	// most on the one frame that could act on it.
	if (!in.pacingAllows)
		return ZONE_RETENTION_PRE_HOLD;

	return ZONE_RETENTION_PRE_ASK_ANCHORS;
}

ZoneRetentionVerdict ZoneRetentionFinalVerdict(bool nearAnchorsNow)
{
	return nearAnchorsNow ? ZONE_RETENTION_HOLD : ZONE_RETENTION_RELEASE;
}

int ZoneRetentionLiveRadius(bool underPressure)
{
	return underPressure ? ZONE_RETENTION_HARD_RADIUS : ZONE_RETENTION_HYSTERESIS_RADIUS;
}

bool ZoneRetentionPacingAllows(bool probedThisFrame, bool adoptedThisFrame,
                               double lastReleaseAt, double now, bool underPressure)
{
	if (probedThisFrame || adoptedThisFrame)
		return false;
	if (underPressure)
		return true;
	if (lastReleaseAt < 0.0)
		return true;
	return now - lastReleaseAt >= ZONE_RETENTION_RELEASE_SPACING_SEC;
}

bool ZoneRetentionPressureNext(bool active, int heldCount, int softCap, int lowWater)
{
	if (!active)
		return heldCount > softCap;
	return heldCount >= lowWater;
}

double ZoneRetentionDeferBackoffSeconds(int streak)
{
	if (streak <= 0)
		return 0.0;
	double w = ZONE_RETENTION_DEFER_BACKOFF_SEC;
	for (int i = 1; i < streak && w < ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC; ++i)
		w *= 2.0;
	return w < ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC ? w : ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC;
}
