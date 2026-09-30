#include "movement/order_outcome_policy.h"
#include <sstream>
#include <iomanip>

bool OrderOutcomeIsLong(int cellSpan)
{
	return cellSpan >= 9;
}

bool OrderOutcomeStallQualifies(double stallSeconds)
{
	return stallSeconds >= 2.0;
}

bool OrderOutcomeIsPostArrival(float dx, float dz)
{
	return (dx * dx + dz * dz) < 10000.0f;   // < 100 units
}

OrderOutcomeRecovery OrderOutcomeClassifyStop(bool qualifies, bool excluded,
                                               OrderOutcomeResolution how,
                                               bool k7SentDuringStall)
{
	if (!qualifies || excluded)
		return OO_REC_NONE;
	if (how == OO_RESOLVE_EVICT)
		return OO_REC_NONE;   // a bookkeeping event, not an outcome
	if (how == OO_RESOLVE_SUPERSEDE)
		return OO_REC_USER;
	if (how == OO_RESOLVE_CLOSE)
		return OO_REC_UNREC;
	// OO_RESOLVE_MOTION: recovered on its own unless a K7 send landed inside
	// the stall, in which case K7 gets the credit.
	return k7SentDuringStall ? OO_REC_K7 : OO_REC_SELF;
}

std::string OrderOutcomeStuckSuffix(bool ko, bool haveHc136, int hc136, bool post)
{
	std::ostringstream ss;
	ss << " ko=" << (ko ? 1 : 0)
	   << " hc=";
	if (haveHc136) ss << hc136;
	else           ss << "-";
	ss << " post=" << (post ? 1 : 0);
	return ss.str();
}

std::string OrderOutcomeFormatLine(int orderNum, double issueTime, int cellSpan,
                                    int members, int walked, int arrived, int ko,
                                    int k7rec, int userRec, int unrec, int selfRec,
                                    double stallCharS, double maxStallS,
                                    const std::string& stopsGuess, const std::string& end)
{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "OrderOutcome: order=#" << orderNum
	   << " t=" << issueTime
	   << " cells=" << cellSpan
	   << " members=" << members
	   << " walked=" << walked
	   << " arrived=" << arrived
	   << " ko=" << ko
	   << " k7rec=" << k7rec
	   << " userRec=" << userRec
	   << " unrec=" << unrec
	   << " selfRec=" << selfRec
	   << " stallCharS=" << stallCharS
	   << " maxStallS=" << maxStallS
	   << " stops=" << (stopsGuess.empty() ? "-" : stopsGuess)
	   << " end=" << (end.empty() ? "-" : end);
	return ss.str();
}

std::string OrderOutcomeFormatSpanTotals(long orders, long longOrders, long longStop,
                                          long longFail, long userRec, long unrec)
{
	std::ostringstream ss;
	ss << " orders=" << orders
	   << " longOrders=" << longOrders
	   << " longStop=" << longStop
	   << " longFail=" << longFail
	   << " userRec=" << userRec
	   << " unrec=" << unrec;
	return ss.str();
}

std::string OrderOutcomePlannerSuffix(int plannerWait)
{
	std::ostringstream ss;
	ss << " plannerWait=" << plannerWait;
	return ss.str();
}
