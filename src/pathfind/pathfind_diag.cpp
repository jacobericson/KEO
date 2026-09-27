// pathfind_diag.cpp - Diagnostic counters and shared probe/player state.
// The path hooks bump the counters with Interlocked* on their own threads.

#include "pathfind/pathfind_diag.h"
#include "pathfind/pathfind_diag_internal.h"

// =========================================================================
// Diagnostic counter definitions
// =========================================================================

volatile long diagPrimaryAttempts  = 0;
volatile long diagPrimarySuccess   = 0;
volatile long diagPrimaryFail      = 0;

volatile long diagConnAttempts     = 0;
volatile long diagConnFail         = 0;

volatile long diagAstarAttempts    = 0;
volatile long diagAstarSuccess     = 0;
volatile long diagAstarUnreachable = 0;
volatile long diagAstarTerminated  = 0;
volatile long diagAstarTruncated   = 0;
volatile long diagAstarInvalid     = 0;
volatile long diagAstarOther       = 0;

volatile long diagWaveStarted[3]   = { 0, 0, 0 };
volatile long diagWaveSuccess[3]   = { 0, 0, 0 };
volatile long diagWaveUnreach[3]   = { 0, 0, 0 };
volatile long diagWaveTerm[3]      = { 0, 0, 0 };
volatile long diagWaveInvalid[3]   = { 0, 0, 0 };
volatile long diagWaveOther[3]     = { 0, 0, 0 };
volatile long diagWaveStale        = 0;

volatile long diagWaveStartedByGate[3] = { 0, 0, 0 };
volatile long diagWaveSuccessByGate[3] = { 0, 0, 0 };
volatile long diagWaveLabelDisagree    = 0;
volatile long diagWaveStartedByTag[2]  = { 0, 0 };
volatile long diagWaveSuccessByTag[2]  = { 0, 0 };
volatile long diagConnRejectByGate[3]  = { 0, 0, 0 };

volatile long diagTermIterLimit    = 0;
volatile long diagTermOpenSetFull  = 0;
volatile long diagTermStatesFull   = 0;
volatile long diagTermOtherCause   = 0;

volatile long diagMaxIterUsed      = 0;
volatile long diagLastTermIter     = 0;

volatile long diagPlayerRequests   = 0;
volatile long diagNPCRequests      = 0;

volatile long diagPlayerCap        = 0;


// =========================================================================
// FindPathInput layout probe state
// =========================================================================

volatile long  probeFPIDumped      = 0;
volatile float probeStartPos[4]    = {};
volatile float probeGoalPos[4]     = {};
volatile long  probeGoalPtrValid   = 0;
volatile long  probeFields[14]     = {};


// =========================================================================
// Multi-call path probe state
// =========================================================================

volatile long  pathProbeArmed    = 0;
volatile long  pathProbeWriteIdx = 0;
PathProbeEntry pathProbeBuf[PATH_PROBE_SIZE];
double         pathProbeArmTime  = 0.0;


// =========================================================================
// Player failure tracking state
// =========================================================================

AstarFailDetail lastAstarFail = {};

PlayerFailEntry playerFailRing[PLAYER_FAIL_RING] = {};
volatile long   playerFailWriteIdx    = 0;
volatile long   playerRequestsInFlight = 0;

TrackedPlayerDest trackedPlayers[MAX_TRACKED_PLAYERS] = {};
int trackedPlayerCount = 0;


// =========================================================================
// Reporter timing + per-window snapshots (main thread only)
// =========================================================================

namespace pathfind_diag_detail {

double lastPathDiagLogTime = 0.0;

long prevPrimaryAttempts = 0;
long prevPrimarySuccess  = 0;
long prevPrimaryFail     = 0;
long prevAstarAttempts   = 0;
long prevAstarSuccess    = 0;
long prevAstarUnreach    = 0;
long prevAstarTerminated = 0;
long prevAstarInvalid    = 0;
long prevConnAttempts    = 0;
long prevConnFail        = 0;
long prevPlayerRequests  = 0;
long prevNPCRequests     = 0;
long prevPlayerCap       = 0;
long prevFailSequence    = 0;

} // namespace pathfind_diag_detail
using namespace pathfind_diag_detail;
