// Host tests for the retention policy arithmetic (src/zone/zone_retention_policy.{h,cpp}).
#include "zone/retention/zone_retention_policy.h"

#include <cstdio>

#include "check.h"


static ZoneRetentionCellInputs Base()
{
	ZoneRetentionCellInputs in;
	in.tracked           = true;
	in.anchorsUnknown    = false;
	in.readerPinned      = false;
	in.minResidenceLive  = false;
	in.discretionaryLive = false;
	in.mapRetained       = false;
	in.underPressure     = false;
	in.pacingAllows      = true;
	return in;
}

static void TestHoldValue()
{
	// The value must survive the native decrement in single precision: the
	// remainder is what keeps the cell, so a remainder that rounds to zero is
	// an unload.
	const float deltas[5] = { 0.0f, 0.004f, 0.016f, 1.5f, 60.0f };
	for (int i = 0; i < 5; ++i)
	{
		float held = ZoneRetentionHoldValue(deltas[i]);
		Check(held > deltas[i], "the hold value is above the frame delta it will lose");
		Check(held - deltas[i] > 0.0f, "the remainder after the native decrement is positive");
		Check(held - deltas[i] <= ZONE_RETENTION_HOLD_MARGIN + 0.001f,
		      "the remainder is the margin, so the hold does not outlive its frame");
	}
	Check(ZoneRetentionHoldValue(-1.0f) > 0.0f, "a nonsense negative delta still writes a positive value");
}

static void TestPrecheckUntracked()
{
	ZoneRetentionCellInputs in = Base();
	in.tracked = false;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_PASS,
	      "an untracked cell expires the way it does in vanilla");
}

static void TestPrecheckUnconditionalHolds()
{
	// Each of the three survives pressure, and each holds on its own.
	const int kCases = 3;
	for (int i = 0; i < kCases; ++i)
	{
		for (int pressure = 0; pressure < 2; ++pressure)
		{
			ZoneRetentionCellInputs in = Base();
			in.underPressure = pressure != 0;
			if (i == 0) in.anchorsUnknown   = true;
			if (i == 1) in.readerPinned     = true;
			if (i == 2) in.minResidenceLive = true;
			Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_HOLD,
			      "unreadable anchors, a pinned reader and min residence hold under pressure too");
		}
	}
}

static void TestPrecheckDiscretionaryHolds()
{
	ZoneRetentionCellInputs in = Base();
	in.mapRetained = true;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_HOLD, "the proximity map holds a cell");
	in.underPressure = true;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_ASK_ANCHORS,
	      "pressure drops the map and asks the live anchors instead");

	in = Base();
	in.discretionaryLive = true;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_HOLD, "a live grace or prediction lease holds a cell");
	in.underPressure = true;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_ASK_ANCHORS, "pressure drops a discretionary lease");
}

static void TestPrecheckPacing()
{
	ZoneRetentionCellInputs in = Base();
	in.pacingAllows = false;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_HOLD,
	      "pacing expresses itself as a hold, and does so before the live anchor read");

	in.pacingAllows = true;
	Check(ZoneRetentionPrecheckCell(in) == ZONE_RETENTION_PRE_ASK_ANCHORS,
	      "with nothing else to hold for, the live anchors decide");
}

static void TestFinalVerdict()
{
	Check(ZoneRetentionFinalVerdict(true) == ZONE_RETENTION_HOLD, "a live anchor in radius holds the cell");
	Check(ZoneRetentionFinalVerdict(false) == ZONE_RETENTION_RELEASE, "no anchor in radius releases it");
	Check(ZoneRetentionLiveRadius(false) == ZONE_RETENTION_HYSTERESIS_RADIUS, "no pressure: the wider radius");
	Check(ZoneRetentionLiveRadius(true) == ZONE_RETENTION_HARD_RADIUS, "pressure narrows the radius to the hard core");
}

