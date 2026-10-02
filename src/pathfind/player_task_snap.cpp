// player_task_snap.cpp - Player arrival, movement and DEV task diagnostics.
// Main thread. DEV reads are POD-only under SEH; no diagnostic lock spans a game call.

#include "game/klib_member_contract.h"
#include "pathfind/pathfind_diag.h"
#include "zone/grid.h"
#include "movement/islands.h"
#include "zone/preload/preload.h"   // PreloadZoneLeakOrphans (PLAYER STUCK orph=)
#include "pathfind/player_task_policy.h"
#include "movement/island_edge_legs.h"
#include "movement/order_outcome.h"
#include "movement/order_outcome_policy.h"   // OrderOutcomeStuckSuffix (PLAYER STUCK ko=/hc=/post=)
#include "zone/zone_pause.h"             // ZonePauseIsPaused (PLAYER STUCK print suppression)

// =========================================================================
// Player movement state poller
// =========================================================================
// Traces the movement execution side when pathfinding succeeds but
// the character won't move.  Calls CharMovement methods through vtable
// (virtual) and KenshiLib stubs (non-virtual).


// CharMovement vtable offsets (from CharMovement.h)
const size_t VT_GET_POSITION    = 0x40;
const size_t VT_PATH_OK         = 0x48;
const size_t VT_PATH_FAILED     = 0x50;
const size_t VT_IS_DEST_REACHED = 0x60;

typedef bool (__fastcall *BoolMethodFn)(void*);
typedef const float* (__fastcall *GetPosFn)(void*);

static double lastStuckPollTime = 0.0;


#ifdef KEO_DEBUG
// =========================================================================
// PLAYER TASK diagnostic (DEV only)
// =========================================================================
//
// Decides between the two ways the engine stops a moving player character:
//   - the move order is deleted: t 29 -> -1 with hc136=1 and edge 1 -> 0
//     (the isDestinationReached shortcut), or with ps=3 (path failure);
//   - a threat preempts it: t 29 -> 32 (SELF_PRESERVATION) or a combat
//     action with thr>0, near<182, lvl=3; t -> 62 with bbReq>0 (STAND_STILL).
// One line per tracked player character on every change of
// (t, stopped, edge, hc136), and alongside every PLAYER STUCK line.
// Key a deletion on t and goal/lvl (0x50CD40 zeroes ts+0x1C0 and ts+0x20C)
// and on dq= (the order deque 0x50CD40 pops), not on permajobs= (legacy ord=; the ts+0x88
// lektor 0x50DB20 scores, which the deletion does not pop).
//
// Every offset is a plain load (no game function is called), with the RVA
// that proves it. Task type decoding is shared with the native fixture.
// Task selection, order pops and task deletion run on the game's threaded
// update, not the main thread, so every pointer chase is inside the one
// guarded helper below. threats hands are NOT resolved (that needs
// hand::getObject); the count, near and tp are enough.

// The PT_OFF_* offsets read below live in player_task_policy.h (shared with
// islands.cpp), each with the RVA that proves it.

// POD snapshot filled by ReadPlayerTaskSnap. Integer fields use PT_NA for
// "unreadable" (printed "-"); -1 in t / goal / permajobHead means a null link
// (no current action, no goal, no permanent job).
namespace player_task_snap_detail {
struct PlayerTaskSnap
{
	int       fault;       // 1 = a read faulted: every other field prints "-"
	int       t;           // current action type (CharBody+0x68 -> +0x70 -> +0x44)
	int       goal;        // current goal type (stage3: ts+0x1C0 -> TaskData+0x44)
	int       lvl;         // ts+0x20C
	int       permajobN;   // ts+0x90
	int       permajobHead; // (*(ts+0x98))[0] -> +0x70 -> +0x44
	long long dq;          // ts+0x60 (order deque size, the one 0x50CD40 pops), -1 = unreadable
	int       fin;         // ts+0x26C
	int       impossible;  // ts+0x26D
	int       stopped;     // CharMovement+0x08
	int       moving;      // CharMovement+0x24
	int       edge;        // CharMovement+0x370
	int       ctr;         // CharMovement+0x368
	int       hc136;       // HavokCharacter+0x88
	int       ps;          // HavokCharacter+0x90
	int       reached;     // computed, mirrors CharMovement::isDestinationReached 0x65E320
	int       en;          // AI+0xBC
	int       thr;         // AI+0x80
	int       haveAiF;     // 1 when nearSq / tp were read
	float     nearSq;      // AI+0x28 (squared)
	float     tp;          // AI+0xB0
	int       hit;         // Character+0x2B0
	int       cst;         // CombatClass+0x1F0
	int       atk;         // CombatClass+0x200 (header-verified)
	long long bbReq;       // Blackboard+0x178, -1 = unreadable
};
} // namespace player_task_snap_detail
using namespace player_task_snap_detail;

