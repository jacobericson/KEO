#include <cstdio>
#include "zone/reset/zone_reset_fence.h"

#include "check.h"

int main()
{
	// --- one deadline, never two stacked bounds ---------------------------
	// The drain and the lock share the reset's budget: the sum of what the
	// drain spent and what the lock may spend never exceeds the total, except
	// by the floor that keeps a lock attempt worth making.
	Check(ZoneResetLockBudgetMs(10000, 0, 500) == 10000,
	      "a drain that waited for nothing leaves the whole budget");
	Check(ZoneResetLockBudgetMs(10000, 2500, 500) == 7500,
	      "what the drain spent comes off the lock's budget");
	Check(ZoneResetLockBudgetMs(10000, 9800, 500) == 500,
	      "a nearly exhausted budget floors rather than giving up the lock");
	Check(ZoneResetLockBudgetMs(10000, 10000, 500) == 500,
	      "an exhausted budget floors too");
	Check(ZoneResetLockBudgetMs(10000, 60000, 500) == 500,
	      "a drain that somehow overran does not underflow into a huge budget");
	Check(ZoneResetLockBudgetMs(10000, 3000, 0) == 7000,
	      "with no floor the pair is bounded by the total exactly");
	{
		// The property the structure exists for, over the whole range.
		bool bounded = true;
		// The drain's own bound is the total, so this is its whole range.
		for (unsigned waited = 0; waited <= 10000; waited += 250)
		{
			unsigned budget = ZoneResetLockBudgetMs(10000, waited, 500);
			if (waited + budget > 10000 + 500)
				bounded = false;
		}
		Check(bounded, "drain + lock never exceeds the total by more than the floor");
	}

	// --- what happens to a zone a generation may be reading ---------------
	Check(ZoneResetDecideSurvivor(true, true) == ZONE_RESET_UNLOAD,
	      "with the fence complete a claim cannot still be running");
	Check(ZoneResetDecideSurvivor(true, false) == ZONE_RESET_UNLOAD,
	      "fence complete, no claim: unload");
	Check(ZoneResetDecideSurvivor(false, true) == ZONE_RESET_SKIP_CLAIMED,
	      "fence incomplete and claimed: left loaded rather than freed under a generation");
	Check(ZoneResetDecideSurvivor(false, false) == ZONE_RESET_UNLOAD,
	      "fence incomplete but unclaimed: nothing is reading it");

	// --- retirements the fence could not verify ---------------------------
	ZoneResetFenceReset();
	{
		unsigned outgoing = ZoneResetFenceGeneration();
		Check(ZoneResetFenceCount() == 0, "the table starts empty");
		Check(!ZoneResetFenceHolds(10, 20, outgoing), "an unrecorded cell is not held over");

		Check(ZoneResetFenceNoteUnverified(10, 20, outgoing), "a skipped cell is recorded");
		Check(ZoneResetFenceCount() == 1, "one record");
		// Still inside the reset that made it: it is this generation's, not a
		// hold-over from a previous world.
		Check(!ZoneResetFenceHolds(10, 20, outgoing),
		      "a record is not held over within the reset that made it");

		ZoneResetFenceAdvanceGeneration();
		unsigned now = ZoneResetFenceGeneration();
		Check(now == outgoing + 1, "the reset ends in a new generation");
		Check(ZoneResetFenceHolds(10, 20, now),
		      "from the next generation on the cell is held over");
		Check(!ZoneResetFenceHolds(20, 10, now), "the coordinate pair is not symmetric");
		Check(!ZoneResetFenceHolds(10, 21, now), "a neighbour is not held over");

		// Re-recording the same cell at a later reset refreshes it in place.
		Check(ZoneResetFenceNoteUnverified(10, 20, now), "the same cell records again");
		Check(ZoneResetFenceCount() == 1, "without a second record");
		Check(!ZoneResetFenceHolds(10, 20, now), "and it is this generation's again");

		Check(ZoneResetFenceClear(10, 20), "a verified retirement clears its record");
		Check(ZoneResetFenceCount() == 0, "the table is empty again");
		Check(!ZoneResetFenceClear(10, 20), "clearing twice reports nothing was there");
	}

	// Coordinates outside the grid are refused rather than clamped onto a
	// real cell, which would hold a cell nothing ever retired.
	ZoneResetFenceReset();
	Check(!ZoneResetFenceNoteUnverified(-1, 0, ZoneResetFenceGeneration()), "negative x refused");
	Check(!ZoneResetFenceNoteUnverified(0, 64, ZoneResetFenceGeneration()), "y past the grid refused");
	Check(ZoneResetFenceCount() == 0, "neither reached the table");

	// The table is bounded, and what it could not take is counted rather than
	// dropped in silence.
	ZoneResetFenceReset();
	{
		unsigned g = ZoneResetFenceGeneration();
		for (int i = 0; i < ZONE_RESET_FENCE_MAX; ++i)
			Check(ZoneResetFenceNoteUnverified(i, 0, g), "the table takes its capacity");
		Check(ZoneResetFenceCount() == ZONE_RESET_FENCE_MAX, "and is then full");
		Check(ZoneResetFenceOverflow() == 0, "with nothing overflowed yet");
		Check(!ZoneResetFenceNoteUnverified(63, 63, g), "one more is refused");
		Check(ZoneResetFenceOverflow() == 1, "and counted");
		// A cell already in a full table still refreshes.
		Check(ZoneResetFenceNoteUnverified(0, 0, g + 1), "a record already there refreshes when full");
		Check(ZoneResetFenceOverflow() == 1, "a refresh is not an overflow");
		// Clearing one makes room again.
		Check(ZoneResetFenceClear(5, 0), "a record clears");
		Check(ZoneResetFenceNoteUnverified(63, 63, g), "the freed slot takes the next cell");
		Check(ZoneResetFenceCount() == ZONE_RESET_FENCE_MAX, "the table is full once more");
		// The swap-with-last removal must not lose the moved record.
		Check(ZoneResetFenceHolds(ZONE_RESET_FENCE_MAX - 1, 0, g + 2),
		      "the record moved by the removal is still findable");
	}

	// --- the injection the reset hook performs ----------------------------
	// A reset with one claimed zone outstanding and an incomplete fence: the
	// zone is skipped and recorded under the outgoing generation, and the
	// next reset classifies it as held over rather than as an unknown.
	ZoneResetFenceReset();
	{
		unsigned outgoing = ZoneResetFenceGeneration();
		Check(ZoneResetDecideSurvivor(false, true) == ZONE_RESET_SKIP_CLAIMED, "skipped");
		ZoneResetFenceNoteUnverified(31, 41, outgoing);
		ZoneResetFenceAdvanceGeneration();

		unsigned next = ZoneResetFenceGeneration();
		Check(ZoneResetFenceHolds(31, 41, next),
		      "the next reset sees the cell as a retirement it never verified");
		// That reset's fence is complete, so the cell unloads and the record
		// is closed.
		Check(ZoneResetDecideSurvivor(true, true) == ZONE_RESET_UNLOAD, "unloaded this time");
		Check(ZoneResetFenceClear(31, 41), "and the record closes");
		ZoneResetFenceAdvanceGeneration();
		Check(!ZoneResetFenceHolds(31, 41, ZoneResetFenceGeneration()),
		      "a closed record is not held over again");
	}

	// --- whether navmesh work may start while a reset runs ------------------
	Check(ZoneResetAdmit(false, false, ZONE_RESET_SITE_CLAIM) == ZONE_RESET_ADMIT,
	      "admit: with no reset and no stop a claim is taken");
	Check(ZoneResetAdmit(true, false, ZONE_RESET_SITE_CLAIM) == ZONE_RESET_DEFER_RESET,
	      "admit: a claim during a reset leaves the job queued");
	Check(ZoneResetAdmit(true, false, ZONE_RESET_SITE_HIT) == ZONE_RESET_DEFER_RESET,
	      "admit: a claimed job's lookup and HIT rebuild wait for the reset's end");
	Check(ZoneResetAdmit(true, false, ZONE_RESET_SITE_BUILD) == ZONE_RESET_DEFER_RESET,
	      "admit: a claimed job's collision build waits for the reset's end");
	Check(ZoneResetAdmit(false, true, ZONE_RESET_SITE_BUILD) == ZONE_RESET_DEFER_STOP,
	      "admit: after NavMesh::stop the stop rule decides, not this one");
	Check(ZoneResetAdmit(true, true, ZONE_RESET_SITE_HIT) == ZONE_RESET_DEFER_STOP,
	      "admit: the stop is checked first, so no thread waits out a reset after the stop");
	{
		bool uniform = true;
		for (int s = 0; s < ZONE_RESET_SITE_COUNT; ++s)
		{
			ZoneResetSite site = (ZoneResetSite)s;
			if (ZoneResetAdmit(false, false, site) != ZONE_RESET_ADMIT
			    || ZoneResetAdmit(true, false, site) != ZONE_RESET_DEFER_RESET
			    || ZoneResetAdmit(true, true, site) != ZONE_RESET_DEFER_STOP)
				uniform = false;
		}
		Check(uniform, "admit: every site answers alike: none is exempt from the reset or the stop");
	}
	Check(ZoneResetAdmit(true, false, (ZoneResetSite)ZONE_RESET_SITE_COUNT) == ZONE_RESET_DEFER_RESET,
	      "admit: a site this build does not know is covered, not admitted");
	{
		int a = 0, b = 0;
		Check(ZoneResetContentKept(&a, &a),
		      "kept: a zone that holds the same content after the wait goes on");
		Check(!ZoneResetContentKept(&a, 0),
		      "kept: a zone unloaded during the wait is dropped");
		Check(!ZoneResetContentKept(&a, &b),
		      "kept: a zone whose content was replaced during the wait is dropped");
		Check(!ZoneResetContentKept(0, 0),
		      "kept: a zone that held no content before the wait is dropped");
	}

	return CheckExit("zone_reset_fence_units");
}
