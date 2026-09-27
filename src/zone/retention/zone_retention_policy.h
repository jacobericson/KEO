#ifndef KENSHI_ZONE_OPT_ZONE_RETENTION_POLICY_H
#define KENSHI_ZONE_OPT_ZONE_RETENTION_POLICY_H

// The arithmetic behind holding a Set B cell past its native expiry: the
// value written into the town countdown, the two-stage keep decision, the
// release pacing and the soft-cap hysteresis. No game types and no game
// state — the caller reads the cell and the world, this decides.

// Chebyshev radii around a live camera or player anchor. The hard radius is
// what survives pressure; the wider one is the ordinary hysteresis.
const int ZONE_RETENTION_HARD_RADIUS       = 1;
const int ZONE_RETENTION_HYSTERESIS_RADIUS = 2;

// Seconds a cell is kept after the game takes it over, and the lease a
// predicted use buys.
const double ZONE_RETENTION_MIN_RESIDENCE_SEC   = 10.0;
const double ZONE_RETENTION_PREDICTION_LEASE_SEC = 10.0;

// Between two releases, when there is no pressure.
const double ZONE_RETENTION_RELEASE_SPACING_SEC = 2.0;

// Cells held past native expiry: above the cap the policy stops holding
// anything discretionary, and it keeps doing so until the count falls below
// the low water mark. Held, not tracked -- a tracked cell inside a live
// camera or player lease costs nothing and must not count towards a cap on
// what the policy is keeping alive by itself.
const int ZONE_RETENTION_SOFT_CAP  = 45;
const int ZONE_RETENTION_LOW_WATER = 36;

// The margin left in the town countdown after the native decrement. Large
// enough that `(frameDelta + margin) - frameDelta` stays positive in single
// precision at any plausible frame delta, and small enough that the cell
// expires on the next frame if the policy stops writing.
const float ZONE_RETENTION_HOLD_MARGIN = 0.05f;

// What to write into the town countdown so the native decrement leaves a
// positive remainder. The write is only ever made on a frame whose decrement
// is about to happen, so the remainder is the margin and nothing else: a
// hold never outlives the frame that wrote it.
float ZoneRetentionHoldValue(float frameDelta);

enum ZoneRetentionVerdict
{
	ZONE_RETENTION_PASS = 0,   // not our decision: call the original unchanged
	ZONE_RETENTION_HOLD,
	ZONE_RETENTION_RELEASE
};

// The first stage answers from cheap state alone. ASK_ANCHORS means every
// cheap reason to hold is exhausted and the caller should now read the live
// anchors — the one expensive input — and finish with the call below.
enum ZoneRetentionPrecheck
{
	ZONE_RETENTION_PRE_PASS = 0,
	ZONE_RETENTION_PRE_HOLD,
	ZONE_RETENTION_PRE_ASK_ANCHORS
};

// Only built for a cell whose three native countdowns all cross zero this
// frame: that test is ZoneRetentionNativeWouldExpireThisFrame, and a cell
// that fails it never reaches here.
struct ZoneRetentionCellInputs
{
	bool tracked;           // the ledger holds a live entry for this cell
	bool anchorsUnknown;    // the last retention rebuild could not read an anchor
	bool readerPinned;      // something is reading this cell
	bool minResidenceLive;  // the post-handover residence window has not run out
	bool discretionaryLive; // grace or a prediction lease still running
	bool mapRetained;       // the one-second proximity map says "near"
	bool underPressure;
	bool pacingAllows;
};

ZoneRetentionPrecheck ZoneRetentionPrecheckCell(const ZoneRetentionCellInputs& in);

// The second stage, with the live anchor answer in hand.
ZoneRetentionVerdict ZoneRetentionFinalVerdict(bool nearAnchorsNow);

// The radius the live check should use: pressure narrows it to the hard core,
// because the proximity map is stamped at the configured retain radius and
// says nothing about radius 1.
int ZoneRetentionLiveRadius(bool underPressure);

// At most one expensive decision per frame, never in a frame that admitted a
// cohort, and otherwise at least the spacing apart since the last release.
// Pressure drops the spacing only. `probedThisFrame` covers releases too: a
// release cannot happen without the expensive decision that precedes it.
bool ZoneRetentionPacingAllows(bool probedThisFrame, bool adoptedThisFrame,
                               double lastReleaseAt, double now, bool underPressure);

// The soft cap with its low-water exit: true while the policy should stop
// holding discretionary cells.
bool ZoneRetentionPressureNext(bool active, int heldCount, int softCap, int lowWater);

// How long a cell stands down after its navmesh fences refused, before the
// prologue asks for it again. The prologue sees every member of the active
// set on every frame, so a fence that keeps refusing would otherwise be asked
// once a frame for as long as the cell lives; the window doubles with the
// refusal streak and stops growing at the cap, which keeps a fence stuck for
// a minute to a handful of attempts rather than thousands.
const double ZONE_RETENTION_DEFER_BACKOFF_SEC     = 0.25;
const double ZONE_RETENTION_DEFER_BACKOFF_MAX_SEC = 4.0;
double ZoneRetentionDeferBackoffSeconds(int streak);

#endif // KENSHI_ZONE_OPT_ZONE_RETENTION_POLICY_H