// The one guarded helper. Plain C: POD only, no C++ object in scope (MSVC
// 2010 rejects __try in a function with objects needing unwinding), no game
// function call, no allocation, no lock. Each pointer is read once into a
// local and null-checked before it is followed. A fault (a Tasker, order
// array or goal record deleted on the AI thread mid-read) sets fault=1; the
// fault never reaches the crash recorder (GuardEnter/GuardLeave, core.h).
// `character` has already passed PollPlayerMovementState's squad-list test.
static void ReadPlayerTaskSnap(uintptr_t character, PlayerTaskSnap* out)
{
	out->fault   = 0;
	out->t       = PT_NA;  out->goal    = PT_NA;  out->lvl     = PT_NA;
	out->permajobN = PT_NA;  out->permajobHead = PT_NA;  out->fin     = PT_NA;
	out->impossible = PT_NA;  out->stopped = PT_NA;  out->moving  = PT_NA;
	out->edge    = PT_NA;  out->ctr     = PT_NA;  out->hc136   = PT_NA;
	out->ps      = PT_NA;  out->reached = PT_NA;  out->en      = PT_NA;
	out->thr     = PT_NA;  out->haveAiF = 0;      out->nearSq  = 0.0f;
	out->tp      = 0.0f;   out->hit     = PT_NA;  out->cst     = PT_NA;
	out->atk     = PT_NA;  out->bbReq   = -1;     out->dq      = -1;

	GuardEnter();
	__try
	{
		out->hit = *(unsigned char*)(KLIB_MEMBER(3, character, Character__isLiterallyUnderMeleeAttackRightNowForSure, PT_OFF_CHAR_HIT));

		uintptr_t body = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_body, PT_OFF_CHAR_BODY));
		if (body)
		{
			uintptr_t action = *(uintptr_t*)(KLIB_MEMBER(3, body, CharBody_currentAction, PT_OFF_BODY_ACTION));
			PT_TASKER_TYPE(action, out->t);
			uintptr_t cc = *(uintptr_t*)(KLIB_MEMBER(3, body, CharBody_combatClass, PT_OFF_BODY_COMBAT));
			if (cc)
			{
				out->cst = *(int*)(KLIB_MEMBER(3, cc, CombatClass_combatState, PT_OFF_CC_STATE));
				out->atk = *(int*)(KLIB_MEMBER(3, cc, CombatClass_attackersH_count, PT_OFF_CC_ATTACKERS));
			}
		}

		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, PT_OFF_CHAR_MOVEMENT));
		if (cm)
		{
			out->stopped = *(unsigned char*)(KLIB_MEMBER(3, cm, AbstractMovementBase_officiallyStopped, PT_OFF_CMOV_STOPPED));
			out->moving  = *(unsigned char*)(KLIB_MEMBER(3, cm, AbstractMovementBase_currentlyMoving, PT_OFF_CMOV_MOVING));
			out->edge    = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, PT_OFF_CMOV_EDGE));
			out->ctr     = *(int*)(KLIB_MEMBER(3, cm, CharMovement_edgeTarget, PT_OFF_CMOV_EDGE_CTR));
			uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, PT_OFF_CMOV_HC));
			if (hc)
			{
				out->hc136 = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, PT_OFF_HC_ARRIVAL));
				out->ps    = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, PT_OFF_HC_PATH_STATE));
				out->reached = (out->hc136 == 1 && !out->moving && !out->edge) ? 1 : 0;
			}
			else
			{
				out->reached = 1;   // 0x65E320 returns true when there is no HavokCharacter
			}
		}

		uintptr_t ai = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_ai, PT_OFF_CHAR_AI));
		if (ai)
		{
			out->en      = *(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_numEnemies, PT_OFF_AI_NUM_ENEMIES));
			out->thr     = *(int*)(KLIB_MEMBER(3, ai, AI_sensoryData_threats_count, PT_OFF_AI_THREATS));
			out->nearSq  = *(float*)(KLIB_MEMBER(3, ai, AI_sensoryData_nearestEnemy, PT_OFF_AI_NEAREST_SQ));
			out->tp      = *(float*)(KLIB_MEMBER(3, ai, AI_sensoryData_totalThreatLevelPersonal, PT_OFF_AI_THREAT_PERS));
			out->haveAiF = 1;

			uintptr_t ts = *(uintptr_t*)(KLIB_MEMBER(3, ai, AI_taskSystemAI, PT_OFF_AI_TASKSYS));
			if (ts)
			{
				uintptr_t goalRec = *(uintptr_t*)(KLIB_MEMBER(3, ts, OrdersReceiver_currentGoal_taskData, PT_OFF_TS_GOAL));
				PT_GOAL_TYPE(goalRec, out->goal);
				out->lvl     = *(int*)(KLIB_MEMBER(3, ts, OrdersReceiver_currentGoalPriority, PT_OFF_TS_GOAL_LEVEL));
				out->fin     = *(unsigned char*)(KLIB_MEMBER(3, ts, AITaskSytem__taskCompletedFlag, PT_OFF_TS_FINISHED));
				out->impossible = *(unsigned char*)(KLIB_MEMBER(3, ts, AITaskSytem__taskImpossibleFlag, PT_OFF_TS_IMPOSSIBLE));
				out->dq      = *(long long*)(KLIB_MEMBER(3, ts, OrdersReceiver_orders_list__Mysize, PT_OFF_TS_DEQUE_SIZE));
				int n = *(int*)(KLIB_MEMBER(3, ts, OrdersReceiver_permajobs_count, PT_OFF_TS_PERMAJOB_COUNT));
				if (n >= 0 && n < PT_PERMAJOB_COUNT_LIMIT)
				{
					out->permajobN = n;
					out->permajobHead = -1;
					if (n > 0)
					{
						uintptr_t arr = *(uintptr_t*)(KLIB_MEMBER(3, ts, OrdersReceiver_permajobs_stuff, PT_OFF_TS_PERMAJOB_ARRAY));
						if (arr)
						{
							uintptr_t head = *(uintptr_t*)arr;
							PT_TASKER_TYPE(head, out->permajobHead);
						}
					}
				}
			}

			uintptr_t platoon = *(uintptr_t*)(KLIB_MEMBER(3, ai, AI_platoon, PT_OFF_AI_PLATOON));
			if (platoon)
			{
				uintptr_t bb = *(uintptr_t*)(KLIB_MEMBER(3, platoon, Platoon_blackboard, PT_OFF_PLATOON_BB));
				if (bb)
					out->bbReq = *(long long*)(KLIB_MEMBER(3, bb, Blackboard_requests_size, PT_OFF_BB_REQ_SIZE));
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out->fault = 1;
	}
	GuardLeave();
}
#undef PT_TASKER_TYPE
#undef PT_TASKDATA_TYPE
#undef PT_GOAL_TYPE

