#include <cstdio>
#include "movement/order_outcome_policy.h"

#include "check.h"

int main()
{
	// Distance bands: the triage's own threshold.
	Check(!OrderOutcomeIsLong(8), "8 cells is not long");
	Check(OrderOutcomeIsLong(9), "9 cells is long");
	Check(OrderOutcomeIsLong(29), "a (50,33)->(21,42) order (29 cells) is long");

	// The 2 s floor.
	Check(!OrderOutcomeStallQualifies(1.9), "1.9s is not a stop");
	Check(OrderOutcomeStallQualifies(2.0), "2.0s is a stop");
	Check(OrderOutcomeStallQualifies(9.3), "9.3s is a stop");

	// post= is a position test, independent of any signature.
	Check(!OrderOutcomeIsPostArrival(741.0f, 0.0f), "741 units short is not post");
	Check(!OrderOutcomeIsPostArrival(120.0f, 0.0f), "just over 100 units short is not post");
	Check(OrderOutcomeIsPostArrival(50.0f, 50.0f), "within 100 units is post");
	Check(OrderOutcomeIsPostArrival(0.0f, 0.0f), "on the destination is post");

	// A stall under 2s is never a stop, whatever else is true.
	Check(OrderOutcomeClassifyStop(false, false, OO_RESOLVE_MOTION, true) == OO_REC_NONE,
	      "a stall under 2s is not a stop even with a K7 send");
	Check(OrderOutcomeClassifyStop(false, false, OO_RESOLVE_CLOSE, false) == OO_REC_NONE,
	      "a stall under 2s closing the order is still not a stop");

	// A knockout/post-arrival exclusion drops the stall entirely, whatever
	// resolution ends it.
	Check(OrderOutcomeClassifyStop(true, true, OO_RESOLVE_MOTION, true) == OO_REC_NONE,
	      "an excluded stall that resumes moving is not k7rec");
	Check(OrderOutcomeClassifyStop(true, true, OO_RESOLVE_CLOSE, false) == OO_REC_NONE,
	      "an excluded stall still open at close is not unrec");
	Check(OrderOutcomeClassifyStop(true, true, OO_RESOLVE_SUPERSEDE, false) == OO_REC_NONE,
	      "an excluded stall superseded by a new order is not userRec");

	// A K7-family re-issue landing in the stall gets credit when motion
	// resumes; with no send it is self-resolved (its own bucket).
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_MOTION, true) == OO_REC_K7,
	      "motion resuming after a K7 send is k7rec");
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_MOTION, false) == OO_REC_SELF,
	      "motion resuming with no K7 send in the window is selfRec");

	// A user re-order (or a cancel) always wins over a K7 send: a recorded shape
	// (three deleted re-issues moved=0, then the user stepped in) must read
	// userRec=1, k7rec=0, not the other way around.
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_SUPERSEDE, true) == OO_REC_USER,
	      "a user re-order after K7 sends is userRec, not k7rec");
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_SUPERSEDE, false) == OO_REC_USER,
	      "a user re-order with no K7 send is userRec");

	// A stall still open when its record closes (no motion, no supersede) is
	// the genuine failure case.
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_CLOSE, false) == OO_REC_UNREC,
	      "a stall never resolved by close time is unrec");
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_CLOSE, true) == OO_REC_UNREC,
	      "a K7 send with no motion before close is still unrec, not k7rec");

	// Eviction is a bookkeeping event, never an outcome, whatever else is
	// true about the stall it interrupts.
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_EVICT, false) == OO_REC_NONE,
	      "eviction is not unrec");
	Check(OrderOutcomeClassifyStop(true, false, OO_RESOLVE_EVICT, true) == OO_REC_NONE,
	      "eviction is not k7rec");
	Check(OrderOutcomeClassifyStop(true, true, OO_RESOLVE_EVICT, false) == OO_REC_NONE,
	      "eviction of an excluded stall is still not counted");

	// The span-totals format: orders is all orders; the rest are
	// cells>=9 only.
	Check(OrderOutcomeFormatSpanTotals(3, 1, 1, 1, 1, 0) ==
	      " orders=3 longOrders=1 longStop=1 longFail=1 userRec=1 unrec=0",
	      "span totals format");

	// The planner column appended while the route planner is armed.
	Check(OrderOutcomePlannerSuffix(0) == " plannerWait=0" && OrderOutcomePlannerSuffix(3) == " plannerWait=3",
	      "suffix: plannerWait=<n>");

	// The order's coordinates, written after cells=.
	Check(OrderOutcomeFormatCoords(true, -1234.4f, 567.6f, 89.0f, -10.0f) == " from=(-1234,568) to=(89,-10)",
	      "coords: from=(x,z) to=(x,z) rounded to the unit");
	Check(OrderOutcomeFormatCoords(false, 1.0f, 2.0f, 3.0f, 4.0f) == " from=- to=-", "coords: none reads from=- to=-");
	{
		std::string line = OrderOutcomeFormatLine(1, 2.0, 9, OrderOutcomeFormatCoords(true, 10.0f, 20.0f, 30.0f, 40.0f),
		                                          2, 2, 2, 0, 0, 0, 0, 0, 0.0, 0.0, "", "done");
		Check(line.find("OrderOutcome: order=#1 t=2.0 cells=9 from=(10,20) to=(30,40) members=2 ") == 0,
		      "outcome line: from= and to= follow cells=");
	}

	return CheckExit("order_outcome_units");
}
