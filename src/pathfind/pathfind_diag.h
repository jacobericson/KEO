// pathfind_diag.h -- Pathfinding diagnostic state, probes, player failure tracking (Layer 3)
// Depends on: config.h

#ifndef KENSHI_ZONE_OPT_PATHFIND_DIAG_H
#define KENSHI_ZONE_OPT_PATHFIND_DIAG_H

#include "base/config.h"


// =========================================================================
// PathProbeEntry (moved from config.h)
// =========================================================================

const int PATH_PROBE_SIZE = 24;

struct PathProbeEntry {
	float startX, startY, startZ;
	float destX, destY, destZ;
	int   hookType;   // 1=csFindPath, 2=findPathFull, 3=csCheckFaceConn
	int   result;     // search status (1=OK, 2=unreachable, 3=terminated, 5=invalid)
	unsigned int faceKey;
};


// =========================================================================
// Diagnostic counters (volatile, Interlocked from bg thread)
// =========================================================================

// Hook 1: Primary path (ContentStream::findPath)
extern volatile long diagPrimaryAttempts;
extern volatile long diagPrimarySuccess;
extern volatile long diagPrimaryFail;

// Hook 2: Face connectivity check
extern volatile long diagConnAttempts;
extern volatile long diagConnFail;

// Hook 3: Full A* search
extern volatile long diagAstarAttempts;
extern volatile long diagAstarSuccess;
extern volatile long diagAstarUnreachable;
extern volatile long diagAstarTerminated;
extern volatile long diagAstarTruncated;
extern volatile long diagAstarInvalid;
extern volatile long diagAstarOther;

// Requester arms for the waved-through buckets below.
enum WaveOwner { WAVE_NPC = 0, WAVE_PLAYER = 1, WAVE_UNK = 2 };

// Waved-through searches: pairs the cluster graph rejected under
// clusterGraphBypass=measure, and what the search it gated returned. Started
// is incremented before the search runs and the outcome after, so
// started - (success + unreach + term + invalid + other) is the number of
// waved searches that never came back. Stale counts labels a searching thread
// dropped, which bounds how far the outcomes can be mislabelled. All six stay
// 0 outside measure mode, where no answer is computed -- the report prints
// "n/a" there rather than zeroes.
//
// Each bucket is split by who the search belongs to, indexed
// WAVE_NPC / WAVE_PLAYER / WAVE_UNK. The label is the engine's own verdict on
// the owning character, taken from the argument it passes to the full-path
// call; a search that reached this point without one lands in WAVE_UNK rather
// than being guessed into either arm, so an arm's count is only requests
// actually attributed to it. The three arms sum to the pooled total, which is
// what makes the rate per arm readable.
extern volatile long diagWaveStarted[3];
extern volatile long diagWaveSuccess[3];
extern volatile long diagWaveUnreach[3];
extern volatile long diagWaveTerm[3];
extern volatile long diagWaveInvalid[3];
extern volatile long diagWaveOther[3];
extern volatile long diagWaveStale;

// The same population labelled by the queue priority of the request being
// served. This is the label the connectivity check itself has to use, because
// the engine does not resolve the owning character until after the check has
// answered, so it is what decides who gets waved through under the player-only
// mode. Disagree counts the searches it and the authoritative label above
// disagree about, which is that decision's miss rate; a search missing either
// label is not counted as a disagreement.
extern volatile long diagWaveStartedByGate[3];
extern volatile long diagWaveSuccessByGate[3];
extern volatile long diagWaveLabelDisagree;

// And labelled by the in-flight-counter tag, a third opinion carried because
// the counter is global and can drift independently of both labels above.
extern volatile long diagWaveStartedByTag[2];
extern volatile long diagWaveSuccessByTag[2];

// Connectivity rejections by the label the check had at the time, counted in
// every mode that consults the graph. Under the player-only mode the NPC and
// unattributed arms are the rejections that were obeyed, which no wave bucket
// can show, so this is the only place they appear.
extern volatile long diagConnRejectByGate[3];

// Termination cause breakdown (when status == 3)
extern volatile long diagTermIterLimit;
extern volatile long diagTermOpenSetFull;
extern volatile long diagTermStatesFull;
extern volatile long diagTermOtherCause;

// Iteration tracking
extern volatile long diagMaxIterUsed;
extern volatile long diagLastTermIter;

// Player vs NPC request counters (main thread, from hook_requestPath priority param)
extern volatile long diagPlayerRequests;
extern volatile long diagNPCRequests;

// Node-cap terminations (status 3, cause 3 -- diagTermStatesFull) on a
// request the queue already carried at player priority (req+44 >= 20,
// hook_findPathFull's own pathPoolPlayerByReq). See playerCap= on PathRate:.
extern volatile long diagPlayerCap;


// =========================================================================
// FindPathInput layout probe (one-time, bg thread writes, main thread reads)
// =========================================================================