static void TestPacing()
{
	Check(!ZoneRetentionPacingAllows(true, false, -1.0, 100.0, false), "one expensive decision per frame");
	Check(!ZoneRetentionPacingAllows(true, false, -1.0, 100.0, true), "one per frame, pressure included");
	Check(!ZoneRetentionPacingAllows(false, true, -1.0, 100.0, false), "never in a frame that admitted a cohort");
	Check(!ZoneRetentionPacingAllows(false, true, -1.0, 100.0, true), "not even under pressure");

	Check(ZoneRetentionPacingAllows(false, false, -1.0, 100.0, false), "the first release of a session is allowed");
	Check(!ZoneRetentionPacingAllows(false, false, 99.0, 100.0, false), "within the spacing: refused");
	Check(ZoneRetentionPacingAllows(false, false, 98.0, 100.0, false), "exactly the spacing apart: allowed");
	Check(ZoneRetentionPacingAllows(false, false, 99.9, 100.0, true), "pressure drops the spacing");
}

static void TestPressure()
{
	Check(!ZoneRetentionPressureNext(false, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_LOW_WATER),
	      "at the cap, pressure has not started");
	Check(ZoneRetentionPressureNext(false, ZONE_RETENTION_SOFT_CAP + 1, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_LOW_WATER),
	      "above the cap, pressure starts");
	Check(ZoneRetentionPressureNext(true, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_LOW_WATER),
	      "pressure does not end at the cap it started above");
	Check(ZoneRetentionPressureNext(true, ZONE_RETENTION_LOW_WATER, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_LOW_WATER),
	      "pressure holds at the low water mark");
	Check(!ZoneRetentionPressureNext(true, ZONE_RETENTION_LOW_WATER - 1, ZONE_RETENTION_SOFT_CAP, ZONE_RETENTION_LOW_WATER),
	      "pressure ends below the low water mark");
	Check(ZONE_RETENTION_LOW_WATER < ZONE_RETENTION_SOFT_CAP, "the low water mark is below the cap, or the hysteresis is not one");
}


static void TestDeferBackoff()
{
	// No streak, no window: a cell that has never been refused is asked on
	// the first frame its countdowns run out.
	Check(ZoneRetentionDeferBackoffSeconds(0) == 0.0, "no refusal means no backoff");
	Check(ZoneRetentionDeferBackoffSeconds(-3) == 0.0, "a negative streak means no backoff");

	Check(ZoneRetentionDeferBackoffSeconds(1) == ZONE_RETENTION_DEFER_BACKOFF_SEC,
	      "the first refusal costs the base window");
	Check(ZoneRetentionDeferBackoffSeconds(2) == 2.0 * ZONE_RETENTION_DEFER_BACKOFF_SEC,
	      "the window doubles with the streak");

	// Monotone and capped, so no streak length can either shrink the window
	// or grow it without end.
	double prev = 0.0;
	for (int streak = 0; streak <= 200; ++streak)
	{
		double w = ZoneRetentionDeferBackoffSeconds(streak);
		Check(w >= prev, "the window never shrinks as the streak grows");
		Check(w <= ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC, "the window never passes the cap");
		prev = w;
	}
	Check(ZoneRetentionDeferBackoffSeconds(200) == ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC,
	      "a long streak sits at the cap");

	// The bound this exists for: a fence that refuses every time it is asked,
	// for a minute, over a frame-by-frame prologue. Without the backoff that
	// is one attempt per frame; with it, attempts are what the windows allow.
	double t = 0.0;
	int attempts = 0, streak = 0;
	double quietUntil = -1.0;
	const double frame = 1.0 / 60.0;
	while (t < 60.0)
	{
		if (quietUntil <= t)
		{
			attempts++;
			streak++;
			quietUntil = t + ZoneRetentionDeferBackoffSeconds(streak);
		}
		t += frame;
	}
	Check(attempts < 30, "a minute of refusals costs fewer than thirty attempts");
	Check(attempts > 0, "the backoff still lets the cell be asked");
}

int main()
{
	TestHoldValue();
	TestPrecheckUntracked();
	TestPrecheckUnconditionalHolds();
	TestPrecheckDiscretionaryHolds();
	TestPrecheckPacing();
	TestFinalVerdict();
	TestPacing();
	TestPressure();
	TestDeferBackoff();

	return CheckExit("zone_retention_units");
}