// Per-character "last printed state" for the change detection: a small
// fixed array parallel to trackedPlayers[] (same index), keyed by the
// character pointer it was recorded for, so a slot handed to another
// character reads as "never printed". PollPlayerMovementState forgets an
// entry whenever it drops the tracked character (not in the player squad any
// more -- which is also what happens to every tracked character at a save
// load -- or arrived).
namespace player_task_snap_detail {
struct PlayerTaskLast
{
	uintptr_t character;   // 0 = nothing printed for this slot
	int       fault, t, stopped, edge, hc136;
};
} // namespace player_task_snap_detail
using namespace player_task_snap_detail;
static PlayerTaskLast g_playerTaskLast[MAX_TRACKED_PLAYERS];

static void PlayerTaskForget(int slot)
{
	if (slot >= 0 && slot < MAX_TRACKED_PLAYERS)
		g_playerTaskLast[slot].character = 0;
}

// true when (fault, t, stopped, edge, hc136) differs from the last line
// printed for this slot's character (or nothing was printed yet).
static bool PlayerTaskChanged(int slot, uintptr_t character, const PlayerTaskSnap& s)
{
	const PlayerTaskLast& l = g_playerTaskLast[slot];
	if (l.character != character) return true;
	return l.fault != s.fault || l.t != s.t || l.stopped != s.stopped
	    || l.edge != s.edge || l.hc136 != s.hc136;
}

static void PtAppendInt(std::ostringstream& ss, int v)
{
	if (v == PT_NA) ss << "-";
	else            ss << v;
}

