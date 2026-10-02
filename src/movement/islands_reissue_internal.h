// islands_reissue_internal.h - main-thread private tracker types and helpers.
// The record table and K7 counters are defined in islands_reissue.cpp; other
// shared state lives beside its writers. This header declares the private contract.
#ifndef KEO_ISLANDS_REISSUE_INTERNAL_H
#define KEO_ISLANDS_REISSUE_INTERNAL_H
#include "movement/islands.h"

namespace order_tracker_detail {
// PollOrders and IsCharacterParkedNow use this private order-type reader for
// the stopped-form check. Its one definition is in island_reissue.cpp, beside
// the discriminator trace; it is not part of islands.h's public API.
int ReadCharOrderType(uintptr_t character);

struct IslandOrder {
	bool      active;
	uintptr_t character;
	float     destX, destY, destZ;
	double    orderTime;
	bool      parked;
	bool      parkedViaStop;   // (e): parked via the stop()-ed test, not the edge test
	float     parkX, parkZ;
	int       parkGX, parkGY;
	double    parkTime;
	bool      haveX0;
	float     x0X, x0Z;
	int       reissueCount;
	int       limitGX, limitGY;    // zone where the re-issue budget started
	double    lastReissueTime;
	bool      retryArmed;
	float     retryX, retryZ;      // crossing at the last re-issue
	unsigned int seenGen;
	unsigned int seenSig;
	bool      noCrossingLogged;
	double    stoppedSince;    // (c): 0.0 = stopped predicate not currently holding
	// K7 deleted-order form (K7Observe / K7SampleSignatures /
	// K7TryDeletedReissue). IslandNoteOrder's memset zeroes all of it on
	// every new player move order; a deleted re-issue restarts the episode.
	bool      k7Seen29;        // current task 29 seen since the order (or the last deleted re-issue)
	bool      k7HaveDest29;    // k7Dest29X/Z valid
	float     k7Dest29X, k7Dest29Z;   // CharMovement +0xDC at the last poll with task 29
	double    k7ReachedTime;   // last frame with hc136==1 && !movingToEdge && dist>100 (0 = none)
	double    k7FailedTime;    // last frame with HavokCharacter path state 3 (0 = none)
	double    k7DeletedSince;  // first poll of task -1 + empty order deque after k7Seen29 (0 = not)
	bool      k7Counted;       // this deletion episode is already counted in delPark=
	bool      k7Preempted;     // last poll: another task runs, the move still queued
	bool      k7HaveLastSend;  // k7LastSendX/Z valid
	float     k7LastSendX, k7LastSendZ;  // position at the previous deleted re-issue (moved=)

	// The swap classifier's own timestamps.
	double    k7Last29Time;    // last poll with curType == ORDER_TYPE_MOVE
	double    k7SigOnset;      // K7SigOnsetStep latch: first frame of the end
	                           // signature since k7Last29Time (0 = none)
	double    k7SwapSeenTime;  // first poll this episode with curType outside {29,-1}
	bool      k7PostDeathHold; // a died-first combat swap is being held, not dropped

	// Consecutive-refusal clock for the destination-readiness wait.
	double    k7DestWaitSince;

