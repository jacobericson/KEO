#include <cstdio>
#include "fixes/stitch/unstitch_guard_policy.h"

#include "check.h"

int main()
{
	// --- the guard's per-record decision ----------------------------------
	// The measured fault: a connection recording opposite node index 58
	// against an opposite node map holding 55 entries. The native walk reads
	// map[58] -- four entries past the end -- and uses what it finds as an
	// index into the opposite instance's node array.

	Check(UnstitchGuardConnAction(58, 55) == UNSTITCH_CONN_SKIP_OOB,
	      "index 58 against the measured 55-entry map is dropped");
	Check(UnstitchGuardConnAction(54, 55) == UNSTITCH_CONN_PROCESS,
	      "the last index that map does hold is handed on");
	Check(UnstitchGuardConnAction(58, 59) == UNSTITCH_CONN_PROCESS,
	      "the same index against a map long enough to hold it is handed on");
	Check(UnstitchGuardConnAction(58, 58) == UNSTITCH_CONN_SKIP_OOB,
	      "58 entries means indices 0..57, so 58 is still past the end");
	Check(UnstitchGuardConnAction(58, 0) == UNSTITCH_CONN_PROCESS,
	      "an empty map short-circuits in the native walk before any read");

	// -1 keeps the meaning the native walk gives it, and this guard does not
	// take it over. The native test is on the value read back out of the map:
	// -1 there means "no instanced node", and such a record is left alone.
	Check(UnstitchGuardMapValue(-1, 0) == UNSTITCH_MAP_NATIVE_SENTINEL,
	      "a map slot holding -1 is the native skip, empty node array or not");
	Check(UnstitchGuardMapValue(-1, 7) == UNSTITCH_MAP_NATIVE_SENTINEL,
	      "-1 is never treated as an index and never counted as a guard action");
	Check(UnstitchGuardMapValue(6, 7) == UNSTITCH_MAP_USE,
	      "the last index the node array holds is used");
	Check(UnstitchGuardMapValue(7, 7) == UNSTITCH_MAP_SKIP_OUT_OF_RANGE,
	      "one past the node array is the second wild load");
	Check(UnstitchGuardMapValue(-2, 7) == UNSTITCH_MAP_SKIP_OUT_OF_RANGE,
	      "a negative that is not the sentinel reads before the node array");

	// A recorded index below zero is not a sentinel: nothing in the native
	// walk tests the index at all, so it reads before the map exactly as an
	// oversized one reads past it.
	Check(UnstitchGuardConnAction(-1, 55) == UNSTITCH_CONN_SKIP_OOB,
	      "a recorded -1 index is a wild read, not a sentinel");

	// --- injection: the fault's own set, driven through the guard's walk ---
	// Twelve records, the last carrying the measured index, against the
	// measured map. The proof is not the count of drops but which records
	// survived: everything except the offending one, in its original order.
	{
		const int indices[12] = { 182, 183, 190, 191, 195, 196, 197, 198, 199, 12, 33, 58 };
		UnstitchSetStub set;
		set.thisUid = 4242;
		set.oppositeUid = 7777;
		set.oppositeResolved = true;
		set.oppositeNodeMapSize = 55;   // the count read out of the dump
		set.oppositeNodeIndices = indices;
		set.connCount = 12;

		int processed[16];
		UnstitchGuardTally t = UnstitchGuardWalkStubs(4242, &set, 1, processed, 16);
		Check(t.connsExamined == 12, "every connection is examined");
		// 182..199 are all past a 55-entry map too; 12, 33 are the only ones inside it.
		Check(t.connsSkipped == 10 && t.connsProcessed == 2,
		      "every index past the end of the measured map is dropped");
		Check(processed[0] == 9 && processed[1] == 10,
		      "the records that survive are the in-bounds ones, in their original order");

		// The same records against a map long enough for all of them: the
		// guard drops nothing and every record is handed on in order.
		set.oppositeNodeMapSize = 200;
		UnstitchGuardTally u = UnstitchGuardWalkStubs(4242, &set, 1, processed, 16);
		Check(u.connsExamined == 12 && u.connsSkipped == 0 && u.connsProcessed == 12,
		      "a healthy set loses nothing");
		bool inOrder = true;
		for (int i = 0; i < 12; ++i)
			if (processed[i] != i) inOrder = false;
		Check(inOrder, "and is handed on record by record in order");
	}

	// The fault's own pair on its own, either side of the boundary.
	{
		const int one[1] = { 58 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeIndices = one; set.connCount = 1;

		set.oppositeNodeMapSize = 55;
		Check(UnstitchGuardWalkStubs(1, &set, 1, 0, 0).connsSkipped == 1,
		      "58 against 55 fires the guard");
		set.oppositeNodeMapSize = 59;
		UnstitchGuardTally t = UnstitchGuardWalkStubs(1, &set, 1, 0, 0);
		Check(t.connsSkipped == 0 && t.connsProcessed == 1,
		      "58 against 59 does not fire it");
	}

	// A set naming another section is not this teardown's business: the guard
	// walks past it and drops nothing there, however wild its records.
	{
		const int wild[1] = { 0x7FFFFFFF };
		UnstitchSetStub sets[2];
		sets[0].thisUid = 5; sets[0].oppositeUid = 6; sets[0].oppositeResolved = true;
		sets[0].oppositeNodeMapSize = 4; sets[0].oppositeNodeIndices = wild; sets[0].connCount = 1;
		sets[1].thisUid = 1; sets[1].oppositeUid = 6; sets[1].oppositeResolved = true;
		sets[1].oppositeNodeMapSize = 4; sets[1].oppositeNodeIndices = wild; sets[1].connCount = 1;
		UnstitchGuardTally t = UnstitchGuardWalkStubs(1, sets, 2, 0, 0);
		Check(t.setsSeen == 2 && t.setsMatched == 1 && t.connsExamined == 1 && t.connsSkipped == 1,
		      "only the set naming this section is walked");
	}

	// An unresolved opposite section is skipped by the native walk before any
	// record is read, so the guard neither examines nor drops anything there.
	{
		const int idx[1] = { 999 };
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = false;
		set.oppositeNodeMapSize = 4; set.oppositeNodeIndices = idx; set.connCount = 1;
		UnstitchGuardTally t = UnstitchGuardWalkStubs(1, &set, 1, 0, 0);
		Check(t.setsMatched == 1 && t.oppositeResolved == 0
		      && t.connsExamined == 0 && t.connsSkipped == 0,
		      "an unresolved opposite section is matched but never walked");
	}

	// Lengths the walk will not trust are counted, never walked and never
	// dropped in silence.
	{
		UnstitchSetStub set;
		set.thisUid = 1; set.oppositeUid = 2; set.oppositeResolved = true;
		set.oppositeNodeMapSize = 4; set.oppositeNodeIndices = 0;
		set.connCount = UNSTITCH_MAX_CONNS + 1;
		UnstitchGuardTally t = UnstitchGuardWalkStubs(1, &set, 1, 0, 0);
		Check(t.implausible == 1 && t.connsExamined == 0,
		      "an untrustworthy connection count is refused, not walked");

		UnstitchGuardTally u = UnstitchGuardWalkStubs(1, 0, UNSTITCH_MAX_SETS + 1, 0, 0);
		Check(u.implausible == 1 && u.setsSeen == 0,
		      "an untrustworthy set count refuses the whole array");
	}

	return CheckExit("unstitch_guard_units");
}