extern volatile long  probeFPIDumped;
extern volatile float probeStartPos[4];
extern volatile float probeGoalPos[4];
extern volatile long  probeGoalPtrValid;
extern volatile long  probeFields[14];


// =========================================================================
// Multi-call path probe state
// =========================================================================

extern volatile long  pathProbeArmed;
extern volatile long  pathProbeWriteIdx;
extern PathProbeEntry pathProbeBuf[PATH_PROBE_SIZE];
extern double         pathProbeArmTime;


// =========================================================================
// Player failure tracking
// =========================================================================

// Last A* failure details — any request (bg thread writes, main thread reads)
struct AstarFailDetail {
	volatile float goalX, goalY, goalZ;
	volatile long  status;
	volatile long  cause;
	volatile long  iterCount;
	volatile long  sequence;  // monotonic for freshness
};

extern AstarFailDetail lastAstarFail;

// Per-player failure ring buffer (bg thread writes, main thread drains)
// Uses priority-based tagging: boosted player requests (pri 45) are at the
// top of the contentStream queue, so we count them down in hook_csFindPath.
struct PlayerFailEntry {
	volatile float goalX, goalZ;
	volatile long  status;
	volatile long  cause;
	volatile long  iterCount;
	volatile long  valid;  // 1=written by bg, 0=consumed by main
};

const int PLAYER_FAIL_RING = 16;
extern PlayerFailEntry playerFailRing[PLAYER_FAIL_RING];
extern volatile long   playerFailWriteIdx;
extern volatile long   playerRequestsInFlight;  // main increments, bg decrements


// =========================================================================
// Diagnostic function declarations
// =========================================================================

void LogPathfindDiagStats(double now);
void ArmPathProbe();
void DumpPathProbe(double now);


// Per-character PLAYER STUCK / PLAYER TASK tracking (main thread only). A
// diagnostic: it issues no orders (the stuckRetry recovery was deleted).
struct TrackedPlayerDest {
	uintptr_t character;        // Character* pointer (0 = empty slot)
	float destX, destY, destZ;  // world-space click destination
	float prevPosX, prevPosZ;   // previous poll position (for velocity)
	double clickTime;           // when the order was issued
	int  zeroVelocityPolls;     // consecutive polls with no movement
	bool active;                // slot in use
	// Far-arrival latch (SamplePlayerArrivals, per frame). The engine's
	// arrival state is transient -- it is cleared as soon as a new path is
	// set -- so the 1 s stuck poll almost never catches it, and destReach=
	// on a stuck line reads 0 even when an arrival is what ended the order.
	bool arrivedPrev;           // isDestinationReached at the previous frame
	int  farArrivals;           // arrivals declared far short of destX/destZ
	float farArriveMaxD;        // largest such distance, world units
	// The same-island test's two inputs, latched at the first far arrival.
	// The labels are reassigned from scratch on every island recalculation
	// and zeroed when a cell deactivates, so the values a stall reads a
	// second or more later are not the ones the routing branch acted on.
	bool arrHaveLabels;         // false = never latched, so the ints are unread
	int  arrSelfLabel, arrDestLabel, arrSpan;
	// The ko= latch (order_outcome.cpp): once true it stays true for this
	// order, so a character that woke from a knockout and is now standing
	// with no order still prints ko=1 -- the instantaneous test alone would
	// read 0 and make the wake-up stall look like a fresh routing failure.
	// post= needs no such latch: reaching the order's destination ends
	// tracking outright (below), so no later line is ever printed for it.
	bool orderKoLatched;
	// The start-delay exclusion (order_outcome.cpp) needs a real "is this
	// character moving" answer on the very first poll after a click too.
	// prevPosX/Z start at (0,0) (StorePlayerClickDest, above), so the plain
	// velocity check reads that first poll as a multi-thousand-unit jump and
	// always "moving" -- havePrev suppresses just that one poll's motion
	// report to order_outcome.cpp, so a queued or gathering member's real
	// start delay still has a chance to open before departure is recorded.
	bool havePrev;
};

const int MAX_TRACKED_PLAYERS = 256;
extern TrackedPlayerDest trackedPlayers[MAX_TRACKED_PLAYERS];
extern int trackedPlayerCount;

void StorePlayerClickDest(uintptr_t character, const float* dest, double now);
void PollPlayerMovementState(double now);

// Per frame, main thread: latch each tracked character's transition into the
// engine's "destination reached" state while it is still far from the
// destination the order was given for. That transition is what makes
// AI::isAtLocation answer true at any distance, which completes the move
// task and pops the order with no path failure recorded.
void SamplePlayerArrivals();

// Session totals for the Islands: line. Count 0 with max 0.0 means no far
// arrival was seen; a far arrival always carries a positive distance, so the
// two are never confused.
long  PlayerFarArrivals();
float PlayerFarArriveMaxDist();


#endif // KENSHI_ZONE_OPT_PATHFIND_DIAG_H
