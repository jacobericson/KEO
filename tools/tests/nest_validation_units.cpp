#include <cstdio>
#include "fixes/world/nest_validation_policy.h"

#include "check.h"

int main()
{
	// Record -> cell decode: in-range coordinates decode, row-major, matching
	// the engine's own areasList + 136 + 184*(y + (x<<6)) formula.
	{
		int idx = -1;
		Check(NestValidationCellFromRecord(0, 0, &idx) && idx == 0, "origin decodes to index 0");
	}
	{
		int idx = -1;
		Check(NestValidationCellFromRecord(1, 0, &idx) && idx == 64, "x=1,y=0 decodes to index 64 (x<<6)");
	}
	{
		int idx = -1;
		Check(NestValidationCellFromRecord(0, 5, &idx) && idx == 5, "x=0,y=5 decodes to index 5");
	}
	{
		int idx = -1;
		Check(NestValidationCellFromRecord(63, 63, &idx) && idx == 63 * 64 + 63,
		      "the top corner (63,63) decodes to the last cell");
	}

	// Out-of-range coordinates (a record this guard does not recognise) never
	// produce an index: the caller falls back to running the original.
	{
		int idx = 0;
		Check(!NestValidationCellFromRecord(64, 0, &idx), "x=64 is out of range");
	}
	{
		int idx = 0;
		Check(!NestValidationCellFromRecord(0, 64, &idx), "y=64 is out of range");
	}
	{
		int idx = 0;
		Check(!NestValidationCellFromRecord(0xFFFFFFFFu, 0, &idx), "a corrupt huge x is rejected");
	}

	// Decision: mesh not ready starts a skip streak on the first call
	// (countSkip) but never again while the streak continues -- a streak
	// counts once, not once per call, or a multi-cycle streak would show a
	// permanent skipped > revalidated gap even though nothing is wrong.
	{
		NestValidationDecision d = NestValidationDecide(false, false);
		Check(d.action == NESTVAL_SKIP, "not ready, never skipped before: skip");
		Check(d.countSkip, "the first call in a streak is counted");
		Check(!d.countRevalidated, "no revalidation on a fresh skip");
	}
	{
		NestValidationDecision d = NestValidationDecide(false, true);
		Check(d.action == NESTVAL_SKIP, "not ready, already skipped: still skip");
		Check(!d.countSkip, "a continuing streak is not counted again");
		Check(!d.countRevalidated, "not ready never counts as revalidated");
	}

	// Decision: mesh ready proceeds; it only counts as a revalidation when a
	// prior skip streak on this cell is the reason there is anything to pair.
	{
		NestValidationDecision d = NestValidationDecide(true, false);
		Check(d.action == NESTVAL_PROCEED, "ready, never skipped: proceed");
		Check(!d.countSkip, "a proceed is never counted as a skip");
		Check(!d.countRevalidated, "nothing to pair without an earlier skip");
	}
	{
		NestValidationDecision d = NestValidationDecide(true, true);
		Check(d.action == NESTVAL_PROCEED, "ready, previously skipped: proceed");
		Check(!d.countSkip, "a proceed is never counted as a skip");
		Check(d.countRevalidated, "this pairs with the earlier skip streak");
	}

	// A full multi-cycle streak (not-ready, not-ready, not-ready, then ready)
	// produces exactly one skip and one revalidation, matching a real
	// cold-cache session spanning several loading cycles on the same cell.
	{
		bool wasSkipped = false;
		int skipCount = 0, revalidatedCount = 0;

		NestValidationDecision d1 = NestValidationDecide(false, wasSkipped);
		if (d1.countSkip) ++skipCount;
		if (d1.countRevalidated) ++revalidatedCount;
		wasSkipped = (d1.action == NESTVAL_SKIP);

		NestValidationDecision d2 = NestValidationDecide(false, wasSkipped);
		if (d2.countSkip) ++skipCount;
		if (d2.countRevalidated) ++revalidatedCount;
		wasSkipped = (d2.action == NESTVAL_SKIP);

		NestValidationDecision d3 = NestValidationDecide(false, wasSkipped);
		if (d3.countSkip) ++skipCount;
		if (d3.countRevalidated) ++revalidatedCount;
		wasSkipped = (d3.action == NESTVAL_SKIP);

		NestValidationDecision d4 = NestValidationDecide(true, wasSkipped);
		if (d4.countSkip) ++skipCount;
		if (d4.countRevalidated) ++revalidatedCount;

		Check(skipCount == 1, "a 3-cycle streak counts exactly one skip");
		Check(revalidatedCount == 1, "a 3-cycle streak counts exactly one revalidation");
	}

	return CheckExit("nest_validation_units");
}
