#include <cstdio>
#include <string>
#include "zone/preload/coverage_stats.h"

#include "check.h"

// The shipped budget: MAX_PRELOADED and CAMERA_RESERVED as preload.h defines
// them. Mirrored rather than included so this stays free of game headers; if
// the real constants move, the boundary cases below are what to re-check.
static const int MAXP = 45;
static const int CAMR = 12;

int main()
{
	// Nothing to cover means no answer, not a full grid.
	Check(!CoverageFullGridAllowed(MAXP, CAMR, 0), "zero centers is not full grid");
	Check(!CoverageFullGridAllowed(MAXP, CAMR, -1), "negative centers is not full grid");

	// A budget that does not exist cannot fund a grid.
	Check(!CoverageFullGridAllowed(12, 12, 1), "exhausted budget refuses");
	Check(!CoverageFullGridAllowed(4, 12, 1), "negative budget refuses");

	// The live boundary: 45 - 12 = 33 cells, 9 per center, so three centers
	// fit (33/3 = 11) and four do not (33/4 = 8). This is the clamp the
	// character scan still applies, and the reason it was left in place.
	Check(CoverageFullGridAllowed(MAXP, CAMR, 1), "one center gets 3x3");
	Check(CoverageFullGridAllowed(MAXP, CAMR, 3), "three centers still fit 3x3");
	Check(!CoverageFullGridAllowed(MAXP, CAMR, 4), "four centers clamp to 2x2");
	Check(!CoverageFullGridAllowed(MAXP, CAMR, 33), "many centers clamp to 2x2");

	// Integer division, not rounding: exactly 9 each qualifies.
	Check(CoverageFullGridAllowed(21, 12, 1), "exactly 9 available qualifies");
	Check(!CoverageFullGridAllowed(20, 12, 1), "8 available does not");

	// The scan's per-character gate: the camera's area always, elsewhere only
	// while the squad radius keeps a ring and the hold is under its cap.
	Check(CoverageCharacterInScan(true, 0, true), "near the camera: scanned whatever the squad radius or cap");
	Check(CoverageCharacterInScan(false, 1, false), "away, squad radius 1, no pressure: scanned");
	Check(CoverageCharacterInScan(false, 3, false), "away, a wider squad radius: scanned");
	Check(!CoverageCharacterInScan(false, 0, false), "away, squad radius 0: skipped");
	Check(!CoverageCharacterInScan(false, 1, true), "away, past the cap: skipped");

	// Every counter prints from the start, so a session can tell "never
	// reached" from "absent". A fresh token must carry all of them at zero.
	{
		CoverageResetSession();
		std::string t = CoverageStatsToken();
		Check(t.find("cam3x3:0/0") != std::string::npos, "camera grid prints at zero");
		Check(t.find("ahead:0/0/0") != std::string::npos, "ahead zones print at zero");
		Check(t.find("charGrid:0/0/0/0") != std::string::npos, "char grid prints at zero");
		Check(t.find("tier:0/0") != std::string::npos, "tiers print at zero");
	}

	// Each note reaches its own field, and a stepless ahead call is counted
	// as a call without being counted as a step -- the difference between
	// "no direction to extrapolate" and "the queue refused".
	{
		CoverageResetSession();
		CoverageNoteCameraGrid(7);
		CoverageNoteAheadZones(true, false, 0);
		CoverageNoteAheadZones(true, true, 3);
		CoverageNoteCharScan(2, true, 11);
		CoverageNoteCharScan(5, false, 4);
		CoverageNoteTier4();
		CoverageNoteTier4();
		CoverageNoteTier5();
		std::string t = CoverageStatsToken();
		Check(t.find("cam3x3:1/7") != std::string::npos, "camera grid accumulates");
		Check(t.find("ahead:2/1/3") != std::string::npos, "stepless ahead call counts as a call only");
		Check(t.find("charGrid:2/1/1/15") != std::string::npos, "char grid splits full and clamped");
		Check(t.find("tier:2/1") != std::string::npos, "tiers accumulate");
	}

	// A scan that found no center is not a scan: it would otherwise inflate
	// the clamped count and read as a budget problem that never happened.
	{
		CoverageResetSession();
		CoverageNoteCharScan(0, false, 0);
		Check(CoverageStatsToken().find("charGrid:0/0/0/0") != std::string::npos,
		      "centerless scan is not counted");
	}

	// A non-camera ahead call belongs to the character path and must not
	// land in the camera ahead field.
	{
		CoverageResetSession();
		CoverageNoteAheadZones(false, true, 3);
		Check(CoverageStatsToken().find("ahead:0/0/0") != std::string::npos,
		      "character-owned ahead call is not camera coverage");
	}

	// The reset clears everything, so a save load cannot leave the previous
	// world's totals looking like this one's coverage.
	{
		CoverageNoteCameraGrid(9);
		CoverageNoteTier5();
		CoverageResetSession();
		std::string t = CoverageStatsToken();
		Check(t.find("cam3x3:0/0") != std::string::npos, "reset clears camera grid");
		Check(t.find("tier:0/0") != std::string::npos, "reset clears tiers");
	}

	// Dropped centers are reported apart from the scan count, which is per
	// scan and cannot express them.
	{
		CoverageResetSession();
		Check(CoverageStatsToken().find("drop:0") != std::string::npos, "drops print at zero");
		CoverageNoteCharScan(24, false, 40);
		CoverageNoteCentersDropped(3);
		CoverageNoteCentersDropped(0);
		Check(CoverageStatsToken().find("drop:3") != std::string::npos, "drops accumulate");
	}

	// The periodic report fires once immediately, then no more often than
	// every 30 s, and never depends on a counter having moved.
	{
		Check(CoverageDueForPeriodicReport(100.0), "first call reports immediately");
		Check(!CoverageDueForPeriodicReport(100.1), "a frame later is too soon");
		Check(!CoverageDueForPeriodicReport(129.9), "just under the interval is too soon");
		Check(CoverageDueForPeriodicReport(130.0), "the interval reports");
		Check(!CoverageDueForPeriodicReport(130.0), "the same instant does not report twice");
		CoverageResetSession();
		Check(!CoverageDueForPeriodicReport(130.5), "a reset does not re-arm the report");
	}

	return CheckExit("coverage_stats_units");
}