	// Arrival wait: armed on the *rising edge* of K7SampleSignatures' own
	// end signature (never while a formation is still gathering), independent
	// of k7DeletedSince -- it survives a live continuation of task 29, not
	// only the "deleted" path, but only covers that live shape when the
	// destination cell was itself not-in-world within the last
	// K7_ARRIVAL_RECENT_TRANSITION seconds (k7DestLastNotIn, below); a
	// signature that fires long after the cell settled in does not arm.
	// 0 = not armed. k7ArrivalGX/GY is the destination cell latched at arm
	// time. k7ArrivalWouldFireTime (k7ArrivalTrigger=false only) is 0 until
	// the fire condition is first seen; the real K7 send (K7TryDeletedReissue)
	// logs the observe line and clears it once it actually fires.
	double    k7ArrivalWaitSince;
	int       k7ArrivalGX, k7ArrivalGY;
	double    k7ArrivalWouldFireTime;
	// The last poll (any poll, armed or not) that saw the destination cell
	// read not-in-world -- kept for every far tracked entry so an arm
	// attempted just after the cell already streamed in can still recognise a
	// recent transition (0 = never observed not-in-world this episode).
	double    k7DestLastNotIn;
	// The previous poll's signature state (reached || failed), so the arm
	// test can find a rising edge instead of re-arming (or never re-arming)
	// on a signature that just keeps holding.
	bool      k7ArrivalPrevSig;
	// Once per order: the "K7 arrival armed" line fires on the first arm of
	// this order, not on a later re-arm within the same order (a retried leg
	// can arm more than once; only the first is worth a line).
	bool      k7ArrivalArmLogged;
};
const int    REISSUE_LABEL_LEN      = 40;    // "group 7 member 29 char@ffff", "char@ffff"

const int    MAX_ISLAND_ORDERS   = 64;
const float  PARK_MIN_DEST_DIST  = 100.0f;  // farther than this from the order destination
const int    MAX_REISSUES        = 8;
// (c) The stopped form of the park test in island_orders.cpp must hold
// continuously for this long before it counts as parked -- a single bad poll
// (mid-frame state change, a still-settling order) must not park a character
// about to move again. The edge form keeps its existing (unhysteresised)
// behaviour.
const double STOPPED_HYSTERESIS  = 3.0;
const int    MAX_REISSUE_CHECKS     = 256;

struct ReissueCheck {
	bool      active;
	uintptr_t character;     // key only: never dereferenced before the live test
	char      label[REISSUE_LABEL_LEN];
	float     sentX, sentZ;  // destination handed to fn_moveOrder (after the nudge)
	IslandReissueTrace pre;  // captured immediately before fn_moveOrder
	double    issueTime;
	int       dispatch;      // g_reissueDispatches index, -1 = prints its own line
	int       overtaken;     // IslandOvertakeReason bits (islands.h), 0 = none
};
inline float Dist2(float ax, float az, float bx, float bz)
{
	float dx = ax - bx, dz = az - bz;
	return dx * dx + dz * dz;
}
} // namespace order_tracker_detail
namespace order_tracker_detail {
// A character's order state, read in one go.
struct K7OrderState {
	bool      ok;        // AI -> task system readable, deque size sane
	long long size;      // orders.list._Mysize (ts+0x60)
	uintptr_t head;      // Tasker* at the deque front, 0 = empty / unreadable
	int       headType;  // its TaskData::key, -1 = none
	int       curType;   // CharBody::currentAction type (ReadCharOrderType), -1 = none
};

// Shared order table and count, defined in islands_reissue.cpp.
extern IslandOrder g_orders[MAX_ISLAND_ORDERS];
extern int g_orderCount;
// Session reissue tally, defined in island_reissue.cpp.
extern volatile long g_reissues;
// Installed cancel-hook flags, defined in island_cancel_hooks.cpp.
extern bool g_cancelStopInstalled;
extern bool g_cancelJobInstalled;
extern bool g_cancelTaskInstalled;
// Pending-check table and session tallies, defined in island_reissue.cpp.
extern ReissueCheck g_reissueChecks[MAX_REISSUE_CHECKS];
extern long g_reissuePostSent;
extern long g_reissuePostLast;
extern long g_reissuePostOther;
extern long g_reissueCheckDropped;
extern long g_reissueCheckEarly;

long K7ArrivalOpenWaits();
void ResetOrders();
bool TrackerPlayerList(uintptr_t** outStuff, unsigned int* outCount);
bool TrackerListHas(const uintptr_t* stuff, unsigned int count, uintptr_t character);
bool TrackerIsLivePlayerCharacter(uintptr_t character);
void ResolveDueReissueChecks(double now);
bool ReissueCharacter(uintptr_t character, float dx, float dy, float dz, double now);
bool ReissueOrder(IslandOrder& o, double now, const char* why, bool haveCross, float cx, float cz, bool forceCharacterOnly, const char* k7Fields = NULL);
IslandOrder* FindOrderForCharacter(uintptr_t character);
bool IsCharacterParkedNow(uintptr_t character, float destX, float destZ, double now);
bool K7FormOn();
bool K7FormOnLogged();
bool K7ReadOrders(uintptr_t character, K7OrderState* st);
bool K7IsUnconcious(uintptr_t character);
bool K7ZonesAccessible(uintptr_t zm, float posX, float posZ, float destX, float destZ);
void K7SampleSignatures(double now, bool paused);
bool K7Observe(IslandOrder& o, uintptr_t cm, double now, bool* deleted);
bool K7TryDeletedReissue(IslandOrder& o, uintptr_t zm, uintptr_t cm, float posX, float posZ, int gx, int gy, double now);
bool K7TryArrivalReissue(IslandOrder& o, uintptr_t zm, uintptr_t cm, float posX, float posZ, int gx, int gy, double now);
void IslandDetachSelectedFromFormation(uintptr_t pi);
int K7DropSelected(uintptr_t pi);
void PollOrders(uintptr_t zm, double now);
void ResetReissueChecks();
void K7RebasePausedClocks(bool paused, double now);
} // namespace order_tracker_detail
#endif // KEO_ISLANDS_REISSUE_INTERNAL_H