// Formats and logs one PLAYER TASK line (main thread) and remembers it as
// this slot's last printed state.
static void LogPlayerTask(int slot, uintptr_t character, const PlayerTaskSnap& s)
{
	std::ostringstream ss;
	ss << "PLAYER TASK: char=@" << std::hex << (character & 0xFFFF) << std::dec;
	if (s.fault)
	{
		ss << " fault=1 t=- goal=-/-"
		   << " permajobs=-:- dq=- fin=- impossible=- stopped=- moving=-"
		   << " edge=-/- hc136=- ps=- reached=- en=- thr=- near=- tp=- hit=-"
		   << " cst=- atk=- bbReq=-";
	}
	else
	{
		ss << " t=";        PtAppendInt(ss, s.t);
		ss << " goal=";     PtAppendInt(ss, s.goal);
		ss << "/";          PtAppendInt(ss, s.lvl);
		ss << " permajobs=";
		PtAppendInt(ss, s.permajobN);
		ss << ":";          PtAppendInt(ss, s.permajobHead);
		ss << " dq=";
		if (s.dq < 0) ss << "-";
		else          ss << s.dq;
		ss << " fin=";      PtAppendInt(ss, s.fin);
		ss << " impossible=";
		PtAppendInt(ss, s.impossible);
		ss << " stopped=";  PtAppendInt(ss, s.stopped);
		ss << " moving=";   PtAppendInt(ss, s.moving);
		ss << " edge=";     PtAppendInt(ss, s.edge);
		ss << "/";          PtAppendInt(ss, s.ctr);
		ss << " hc136=";    PtAppendInt(ss, s.hc136);
		ss << " ps=";       PtAppendInt(ss, s.ps);
		ss << " reached=";  PtAppendInt(ss, s.reached);
		ss << " en=";       PtAppendInt(ss, s.en);
		ss << " thr=";      PtAppendInt(ss, s.thr);
		ss << std::fixed << std::setprecision(0);
		ss << " near=";
		// nearestEnemy is a running minimum of squared distances (0x858500);
		// with no enemy seen it holds a large reset value, printed as "inf".
		if (!s.haveAiF || !(s.nearSq >= 0.0f)) ss << "-";
		else if (s.nearSq > 1.0e12f)           ss << "inf";
		else                                   ss << sqrtf(s.nearSq);
		ss << std::setprecision(1);
		ss << " tp=";
		if (s.haveAiF) ss << s.tp;
		else           ss << "-";
		ss << " hit=";      PtAppendInt(ss, s.hit);
		ss << " cst=";      PtAppendInt(ss, s.cst);
		ss << " atk=";      PtAppendInt(ss, s.atk);
		ss << " bbReq=";
		if (s.bbReq < 0) ss << "-";
		else             ss << s.bbReq;
	}
	LogMsg(ss.str());

	PlayerTaskLast& l = g_playerTaskLast[slot];
	l.character = character;
	l.fault   = s.fault;
	l.t       = s.t;
	l.stopped = s.stopped;
	l.edge    = s.edge;
	l.hc136   = s.hc136;
}
#endif // KEO_DEBUG


void StorePlayerClickDest(uintptr_t character, const float* dest, double now)
{
	// Update existing entry or find empty slot
	int emptySlot = -1;
	for (int i = 0; i < pathfind::g_pathDiag.trackedPlayerCount; ++i)
	{
		if (pathfind::g_pathDiag.trackedPlayers[i].character == character)
		{
			// The match is by pointer whether the entry is active or not,
			// and PollPlayerMovementState deactivates an entry on arrival
			// (or squad removal). A re-click resets the entry exactly like
			// the add branch below (dest, prevPos, clickTime,
			// zeroVelocityPolls, active) plus, for a reactivated entry,
			// the PLAYER TASK last-printed state, so it behaves like a
			// new one.
#ifdef KEO_DEBUG
			if (!pathfind::g_pathDiag.trackedPlayers[i].active)
				PlayerTaskForget(i);
#endif
			pathfind::g_pathDiag.trackedPlayers[i].destX = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_x, 0));
			pathfind::g_pathDiag.trackedPlayers[i].destY = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_y, 4));
			pathfind::g_pathDiag.trackedPlayers[i].destZ = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_z, 8));
			pathfind::g_pathDiag.trackedPlayers[i].prevPosX = 0;
			pathfind::g_pathDiag.trackedPlayers[i].prevPosZ = 0;
			pathfind::g_pathDiag.trackedPlayers[i].clickTime = now;
			pathfind::g_pathDiag.trackedPlayers[i].zeroVelocityPolls = 0;
			pathfind::g_pathDiag.trackedPlayers[i].active = true;
			pathfind::g_pathDiag.trackedPlayers[i].arrivedPrev = false;
			pathfind::g_pathDiag.trackedPlayers[i].farArrivals = 0;
			pathfind::g_pathDiag.trackedPlayers[i].farArriveMaxD = 0.0f;
			pathfind::g_pathDiag.trackedPlayers[i].arrHaveLabels = false;
			pathfind::g_pathDiag.trackedPlayers[i].orderKoLatched = false;
			pathfind::g_pathDiag.trackedPlayers[i].havePrev = false;
			EdgeLegsForget(i);
			return;
		}
		if (!pathfind::g_pathDiag.trackedPlayers[i].active && emptySlot < 0)
			emptySlot = i;
	}

	// Add new entry
	int slot = emptySlot;
	if (slot < 0)
	{
		if (pathfind::g_pathDiag.trackedPlayerCount >= MAX_TRACKED_PLAYERS)
			return;
		slot = pathfind::g_pathDiag.trackedPlayerCount++;
	}

#ifdef KEO_DEBUG
	PlayerTaskForget(slot);   // a new entry has printed nothing yet
