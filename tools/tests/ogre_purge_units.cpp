#include <cstdio>
#include "render/ogre_purge_policy.h"

#include "check.h"

int main()
{
	// Not eligible (a real transition's own purge): always runs, whatever the
	// clock says, and never counted as a skip or a fallback.
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(false, 1000.0, 0.0, 120.0);
		Check(d.action == OGREPURGE_RUN, "ineligible: run");
		Check(!d.countSkip && !d.countFallback, "ineligible: no counters");
	}
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(false, 0.0, 0.0, 120.0);
		Check(d.action == OGREPURGE_RUN, "ineligible at zero elapsed: still run");
	}

	// Eligible, well under the bound: skip, counted once.
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 10.0, 0.0, 120.0);
		Check(d.action == OGREPURGE_SKIP, "eligible, 10s elapsed: skip");
		Check(d.countSkip && !d.countFallback, "eligible, 10s elapsed: counts as skip only");
	}

	// The 120 s boundary: "more than" the bound is required to force a run,
	// so elapsed exactly equal to it still skips.
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 120.0, 0.0, 120.0);
		Check(d.action == OGREPURGE_SKIP, "elapsed exactly 120s: still skip, not yet past the bound");
		Check(d.countSkip && !d.countFallback, "elapsed exactly 120s: counts as skip, not fallback");
	}
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 120.000001, 0.0, 120.0);
		Check(d.action == OGREPURGE_RUN, "elapsed just past 120s: fallback run");
		Check(!d.countSkip && d.countFallback, "elapsed just past 120s: counts as fallback, not skip");
	}
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 119.999999, 0.0, 120.0);
		Check(d.action == OGREPURGE_SKIP, "elapsed just under 120s: still skip");
	}

	// A fallback run updates the caller's lastRunAt (the caller's job, not
	// this function's); the next call one tick later, against the new
	// lastRunAt, is back to a fresh skip streak rather than another fallback.
	{
		double lastRunAt = 0.0;
		OgrePurgeDecision first = OgrePurgeSkipDecide(true, 121.0, lastRunAt, 120.0);
		Check(first.action == OGREPURGE_RUN && first.countFallback, "first call past the bound: fallback");
		lastRunAt = 121.0;
		OgrePurgeDecision second = OgrePurgeSkipDecide(true, 121.001, lastRunAt, 120.0);
		Check(second.action == OGREPURGE_SKIP, "immediately after the fallback ran: back to skipping");
	}

	// maxSkipSeconds <= 0 disables the fallback entirely: an eligible call is
	// always skipped, however long it has been.
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 1.0e9, 0.0, 0.0);
		Check(d.action == OGREPURGE_SKIP, "maxSkipSeconds=0: never falls back");
	}
	{
		OgrePurgeDecision d = OgrePurgeSkipDecide(true, 1.0e9, 0.0, -5.0);
		Check(d.action == OGREPURGE_SKIP, "maxSkipSeconds<0: never falls back");
	}

	return CheckExit("ogre_purge_units");
}
