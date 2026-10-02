#ifndef KEO_ORDER_OUTCOME_POLICY_H
#define KEO_ORDER_OUTCOME_POLICY_H

// Pure arithmetic and line formatting behind the player order-outcome
// metric (order_outcome_table.cpp). No game headers: host-testable, and the
// record-keeper and the host tests share one definition of what counts.

#include <string>

// A player order spanning this many cells or more is "long".
bool OrderOutcomeIsLong(int cellSpan);

// A stall shorter than this never counts as a stop.
bool OrderOutcomeStallQualifies(double stallSeconds);

// A K7-family re-issue send within this many seconds before a stall opens
// still credits the eventual recovery to K7: a stall only opens on the
// first fully-still 1s poll, 1-2s after the real stop, so a fast recovery
// can resolve before there was ever an open stall to latch the send into.
const double OO_K7_SEND_LOOKBACK = 2.0;

// Within 100 units of the order's own destination: PLAYER STUCK's post= and
// the "already arrived" check share this test.
bool OrderOutcomeIsPostArrival(float dx, float dz);

enum OrderOutcomeRecovery
{
	OO_REC_NONE,    // not a counted stop: too short, excluded, a start delay,
	                // or eviction (a bookkeeping event, not an outcome)
	OO_REC_SELF,    // motion resumed on its own, no K7 send in the window
	OO_REC_K7,      // motion resumed after a K7-family re-issue landed
	                // inside the stall
	OO_REC_USER,    // a new player order (or a cancel) ended the member's
	                // participation while the stall was open
	OO_REC_UNREC    // the stall was still open when its record closed
};

// How a stall episode ended:
//   OO_RESOLVE_MOTION     the character started moving again
//   OO_RESOLVE_SUPERSEDE  a new player order, or a cancel, ended it
//   OO_RESOLVE_CLOSE      its order record closed while still stalled
//   OO_RESOLVE_EVICT       the table reclaimed the slot for a newer order
enum OrderOutcomeResolution
{
	OO_RESOLVE_MOTION,
	OO_RESOLVE_SUPERSEDE,
	OO_RESOLVE_CLOSE,
	OO_RESOLVE_EVICT
};

// excluded: the character went unconscious or reached its own order's
// destination sometime during the stall -- neither is a routing failure, so
// the episode is dropped rather than counted in any bucket.
// k7SentDuringStall: a K7-family re-issue (island_reissue.cpp ReissueOrder's
// one success point, every reason) landed between the stall's start and its
// resolution.
OrderOutcomeRecovery OrderOutcomeClassifyStop(bool qualifies, bool excluded,
                                               OrderOutcomeResolution how,
                                               bool k7SentDuringStall);

// The PLAYER STUCK suffix: " ko=<0|1> hc=<n|-> post=<0|1>".
std::string OrderOutcomeStuckSuffix(bool ko, bool haveHc136, int hc136, bool post);

// One "OrderOutcome:" line. `end` is one of done/cancel/supersede/timeout/
// evict/reset -- why the record closed, not what happened to any one member.
std::string OrderOutcomeFormatLine(int orderNum, double issueTime, int cellSpan,
                                    int members, int walked, int arrived, int ko,
                                    int k7rec, int userRec, int unrec, int selfRec,
                                    double stallCharS, double maxStallS,
                                    const std::string& stopsGuess, const std::string& end);

// The " orders=<n> longOrders=<n> longStop=<n> longFail=<n> userRec=<n>
// unrec=<n>" suffix appended to the PROD-visible IslandSpan: line. orders= is
// every order; the rest count only cells>=9 orders (longOrders is that
// denominator; userRec/unrec are character-stall counts, for severity).
std::string OrderOutcomeFormatSpanTotals(long orders, long longOrders, long longStop,
                                          long longFail, long userRec, long unrec);

// The planner column appended to an "OrderOutcome:" line while the route planner is armed: stalls
// whose wait the planner owned, counted there instead of as stops.
std::string OrderOutcomePlannerSuffix(int plannerWait);   // " plannerWait=<n>"

#endif // KEO_ORDER_OUTCOME_POLICY_H