#endif
	pathfind::g_pathDiag.trackedPlayers[slot].character = character;
	pathfind::g_pathDiag.trackedPlayers[slot].destX = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_x, 0));
	pathfind::g_pathDiag.trackedPlayers[slot].destY = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_y, 4));
	pathfind::g_pathDiag.trackedPlayers[slot].destZ = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_z, 8));
	pathfind::g_pathDiag.trackedPlayers[slot].prevPosX = 0;
	pathfind::g_pathDiag.trackedPlayers[slot].prevPosZ = 0;
	pathfind::g_pathDiag.trackedPlayers[slot].clickTime = now;
	pathfind::g_pathDiag.trackedPlayers[slot].zeroVelocityPolls = 0;
	pathfind::g_pathDiag.trackedPlayers[slot].active = true;
	pathfind::g_pathDiag.trackedPlayers[slot].arrivedPrev = false;
	pathfind::g_pathDiag.trackedPlayers[slot].farArrivals = 0;
	pathfind::g_pathDiag.trackedPlayers[slot].farArriveMaxD = 0.0f;
	pathfind::g_pathDiag.trackedPlayers[slot].arrHaveLabels = false;
	pathfind::g_pathDiag.trackedPlayers[slot].orderKoLatched = false;
	pathfind::g_pathDiag.trackedPlayers[slot].havePrev = false;
	EdgeLegsForget(slot);
}


// A far arrival is one declared more than this far from the destination the
// order carried: past it, "arrived" cannot mean the character is at the goal.
static const float FAR_ARRIVE_DIST = 100.0f;

static long  g_farArrivals    = 0;
static float g_farArriveMaxD  = 0.0f;

long  PlayerFarArrivals()     { return g_farArrivals; }
float PlayerFarArriveMaxDist() { return g_farArriveMaxD; }

// Mirrors CharMovement::isDestinationReached 0x65E320: the arrival code with
// no movement and no edge leg outstanding. Plain loads, each pointer
// null-checked; nothing here calls a game function or allocates.
void SamplePlayerArrivals()
{
	if (pathfind::g_pathDiag.trackedPlayerCount == 0)
		return;

	// Live-list test before any dereference: a tracked entry is only cleared
	// by the 1 s stuck poll, so between two of its passes an entry can name a
	// character the game has already freed. This runs every frame, so it
	// tests membership itself rather than relying on the poll having run.
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return;
	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 256)
		return;

	for (int i = 0; i < pathfind::g_pathDiag.trackedPlayerCount; ++i)
	{
		TrackedPlayerDest& tp = pathfind::g_pathDiag.trackedPlayers[i];
		if (!tp.active || !tp.character)
			continue;

		bool live = false;
		for (unsigned int j = 0; j < scCount; ++j)
		{
			if (scStuff[j] == tp.character) { live = true; break; }
		}
		if (!live)
			continue;

		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, tp.character, Character_movement, OFF_CHAR_MOVEMENT));
		if (!cm)
			continue;
		uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
		if (!hc)
			continue;

		int hc136 = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL));
		bool moving = *(unsigned char*)(KLIB_MEMBER(3, cm, AbstractMovementBase_currentlyMoving, PT_OFF_CMOV_MOVING)) != 0;
		bool edge = *(unsigned char*)(KLIB_MEMBER(3, cm, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE)) != 0;
		bool arrived = (hc136 == 1 && !moving && !edge);
		float posX = *(float*)(KLIB_MEMBER(3, tp.character, RootObjectBase_pos_x, OFF_CHAR_POS_X));
		float posZ = *(float*)(KLIB_MEMBER(3, tp.character, RootObjectBase_pos_z, OFF_CHAR_POS_Z));
		EdgeLegsSample(i, cm, hc136, moving, edge, posX, posZ, tp.destX, tp.destZ, ElapsedSec());

		if (arrived && !tp.arrivedPrev)
		{
			float dx = tp.destX - posX;
			float dz = tp.destZ - posZ;
			float d2 = dx * dx + dz * dz;
			if (d2 > FAR_ARRIVE_DIST * FAR_ARRIVE_DIST)
			{
				float d = sqrtf(d2);
				tp.farArrivals++;
				if (d > tp.farArriveMaxD) tp.farArriveMaxD = d;
				g_farArrivals++;
				if (d > g_farArriveMaxD) g_farArriveMaxD = d;
				// Latch the routing branch's inputs at the first far arrival
				// of this order. Read here and not on the stuck poll because
				// the labels do not survive the intervening seconds.
				if (!tp.arrHaveLabels && g_cachedZoneMgr)
				{
					int sl, dl, sp;
					if (IslandSampleLabels(g_cachedZoneMgr, posX, posZ,
					                       tp.destX, tp.destZ, &sl, &dl, &sp))
					{
						tp.arrSelfLabel = sl;
						tp.arrDestLabel = dl;
						tp.arrSpan = sp;
						tp.arrHaveLabels = true;
					}
				}
			}
		}
		tp.arrivedPrev = arrived;
	}
}


