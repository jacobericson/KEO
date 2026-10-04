// plan_policy.h - Every decision the route planner makes, pure: the verdict, the leg-target rule,
// the leg state machine at getZoneEdge, the rung slide, the flip and its order against the far-span
// rule, the ownership of a wait, the re-plan triggers and the preload feed's cells. No Windows,
// KenshiLib or game header; any thread.
#ifndef KEO_PLANNER_PLAN_POLICY_H
#define KEO_PLANNER_PLAN_POLICY_H

namespace planner {

// The route planner's mode (plannerMode): off plans nothing; observe counts what on would do; on steers.
enum PlannerMode { PLANNER_OFF = 0, PLANNER_OBSERVE, PLANNER_ON };

const int   PLAN_MAX_LEGS     = 32;      // leg slots: up to PLAN_MAX_PORTAL_LEGS portals, then the destination
const int   PLAN_MAX_PORTAL_LEGS = 31;
const float PLAN_REACH        = 20.0f;   // the parked test's reach; arrival at a portal
const float PLAN_DEST_MATCH   = 2.0f;    // a call carries the plan's destination within this
// A gather sends every member to one point and each walks to its own slot around it. The engine's
// slot spread is not read: the formation's arrival-scatter radius stands in as a proxy for it,
// sqrt(members * 60) / 2 + 5, 26.2 at the 30-member cap, plus the 2-unit match and a margin.
const float PLAN_HOLD_MATCH   = 30.0f;   // a call carries the gather point within this
const float PLAN_RUNG_INSET   = 5.0f;    // a slid portal stays this far inside its edge
const float PLAN_POST_ARRIVAL = 100.0f;  // within this of the destination the order is done
const float PLAN_RESEND_REACH = 10.0f;   // a re-send of the destination lies within this of it (x-z)
const int   PLAN_RESEND_POINTS = 2;      // the mod's distinct re-sent points a plan keeps

enum PlanVerdict { PV_NONE = 0, PV_DIRECT, PV_LEGGED, PV_NO_ROUTE };

struct PlanLeg
{
	float point[3];        // the portal's midpoint, or the destination for the final leg; world
	float edgeA[3];        // the portal's border edge; both equal point for the destination leg
	float edgeB[3];
	int   cellX, cellY;    // point's cell
	int   farSection;      // store directory index of the section across the portal (the goal's
	                       //   section for the final leg)
	int   isDestination;
};

// The cell of a world point: floor((x + 147456) / 4608), floor((z + 147456) / 4608).
void PlanCellOf(float x, float z, int* cx, int* cy);
int  PlanCellSpan(int ax, int ay, int bx, int by);   // Chebyshev

// DIRECT: a route, every leg's far section loaded (loadedMask bit i for leg i) and span < legSpan.
// LEGGED: a route otherwise. NO_ROUTE: none.
PlanVerdict PlanDecideVerdict(bool routeFound, int legCount, unsigned loadedMask, int span, int legSpan);

// The leg-target rule, from leg `from` on: the last leg whose far section is loaded and whose cell
// is within legSpan - 1 of (cx, cy); when leg `from` itself is unloaded, `from`. The destination leg
// qualifies only loaded and within the bound. -1 when from >= n.
int PlanLegTarget(const PlanLeg* legs, int n, unsigned loadedMask, int from, int cx, int cy, int legSpan);

// The leg state machine at getZoneEdge's two sites.
enum PlanEdgeSite   { PES_OTHER = 0, PES_RECHECK, PES_COMPUTE };
enum PlanEdgeAction { PEA_PASS = 0, PEA_POINT };
struct PlanEdgeIn
{
	int      site;         // PlanEdgeSite, from the return address
	float    offset;       // the call's lateral offset
	float    pos[3];       // the character's position
	int      legIndex;     // the slot's current leg
	unsigned loadedMask;
	int      legSpan;
	int      routeTruncated;   // the plan kept its first PLAN_MAX_PORTAL_LEGS portals of a longer route
};
struct PlanEdgeOut
{
	int   action;          // PlanEdgeAction
	int   newLegIndex;     // in.legIndex unless the recheck advanced
	int   waiting;         // 1: within reach of the current portal and no later leg is a target yet
	int   rung;            // 1: this call is a rung (COMPUTE with offset != 0)
	float point[3];        // the waypoint to return, before the snap
};
// PES_OTHER, or a legIndex out of range: PASS. RECHECK: within PLAN_REACH (x-z distance) of the
// current leg's point, and only once the current leg's far section is loaded (loadedMask bit
// legIndex; until then the current point with waiting = 1), the target from legIndex + 1 by
// PlanLegTarget (from the character's cell); a target past legIndex advances, else the current
// point with waiting = 1; outside the reach, the current point. A truncated plan never advances
// onto its destination leg: at its last portal the recheck holds with waiting = 1 until the
// re-plan. COMPUTE with offset 0: the current point.
// COMPUTE with offset != 0: the current point slid by offset (PlanRungSlide), rung = 1. A
// destination leg is never slid.
void PlanEdgeStep(const PlanLeg* legs, int n, const PlanEdgeIn& in, PlanEdgeOut* out);

// The point moved along its edge (edgeA -> edgeB, x-z direction) by offset, clamped to
// [PLAN_RUNG_INSET, length - PLAN_RUNG_INSET] along the edge (the midpoint when shorter than twice
// the inset). y interpolated along the edge.
void PlanRungSlide(const PlanLeg& leg, float offset, float out[3]);

// The flip, from the thread-local the entry detour published.
enum PlanFlipAnswer { PFA_NOT_MINE = 0, PFA_FALSE, PFA_VANILLA };
struct PlanFlipIn
{
	int   mode;            // PlannerMode
	int   haveChar;        // the thread-local names a character
	int   haveSlot;        // it has a readable plan slot
	int   verdict;         // PlanVerdict
	int   legIsDestination;
	float dest[3];         // the call's destination, copied at entry
	float finalDest[3];    // the plan's
	float resend[PLAN_RESEND_POINTS][3]; int resendCount;   // the mod's recorded re-sends of finalDest
};
// NOT_MINE: off, no character, no slot, NO_ROUTE or NONE, or dest not the plan's (PlanDestIsPlans).
// Otherwise, in on: FALSE for LEGGED with the current leg not the destination, else
// VANILLA (the planner's answer, which the far-span rule does not modify). In observe: NOT_MINE
// always (the caller counts what on would have answered).
PlanFlipAnswer PlanFlipRule(const PlanFlipIn& in);
// What on would answer, whatever in.mode says; observe counts it.
PlanFlipAnswer PlanFlipRuleOn(const PlanFlipIn& in);

// |a - b| <= PLAN_DEST_MATCH in three dimensions: a call carries the plan's destination.
bool PlanDestMatches(const float a[3], const float b[3]);
// Whether a call's destination is the plan's: within PLAN_DEST_MATCH (three dimensions) of the plan's
// destination or of one of the first resendCount recorded re-sends (at most PLAN_RESEND_POINTS).
bool PlanDestIsPlans(const float dest[3], const float finalDest[3], const float resend[][3], int resendCount);
// The same test in x-z only, for a movement destination read without its height: within
// PLAN_DEST_MATCH of the plan's destination or of one of the first resendCount recorded re-sends.
bool PlanDestIsPlansXz(const float dest[3], const float finalDest[3], const float resend[][3], int resendCount);
// Whether the getZoneEdge detour returns the planner's point: on, a LEGGED plan and a matching
// destination; observe and off never steer.
bool PlanEdgeSteers(int mode, int verdict, bool destMatches);
// Whether the straight-line ahead enqueue is skipped for a character: on and a LEGGED plan; observe
// and off keep it.
bool PlanReplacesAhead(int mode, int verdict);

// Whether the planner owns a character's stop at its current portal (PlannerOwnsWait's pure half):
// on, LEGGED, the current leg not the destination, and either the slot's waiting word set within
// PLAN_REACH of the portal (a held wait), or the character within PLAN_REACH of its waypoint while
// that waypoint lies within PLAN_REACH of the portal (it stands at the portal the planner gave,
// before the engine's next advance). Distances x-z. Only while the movement destination is the
// plan's (destIsPlans): a halt or a detour's hold point is never owned.
bool PlanOwnsWait(int mode, int verdict, int legIsDestination, int waiting, float distToPortal,
                  float wpToPortal, float posToWp, bool destIsPlans);

const int PLAN_PATH_COMPLETE     = 1;   // HavokCharacter::PathState COMPLETE
const int PLAN_CHAR_GOAL_REACHED = 1;   // HavokCharacter::CharacterState GOAL_REACHED (IDLE is 0)
// A portal leg is complete when its path search finished and the character has stopped at the path's
// end (IDLE or GOAL_REACHED, the engine's own advance test) farther than PLAN_REACH from the portal in
// x-z. A path stays COMPLETE for the whole walk along it.
bool PlanLegComplete(int portalLeg, float distToPortal, int pathState, int characterState);

float PlanSnapDistance(const float raw[3], const float snapped[3]);   // x-z

// The re-plan triggers (main thread, per slot per tick).
enum PlanReplanWhy { PRW_NONE = 0, PRW_WAIT, PRW_ARRIVAL_TIMEOUT, PRW_RUNGS, PRW_AGE, PRW_GOAL_LOADED, PRW_ROUTE_END, PRW_COUNT };
struct PlanReplanIn
{
	double now, planTime, waitSince, completeSince;   // 0: not waiting / not complete
	int    waitSeconds;          // plannerWaitSeconds
	int    loadedChangedSincePlan;
	int    awaitedLoaded;        // the current leg's far section is loaded (loadedMask bit legIndex): the bit the recheck gates on
	int    rungs;
	int    goalByFootprint, goalLoaded;
	int    legIndex, legCount, routeTruncated;
	float  distToPortal;
};
// In this order: RUNGS (3 or more); ARRIVAL_TIMEOUT (complete for 8 s, never within PLAN_REACH);
// ROUTE_END (a truncated route whose current leg is its last portal, legIndex == legCount - 2, with
// the character within PLAN_REACH of it); WAIT (waiting for waitSeconds, the awaited section still
// unloaded, the loaded set changed since the plan); GOAL_LOADED (located by footprint, the goal's
// section now loaded); AGE (120 s); else NONE.
PlanReplanWhy PlanReplanDue(const PlanReplanIn& in);

// The preload feed: up to `ahead` distinct cells of legs from + 1 on, in route order, skipping
// interior sections (farSection >= exteriorSlots) and the cell of leg `from`. Writes x,y pairs.
int PlanFeedCells(const PlanLeg* legs, int n, int from, int ahead, int exteriorSlots, int* outXY);

// The install step's decision: off stays off; observe arms; on arms only with the hierarchical
// player search on (a multi-cell leg under the straight-line heuristic caps).
enum PlanArm { PLAN_ARM_OFF = 0, PLAN_ARM_GO, PLAN_ARM_REFUSE_PREREQ };
PlanArm PlanArmDecide(int mode, bool playerHierarchicalOn);

// The water cost. A coarse arc's cost rises with the water under its two nodes by a per-character
// multiplier m >= 1: OFF plans as if water were land; FLOOR (the default) takes the larger of the
// land-to-water speed ratio and the engine's own water value; DYNAMIC the ratio alone; ENGINE the
// engine value alone.
enum PlanWaterMode { PWC_OFF = 0, PWC_FLOOR, PWC_DYNAMIC, PWC_ENGINE };
const float PLAN_WATER_CAP   = 20.0f;   // the largest multiplier a plan uses
const float PLAN_JOG_SPEED   = 55.0f;   // the engine's JOG desired speed
const float PLAN_BOTTOM_WALK = 2.0f;    // a race that does not swim crosses water at this times its walk speed
struct PlanWaterInputs
{
	int   mode;            // PlanWaterMode
	int   readOk;          // 1: the speed fields were read; 0: only engineValue is known
	float landSpeed;       // L: the character's land speed while dry
	float waterSpeed;      // W: its swim speed, for a race that swims
	float raceWalkSpeed;   // its race's walk speed
	int   swims;           // its race swims
	float speedCap;        // the order's speed bound: the walk speed (WALK), PLAN_JOG_SPEED (JOG), 0 none
	float engineValue;     // the engine's water multiplier for the character; 1 when it has none
};
// L / W', where W' is waterSpeed for a race that swims and PLAN_BOTTOM_WALK * raceWalkSpeed for one
// that does not, both bounded by speedCap when it is positive; 0 (unknown) when !readOk or either
// speed as read is not finite and positive.
float PlanWaterRatio(const PlanWaterInputs& in);
// One character's m. With e the engine value (1 unless finite and positive) and r the ratio: OFF 1;
// ENGINE max(1, e); FLOOR max(1, r, e); DYNAMIC max(1, r); FLOOR and DYNAMIC with r unknown max(1, e);
// an unknown mode reads as FLOOR. Every result is at most PLAN_WATER_CAP.
float PlanWaterMultiplier(const PlanWaterInputs& in);
// A run-together order's one m: the mode formula over the slowest land speed and the slowest W' of
// the members whose read succeeded (no speed bound), and the largest engine value of all n members.
float PlanWaterGroupMultiplier(const PlanWaterInputs* members, int n);
// An arc's weighted cost: cost * (1 + (m - 1) * (wFrom + wTo) / 510), the mean water byte of its two
// nodes as a share; cost itself when m <= 1.
float PlanWaterArcCost(float cost, float m, int wFrom, int wTo);
// A route's water share: each step's 3D length weighted by its two nodes' mean water byte, over the
// route's length; 0 for fewer than two nodes or a zero length.
float PlanRouteWaterShare(const float (*centres)[3], const int* water, int n);

enum PlanWaterEngine { PWE_OFF = 0, PWE_MATCH };
// The float after 1.0. The engine installs no water modifier for a request at exactly 1.0, and the
// modifier also carries the FaceData-4 step cost, so a request is never written at 1.0.
const float PLAN_WATER_REQ_MIN = 1.00000012f;
// The value a player path request's water field takes from the character's multiplier m: 0 to leave
// the engine's own value (mode off, no multiplier known, or m already the engine's), otherwise m
// capped at PLAN_WATER_CAP and never below PLAN_WATER_REQ_MIN.
float PlanWaterRequestValue(int mode, float m, float engineValue);
// The water mode the planner arms with. Dynamic prices routes the engine's leg searches follow only
// while the request write runs, so without it dynamic arms as floor; every other mode as given.
int PlanWaterEffectiveMode(int configured, int engineLive);

} // namespace planner

#endif
