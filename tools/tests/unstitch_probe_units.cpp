#include <cstdio>
#include "fixes/stitch/unstitch_probe_policy.h"

#include "check.h"

int main()
{
	// --- the per-index verdict -------------------------------------------

	Check(UnstitchClassifyNodeIndex(0, 1) == UNSTITCH_NODE_IN_BOUNDS,
	      "index 0 into a one-entry map is in bounds");
	Check(UnstitchClassifyNodeIndex(39, 40) == UNSTITCH_NODE_IN_BOUNDS,
	      "the last valid index is in bounds");
	Check(UnstitchClassifyNodeIndex(40, 40) == UNSTITCH_NODE_PAST_END,
	      "one past the end is out of bounds");
	Check(UnstitchClassifyNodeIndex(-1, 40) == UNSTITCH_NODE_NEGATIVE,
	      "-1 is a wild read here, not a sentinel: the native -1 test is on the value read back");
	Check(UnstitchClassifyNodeIndex(-7, 40) == UNSTITCH_NODE_NEGATIVE,
	      "any negative index is a read before the map");

	// An empty map short-circuits in the native walk before the indexed read,
	// so no index there can fault and none is reported.
	Check(UnstitchClassifyNodeIndex(58, 0) == UNSTITCH_NODE_IN_BOUNDS,
	      "an empty map never performs the read");
	Check(UnstitchClassifyNodeIndex(-1, 0) == UNSTITCH_NODE_IN_BOUNDS,
	      "an empty map short-circuits a negative index too");

	Check(UnstitchVerdictIsRow(UNSTITCH_NODE_PAST_END), "past-end is reported");
	Check(UnstitchVerdictIsRow(UNSTITCH_NODE_NEGATIVE), "negative is reported");
	Check(!UnstitchVerdictIsRow(UNSTITCH_NODE_IN_BOUNDS), "in-bounds is not reported");

	// --- length plausibility ---------------------------------------------

	Check(UnstitchSetCountPlausible(0) && UnstitchSetCountPlausible(UNSTITCH_MAX_SETS),
	      "zero and the cap are plausible set counts");
	Check(!UnstitchSetCountPlausible(-1) && !UnstitchSetCountPlausible(UNSTITCH_MAX_SETS + 1),
	      "a negative or oversized set count is refused");
	Check(UnstitchConnCountPlausible(UNSTITCH_MAX_CONNS)
	      && !UnstitchConnCountPlausible(UNSTITCH_MAX_CONNS + 1),
	      "the connection cap is inclusive and one past it is refused");

	// --- the set filter ---------------------------------------------------

	Check(UnstitchClassifySet(7, 9, 4) == UNSTITCH_SET_SKIP_NOT_OURS,
	      "a set naming another section is skipped, as the native walk skips it");
	Check(UnstitchClassifySet(9, 9, 4) == UNSTITCH_SET_WALK,
	      "a set naming this section is walked");
	Check(UnstitchClassifySet(9, 9, -3) == UNSTITCH_SET_SKIP_IMPLAUSIBLE,
	      "a set naming this section with an untrustworthy length is refused, not walked");

	// --- injection: the r3 record, driven through the walk ----------------
	// The fault read the opposite instance's node map at recorded index 58.
	// The same walk over a synthesised set reports it when the map is shorter
	// than 59 entries and stays silent when it is not -- which is the fork the
	// probe exists to decide.
	{
		const int indices[12] = { 182, 183, 190, 191, 195, 196, 197, 198, 199, 12, 33, 58 };
		UnstitchSetStub set;
		set.thisUid = 4242;
		set.oppositeUid = 7777;
		set.oppositeResolved = true;
		set.oppositeNodeMapSize = 40;      // shorter than the recorded index
		set.oppositeNodeIndices = indices;
		set.connCount = 12;

		UnstitchWalkTally t = UnstitchWalkStubs(4242, &set, 1);
		Check(t.setsSeen == 1 && t.setsMatched == 1 && t.oppositeResolved == 1,
		      "the synthesised set is seen, matched and resolved");
		Check(t.connsExamined == 12, "every connection in the set is examined");
		// 182..199 and 58 are all >= 40 here; 12 and 33 are not.
		Check(t.rows == 10, "every index past the end of the map is reported");
		Check(t.negativeRows == 0, "none of these are negative");

		// Same records, a map long enough to hold index 58: no row at all.
		set.oppositeNodeMapSize = 200;
		UnstitchWalkTally u = UnstitchWalkStubs(4242, &set, 1);
		Check(u.connsExamined == 12, "the same connections are still examined");
		Check(u.rows == 0, "an in-bounds index 58 produces no row");
	}

	// A single record at exactly the fault's numbers, stated on its own.
	{
		const int one[1] = { 58 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeMapSize = 40; set.oppositeNodeIndices = one; set.connCount = 1;
		Check(UnstitchWalkStubs(1, &set, 1).rows == 1, "index 58 against a 40-entry map is a row");
		set.oppositeNodeMapSize = 55;
		Check(UnstitchWalkStubs(1, &set, 1).rows == 1,
		      "index 58 against the 55-entry map read out of the dump is a row");
		set.oppositeNodeMapSize = 58;
		Check(UnstitchWalkStubs(1, &set, 1).rows == 1, "index 58 against a 58-entry map is still a row");
		set.oppositeNodeMapSize = 59;
		Check(UnstitchWalkStubs(1, &set, 1).rows == 0, "index 58 against a 59-entry map is not");
	}

	// Negative indices are reported and counted apart, so "out of bounds" and
	// "recorded as -1" never collapse into one number.
	{
		const int neg[3] = { -1, 5, -4 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeMapSize = 10; set.oppositeNodeIndices = neg; set.connCount = 3;
		UnstitchWalkTally t = UnstitchWalkStubs(1, &set, 1);
		Check(t.rows == 2 && t.negativeRows == 2, "both negative indices are rows, counted as negative");
	}

	// An empty opposite map: the native walk never reads, so neither does the
	// tally, however wild the recorded index is.
	{
		const int wild[2] = { 0x7FFFFFFF, 58 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeMapSize = 0; set.oppositeNodeIndices = wild; set.connCount = 2;
		UnstitchWalkTally t = UnstitchWalkStubs(1, &set, 1);
		Check(t.connsExamined == 2 && t.rows == 0, "an empty opposite map produces no rows");
	}

	// An unresolved opposite section is counted as seen and matched but never
	// walked: the native lookup returns -1 there and skips the set.
	{
		const int idx[1] = { 999 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = false;
		set.oppositeNodeMapSize = 4; set.oppositeNodeIndices = idx; set.connCount = 1;
		UnstitchWalkTally t = UnstitchWalkStubs(1, &set, 1);
		Check(t.setsMatched == 1 && t.oppositeResolved == 0 && t.connsExamined == 0 && t.rows == 0,
		      "an unresolved opposite section is matched but not walked");
	}

	// Sets belonging to other sections are walked past, not into.
	{
		const int idx[1] = { 999 };
		UnstitchSetStub sets[2];
		sets[0].thisUid = 5; sets[0].oppositeUid = 6; sets[0].oppositeResolved = true;
		sets[0].oppositeNodeMapSize = 4; sets[0].oppositeNodeIndices = idx; sets[0].connCount = 1;
		sets[1].thisUid = 1; sets[1].oppositeUid = 6; sets[1].oppositeResolved = true;
		sets[1].oppositeNodeMapSize = 4; sets[1].oppositeNodeIndices = idx; sets[1].connCount = 1;
		UnstitchWalkTally t = UnstitchWalkStubs(1, sets, 2);
		Check(t.setsSeen == 2 && t.setsMatched == 1 && t.connsExamined == 1 && t.rows == 1,
		      "only the set naming this section contributes a row");
	}

	// Refused lengths are counted, never dropped in silence.
	{
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeMapSize = 4; set.oppositeNodeIndices = 0;
		set.connCount = UNSTITCH_MAX_CONNS + 1;
		UnstitchWalkTally t = UnstitchWalkStubs(1, &set, 1);
		Check(t.setsMatched == 1 && t.implausible == 1 && t.connsExamined == 0,
		      "an untrustworthy connection count is counted as implausible, not walked");
	}
	{
		UnstitchWalkTally t = UnstitchWalkStubs(1, 0, UNSTITCH_MAX_SETS + 1);
		Check(t.implausible == 1 && t.setsSeen == 0,
		      "an untrustworthy set count refuses the whole array and says so");
	}
	{
		UnstitchWalkTally t = UnstitchWalkStubs(1, 0, 3);
		Check(t.implausible == 1 && t.setsSeen == 0,
		      "a null array with a non-zero count is refused, not walked");
	}
	{
		UnstitchWalkTally t = UnstitchWalkStubs(1, 0, 0);
		Check(t.implausible == 0 && t.setsSeen == 0 && t.rows == 0,
		      "an empty set array is not implausible, it is just empty");
	}

	return CheckExit("unstitch_probe_units");
}