namespace player_task_snap_detail
{

struct PollPlayerFrameCtx
{
	unsigned int scCount;
	uintptr_t* scStuff;
	double now;
	bool paused;
};

struct PollPlayerCharacterCtx
{
	uintptr_t charMov;
	float posX, posZ;
	bool moving, firstPoll;
	bool pathOk, destReached, destIsPos;
	struct { float x, y, z; } dest;
	int hc136Val;
	bool haveHc136;
	const char* k7Form;
#ifdef KEO_DEBUG
	PlayerTaskSnap taskSnap;
	bool taskPrinted;
#endif
};

static bool ReadPlayerCharacter(int i, const PollPlayerFrameCtx& frame, PollPlayerCharacterCtx& c)
{
	TrackedPlayerDest& tp = pathfind::g_pathDiag.trackedPlayers[i];
	if (!tp.active)
		return false;

	// Verify character still in squad
	bool found = false;
	for (unsigned int j = 0; j < frame.scCount; ++j)
	{
		if (frame.scStuff[j] == tp.character)
		{ found = true; break; }
	}
	if (!found)
	{
		tp.active = false;
#ifdef KEO_DEBUG
		PlayerTaskForget(i);
#endif
		return false;
	}

#ifdef KEO_DEBUG
	// PLAYER TASK (DEV): one guarded read per poll, printed on a change of
	// (t, stopped, edge, hc136) here, or next to the PLAYER STUCK line below.
	ReadPlayerTaskSnap(tp.character, &c.taskSnap);
	c.taskPrinted = false;
	if (PlayerTaskChanged(i, tp.character, c.taskSnap))
	{
		LogPlayerTask(i, tp.character, c.taskSnap);
		c.taskPrinted = true;
	}
#endif

	c.charMov = *(uintptr_t*)(KLIB_MEMBER(3, tp.character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!c.charMov)
		return false;

	uintptr_t vt = *(uintptr_t*)c.charMov;
	if (!vt)
		return false;

	// Read position
	const float* pos = KlibMovementPosition((void*)c.charMov);
	if (!pos)
		return false;

	c.posX = (*(const float*)KLIB_MEMBER(5, pos, Ogre__Vector3_x, 0));
	c.posZ = (*(const float*)KLIB_MEMBER(5, pos, Ogre__Vector3_z, 8));

	// Velocity check: compare with previous poll position
	float dx = c.posX - tp.prevPosX;
	float dz = c.posZ - tp.prevPosZ;
	c.moving = (dx * dx + dz * dz) > 1.0f;

	// The first poll after a click has no real previous position
	// (prevPosX/Z start at (0,0), StorePlayerClickDest), so the check
	// above reads a multi-thousand-unit "jump" and moving is always
	// true here. Report it as motionless to order_outcome.cpp instead,
	// so a real start delay (queued behind the path thread, or a
	// spread squad's gather wait) still has a chance to open before
	// order_outcome.cpp records this member as departed.
	c.firstPoll = !tp.havePrev;
	tp.havePrev = true;

	tp.prevPosX = c.posX;
	tp.prevPosZ = c.posZ;

	return true;
}

static bool UpdatePlayerMotion(int i, const PollPlayerFrameCtx& frame, PollPlayerCharacterCtx& c)
{
	TrackedPlayerDest& tp = pathfind::g_pathDiag.trackedPlayers[i];
	// Whether the order's own destination (tp.destX/Z, not the engine's
	// collapsed pathDestination below) is already within 100 units --
	// computed every poll, moving or not, so a character that passes
	// through the radius without ever stalling still has this order's
	// arrival recorded (it would otherwise never be checked: the whole
	// block below is gated on two stationary polls). Terminal: the
	// order-outcome record retires this member on it, and tracking here
	// ends outright, whatever the engine's own transient destReached flag
	// says this poll -- gating this on that flag instead left tracking
	// alive through it, so a later AI-job wander re-opened a "stall" that
	// was really a finished order.
	bool orderPost = OrderOutcomeIsPostArrival(tp.destX - c.posX, tp.destZ - c.posZ);

	// A stall's start and end come from this poll's timestamp, not from
	// the zero-velocity poll count below (which only reflects whether
	// the diagnostic line has started printing yet). moving && !firstPoll:
	// PLAYER STUCK's own zeroVelocityPolls bookkeeping below still uses
	// the plain `moving` -- only the order-outcome report suppresses the
	// click poll's spurious jump.
	OrderOutcomeNoteMotion(tp.character, c.moving && !c.firstPoll, orderPost, frame.now);

	if (orderPost)
	{
		tp.active = false;
#ifdef KEO_DEBUG
		PlayerTaskForget(i);
#endif
		return false;
	}

	// Nothing simulates while paused: OrderOutcomeNoteMotion above still
	// ran (so the order-outcome clock stays fed and a stall opened before the pause
	// still resolves correctly on the first unpaused poll), but no stuck
	// count, class guess, KO note or PLAYER STUCK line comes from a
	// paused poll.
	if (frame.paused) return false;

	if (c.moving)
	{
		tp.zeroVelocityPolls = 0;
		return false;
	}

	tp.zeroVelocityPolls++;

	// Need two consecutive zero-velocity polls (~2s) before checking
	if (tp.zeroVelocityPolls < 2)
		return false;

	return true;
}

static void ClassifyPlayerStall(int i, const PollPlayerFrameCtx& frame, PollPlayerCharacterCtx& c)
{
	TrackedPlayerDest& tp = pathfind::g_pathDiag.trackedPlayers[i];
	// Read movement state
	c.pathOk = KlibMovementPathOk((void*)c.charMov);
	c.destReached = KlibMovementDestinationReached((void*)c.charMov);

	float coordinates[3];
	KlibMovementDestination((void*)c.charMov, coordinates);
	c.dest.x = coordinates[0];
	c.dest.y = coordinates[1];
	c.dest.z = coordinates[2];

	// Deadlock check: dest collapsed to pos, pathOk (trivially), not arrived
	float ddx = c.dest.x - c.posX;
	float ddz = c.dest.z - c.posZ;
	c.destIsPos = (ddx * ddx + ddz * ddz) < 100.0f;  // < 10 units

	// ko= is latched (tp.orderKoLatched), because a KO'd member that
	// wakes and stands again must not print ko=0 on what is really a
	// wake-up stall, not a fresh routing failure. post= needs no latch
	// here: reaching it above already ended tracking for this poll.
	bool orderKo = IslandK7IsUnconcious(tp.character);
	if (orderKo)
	{
		tp.orderKoLatched = true;
		OrderOutcomeNoteKo(tp.character, frame.now);
	}
	c.hc136Val = 0;
	c.haveHc136 = IslandReadHc136(tp.character, &c.hc136Val);
	c.k7Form = IslandK7StuckForm(tp.character);
	// The stops= class guess never latches on an excluded stall (ko or
	// post): it must survive to describe the stall that is actually
	// counted, not whichever one happened to be open when a knockout or
	// an arrival passed through.
	if (!tp.orderKoLatched)
	{
		std::ostringstream guess;
		guess << "k7=" << c.k7Form << "/" << IslandK7StopSig(tp.character) << "/span";
		if (tp.arrHaveLabels) guess << tp.arrSpan; else guess << "?";
		OrderOutcomeNoteStopGuess(tp.character, guess.str().c_str(), frame.now);
	}
}

static void ReportPlayerStuck(int i, const PollPlayerCharacterCtx& c)
{
	TrackedPlayerDest& tp = pathfind::g_pathDiag.trackedPlayers[i];
	// PLAYER STUCK diagnostic line (no recovery: the retry was deleted).
	// UpdatePlayerMotion's paused return already keeps this whole block
	// (and the stuck count/guess/KO note before it) from running on a
	// paused poll, so this phase needs no separate paused check of its own.
	{
		bool pathFailed = KlibMovementPathFailed((void*)c.charMov);
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(0);
		ss << "PLAYER STUCK: "
		   << tp.zeroVelocityPolls << " polls"
		   << " pos=(" << c.posX << "," << c.posZ << ")"
		   << " dest=(" << c.dest.x << "," << c.dest.z << ")"
		   << " pathOk=" << (c.pathOk ? 1 : 0)
		   << " pathFail=" << (pathFailed ? 1 : 0)
		   << " destReach=" << (c.destReached ? 1 : 0)
		   << " destIsPos=" << (c.destIsPos ? 1 : 0)
		// How far short of the order's own destination the character
		// stopped, and the far arrivals latched for it since the order
		// was given. destIsPos= cannot carry this: CharMovement::halt
		// copies the position into the destination as the order ends, so
		// it reads 1 whether the order finished at the goal or nowhere
		// near it.
		   << " dDest=" << sqrtf((tp.destX - c.posX) * (tp.destX - c.posX)
		                       + (tp.destZ - c.posZ) * (tp.destZ - c.posZ))
		   << " farArr=" << tp.farArrivals << "/" << tp.farArriveMaxD;
		// Island routing view of the park (see islands.h):
		//   wp   = pathDestination, edge = movingToEdge/edgeCounter
		//   self = liveComp(char zone), xd = |pos - raw emulated crossing|
		//   next = zone beyond the crossing: c=liveComp l=label ld=+176 a=+177
		IslandStuckInfo isi;
		if (g_cachedZoneMgr
		    && IslandDescribeStuck(g_cachedZoneMgr, c.charMov, c.posX, c.posZ,
		                           tp.destX, tp.destZ, &isi))
		{
			ss << " wp=(" << isi.wpX << "," << isi.wpZ << ")"
			   << " edge=" << isi.movingToEdge << "/" << isi.edgeCounter
			   << " self=" << isi.selfComp;
			if (isi.haveCrossing)
				ss << " xd=" << isi.xd;
			else
				ss << " xd=none";
			ss << " next=(" << isi.nextGX << "," << isi.nextGY << ")"
			   << " c=" << isi.nextComp
			   << " l=" << isi.nextLabel
			   << " ld=" << isi.nextLoading
			   << " a=" << isi.nextAccess;
			// The engine's same-island inputs for this order: the island
			// label of the character's own cell and of the cell holding
			// the destination, the cell span between them, and the answer
			// those two labels give. sameIsl=1 on a multi-cell span means
			// the route was sent as one direct path instead of an edge
			// route. A label that could not be read prints ? rather than
			// 0, because 0 is itself a meaningful label value.
			ss << " isl=";
			if (isi.haveSelf) ss << isi.selfLabel; else ss << "?";
			ss << "/";
			if (isi.haveDest) ss << isi.destLabel; else ss << "?";
			ss << " dcell=(";
			if (isi.haveDest) ss << isi.destGX << "," << isi.destGY; else ss << "?,?";
			ss << ") span=";
			if (isi.cellSpan >= 0) ss << isi.cellSpan; else ss << "?";
			ss << " sameIsl=";
			if (isi.sameIsland >= 0) ss << isi.sameIsland; else ss << "?";
			// The same pair as the engine's routing branch saw it, latched
			// at this order's first far arrival. isl= above is sampled now,
			// up to tens of seconds later, and an island recalculation or a
			// cell deactivation in between rewrites both labels -- so only
			// this field decides which branch was taken. Absent latch reads
			// ?, never a label value.
			ss << " isl@arr=";
			if (tp.arrHaveLabels)
				ss << tp.arrSelfLabel << "/" << tp.arrDestLabel
				   << " span@arr=" << tp.arrSpan
				   << " sameIsl@arr=" << ((tp.arrSelfLabel == tp.arrDestLabel) ? 1 : 0);
			else
				ss << "?/? span@arr=? sameIsl@arr=?";
		}
		// The far-span rule's view of this order: whether it is one the
		// rule moves (or, off, would move) and what its edge legs did.
		EdgeLegsAppendStuck(i, ss);
		// The K7 tracker's view of this character (islands.h
		// IslandK7StuckForm, checked in this order: off / - / held / del /
		// arr / pre / trk / new).
		ss << " k7=" << c.k7Form;
		// Mod-loaded zones still loaded but untracked ("-" until
		// PreloadZoneLeakOrphans measures it).
		int orphans = PreloadZoneLeakOrphans();
		ss << " orph=";
		if (orphans < 0) ss << "-";
		else             ss << orphans;
		// Whether this line is a knockout/death (ko=, latched -- see
		// above), the arrival flag (hc=, HavokCharacter::characterState),
		// and the order's own destination (post=, always 0 here: reaching
		// it ends tracking earlier in this same poll, above) -- appended
		// last so existing prefix readers still match.
		ss << OrderOutcomeStuckSuffix(tp.orderKoLatched, c.haveHc136, c.hc136Val, false);
		LogMsg(ss.str());
	}
#ifdef KEO_DEBUG
	// Every PLAYER STUCK line gets a PLAYER TASK line (unless this poll's
	// change already printed one, just above it).
	if (!c.taskPrinted)
		LogPlayerTask(i, tp.character, c.taskSnap);
#endif
}

static void PollPlayerCharacter(int i, const PollPlayerFrameCtx& frame)
{
	PollPlayerCharacterCtx c;
	if (!ReadPlayerCharacter(i, frame, c))
		return;
	if (!UpdatePlayerMotion(i, frame, c))
		return;
	ClassifyPlayerStall(i, frame, c);
	ReportPlayerStuck(i, c);
}

} // namespace player_task_snap_detail
using namespace player_task_snap_detail;

void PollPlayerMovementState(double now)
{
	if (now - lastStuckPollTime < 1.0)
		return;
	lastStuckPollTime = now;

	if (pathfind::g_pathDiag.trackedPlayerCount == 0)
		return;

	// Validate tracked characters still exist in player squad
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return;

	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 256)
		return;

	// A paused game (the escape menu, kept open through a loader unpause) must
	// never produce a PLAYER STUCK line, a stuck count or a class guess: the
	// character has not stopped, the clock measuring it has.
	// OrderOutcomeNoteMotion runs on a paused poll too (UpdatePlayerMotion, after
	// orderPost), so the order-outcome clock stays fed and a stall opened before the
	// pause resolves on the first unpaused poll; UpdatePlayerMotion then returns
	// false, so PollPlayerCharacter stops before ClassifyPlayerStall and ReportPlayerStuck.
	bool paused = ZonePauseIsPaused();

	PollPlayerFrameCtx frame;
	frame.scCount = scCount;
	frame.scStuff = scStuff;
	frame.now = now;
	frame.paused = paused;
	for (int i = 0; i < pathfind::g_pathDiag.trackedPlayerCount; ++i)
		PollPlayerCharacter(i, frame);
}
