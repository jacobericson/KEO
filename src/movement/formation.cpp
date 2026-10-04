// formation.cpp - Group state, creation, deactivation and main-thread polling.
// Called on the main thread, plus one clear at hook install.

#include "movement/formation.h"
#include "movement/formation_members.h"
#include "pathfind/pathfinding.h"
#include "movement/formation_internal.h"
#include "movement/order_outcome.h"
#include "movement/order_outcome_table.h"
#include "planner/plan_store.h"
#include "planner/planner_merge.h"
#include "movement/formation_follow.h"
#include "movement/formation_pace.h"
#include "movement/formation_gather_policy.h"
#include "movement/islands.h"
#include "zone/zone_pause.h"

FormationGroup formationGroups[MAX_FORMATION_GROUPS];

bool scatterPatchApplied = false;

namespace formation_detail {

// A group slot's gather clocks, kept beside formationGroups: the gather timeout and the active-clock
// time it ages from, whether the route planner merged the order, and each member's last gather send
// (wall clock, the clock the tracker's re-issue cooldown reads). Main thread.
struct GatherClock
{
	double timeout;
	double startActive;
	bool   merged;
	double sendTime[MAX_FORMATION_MEMBERS_LIMIT];
};

} // namespace formation_detail
using namespace formation_detail;

static GatherClock s_gather[MAX_FORMATION_GROUPS];

// =========================================================================
// Formation group creation
// =========================================================================

static unsigned int nextFormationGroupId = 0;

// A member that picks up Hold or inSomething after the squad order was given
// must never receive the group's move order: Hold is not hooked, so the send
// sites are the only place left to catch it, and inSomething catches the
// sleeper before the game clears its own order. A member already in one of
// these states when the group formed (CreateFormationGroup's
// holdAtCreation/inSomethingAtCreation) travels with the group as before;
// only the on-after-off transition detaches it (FormationMemberNewlyHeld).
static bool CharacterIsHolding(uintptr_t character)
{
	uintptr_t stats = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_stats, OFF_CHAR_STATS));
	return stats && *(unsigned char*)(KLIB_MEMBER(3, stats, CharStats__holdPositionMode, OFF_STATS_HOLD)) != 0;
}

static bool CharacterIsInSomething(uintptr_t character)
{
	return *(int*)(KLIB_MEMBER(3, character, Character_inSomething, OFF_CHAR_IN_SOMETHING)) != 0;
}

namespace formation_detail {

bool CharacterNewlyHeld(uintptr_t character, bool holdAtCreation, bool inSomethingAtCreation)
{
	return FormationMemberNewlyHeld(holdAtCreation, CharacterIsHolding(character),
	                                 inSomethingAtCreation, CharacterIsInSomething(character));
}

} // namespace formation_detail
using namespace formation_detail;

void ClearFormationGroups()
{
	for (int i = 0; i < MAX_FORMATION_GROUPS; ++i)
	{
		formationGroups[i].active = false;
		formationGroups[i].gathered = false;
		formationGroups[i].count = 0;
		formationGroups[i].lastReissueTime = 0.0;
	}
	memset(s_gather, 0, sizeof(s_gather));
	FormationFollowOnClear();
	FormationPaceClear();
}

// A run-together order the route planner merged gathers at its gather point, and a member the merge
// left alone (alone[i] for chars[i]) walks its own route to the destination; *gatherDist receives the
// merge's farthest route distance to that point. When the planner did not merge this order nothing is
// written and every member gathers at the leader. Returns whether it merged.
static bool ReadPlannerMerge(FormationGroup& grp, const uintptr_t* chars, int charCount, unsigned char* alone,
                             float* gatherDist)
{
	float gather[3];
	int n = charCount < MAX_FORMATION_MEMBERS ? charCount : MAX_FORMATION_MEMBERS;
	if (!planner::PlannerMergeGather(chars, n, gather, alone, gatherDist))
		return false;
	grp.startX = gather[0];
	grp.startY = gather[1];
	grp.startZ = gather[2];
	return true;
}

// The group's speed in game units per second: the slowest Havok desired speed over the members that
// gather (CharMovement::update writes a grouped character's every frame, standing or not; Havok speeds
// are a tenth of a game unit); 0 when none reads positive. Game speed 1, so a faster game only shortens
// the walk against the timeout.
static float GatherSpeedOf(uintptr_t cm, float slowest)
{
	uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
	if (!hc)
		return slowest;
	float v = 10.0f * *(float*)(KLIB_MEMBER(3, hc, HavokCharacter_desiredSpeed, OFF_HC_DESIRED_SPEED));
	if (!(v > 0.0f))
		return slowest;
	return (slowest > 0.0f && slowest < v) ? slowest : v;
}

void CreateFormationGroup(const float* dest, uintptr_t* chars, int charCount)
{
	double now = ElapsedSec();

	// Debounce: if a recent group (< 500ms) has exactly this order's member
	// set (order-independent), update its destination in-place instead of
	// detach+recreate. Handles drag/shift-click paths where the game fires
	// many rapid addOrderSelectedCharacters calls for the same selection —
	// only the final destination matters for arrival scatter. A *different*
	// set (a subset or a set sharing only some members) falls through to the
	// normal detach-overlap-then-create path below, so a rapid subset order
	// doesn't drag the whole squad's destination along with it.
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		if (!formationGroups[g].active)
			continue;
		if (now - formationGroups[g].createdTime > 0.5)
			continue;

		uintptr_t existingChars[MAX_FORMATION_MEMBERS_LIMIT];
		for (int m = 0; m < formationGroups[g].count; ++m)
			existingChars[m] = formationGroups[g].members[m].character;

		if (FormationSameMemberSet(existingChars, formationGroups[g].count, chars, charCount))
		{
			formationGroups[g].destX = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_x, 0));
			formationGroups[g].destY = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_y, 4));
			formationGroups[g].destZ = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_z, 8));
			formationGroups[g].createdTime = now;
			for (int m = 0; m < formationGroups[g].count; ++m)
				formationGroups[g].members[m].dispatched = false;
			return;
		}
	}

	// Detach the overlapping members from any other active group instead of
	// killing it outright: the remainder keeps gathering/traveling to its
	// own destination and only retires once nothing is left
	// (FormationDetachCharacters, via DeactivateFormationGroup). Snapshot
	// each group's live count first so the log below reports only members
	// this call actually moved, not ones already zeroed by an earlier event.
	{
		int liveBefore[MAX_FORMATION_GROUPS];
		for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
		{
			liveBefore[g] = 0;
			if (!formationGroups[g].active) continue;
			for (int m = 0; m < formationGroups[g].count; ++m)
				if (formationGroups[g].members[m].character) ++liveBefore[g];
		}

		unsigned int newGroupId = nextFormationGroupId; // assigned to this call's group below

		FormationDetachCharacters(chars, charCount);

		for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
		{
			if (liveBefore[g] == 0) continue;
			int liveAfter = 0;
			for (int m = 0; m < formationGroups[g].count; ++m)
				if (formationGroups[g].members[m].character) ++liveAfter;
			int moved = liveBefore[g] - liveAfter;
			if (moved > 0)
			{
				std::ostringstream ss;
				ss << "Formation group " << formationGroups[g].groupId << ": " << moved
				   << " members moved to group " << newGroupId << " (" << liveAfter << " remain)";
				LogMsg(ss.str());
			}
		}
	}

	// Find free slot or evict oldest
	int slot = -1;
	double oldestTime = 1e20;
	int oldestSlot = 0;
	for (int i = 0; i < MAX_FORMATION_GROUPS; ++i)
	{
		if (!formationGroups[i].active)
		{
			slot = i;
			break;
		}
		if (formationGroups[i].createdTime < oldestTime)
		{
			oldestTime = formationGroups[i].createdTime;
			oldestSlot = i;
		}
	}
	if (slot < 0)
		slot = oldestSlot;

	FormationGroup& grp = formationGroups[slot];
	grp.destX = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_x, 0));
	grp.destY = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_y, 4));
	grp.destZ = (*(const float*)KLIB_MEMBER(5, dest, Ogre__Vector3_z, 8));
	grp.count = 0;
	grp.createdTime = now;
	grp.active = true;
	grp.gathered = false;
	grp.lastReissueTime = 0.0;
	FormationPaceResetGroup(slot);
	{
		float gatherRadius = FormationGatherRadius(charCount);
		grp.gatherRadiusSq = gatherRadius * gatherRadius;
	}

	// Capture leader position as the gather point
	grp.startX = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_x, OFF_CHAR_POS_X));
	grp.startY = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_y, OFF_CHAR_POS_Y));
	grp.startZ = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_z, OFF_CHAR_POS_Z));
	unsigned char alone[MAX_FORMATION_MEMBERS_LIMIT] = { 0 };   // the merge's members left alone
	float mergeDist = 0.0f;
	GatherClock& gc = s_gather[slot];
	memset(&gc, 0, sizeof(gc));
	gc.merged = ReadPlannerMerge(grp, chars, charCount, alone, &mergeDist);
	float farthestSq = 0.0f;
	float speed = 0.0f;

	unsigned int groupId = nextFormationGroupId++;
	grp.groupId = groupId;

	// Scatter radius: half of original formula for tighter arrival spread
	float scatterRadius = sqrtf((float)charCount * 60.0f) * 0.5f + 5.0f;
	int nonLeaderCount = charCount - 1;
	bool allAlreadyNear = true;

	for (int i = 0; i < charCount && grp.count < MAX_FORMATION_MEMBERS; ++i)
	{
		uintptr_t ch = chars[i];
		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, ch, Character_movement, OFF_CHAR_MOVEMENT));
		if (!cm) continue;

		// Check if this member is already near the gather point; the farthest one and the slowest
		// size the gather timeout.
		if (!alone[i])
		{
			float gdx = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_x, OFF_CHAR_POS_X)) - grp.startX;
			float gdz = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_z, OFF_CHAR_POS_Z)) - grp.startZ;
			float gSq = gdx * gdx + gdz * gdz;
			if (gSq > grp.gatherRadiusSq)
				allAlreadyNear = false;
			if (gSq > farthestSq)
				farthestSq = gSq;
			speed = GatherSpeedOf(cm, speed);
		}

		FormationMember& m = grp.members[grp.count];
		m.character = ch;
		m.charMovement = cm;
		m.dispatched = false;
		m.gatherSent = false;
		m.holdAtCreation        = CharacterIsHolding(ch);
		m.inSomethingAtCreation = CharacterIsInSomething(ch);
		m.alone = alone[i] != 0;

		if (grp.count == 0)
		{
			// Leader stays at exact destination
			m.scatterX = 0.0f;
			m.scatterZ = 0.0f;
		}
		else if (nonLeaderCount > 0)
		{
			// Deterministic circle: evenly spaced around destination
			float angle = 6.2831853f * (float)(grp.count - 1) / (float)nonLeaderCount;
			m.scatterX = sinf(angle) * scatterRadius;
			m.scatterZ = cosf(angle) * scatterRadius;
		}
		grp.count++;
	}
	{
		// The merge's route distance, or the farthest straight-line walk when that is longer (a route is
		// never shorter than its straight line; an unmerged order, or a merge that gathers at its anchor,
		// records no route distance).
		float straight = sqrtf(farthestSq);
		gc.timeout = FormationGatherTimeout(mergeDist > straight ? mergeDist : straight, speed,
		                                    GATHER_TIMEOUT, GATHER_TIMEOUT_CAP);
		gc.startActive = OOT_ActiveNow(now, ZonePauseIsPaused());
	}


	// Pre-gathered: skip the gather phase entirely. orig_addOrderSelected
	// (called after this returns) issues the move orders directly — no
	// gather cancel, no triple-move-order sequence.
	if (allAlreadyNear && grp.count > 1)
	{
		grp.gathered = true;
	}

	{
		std::ostringstream ss;
		ss << "Formation group " << groupId << ": "
		   << grp.count << " members, dest=(" << std::fixed << std::setprecision(0)
		   << grp.destX << "," << grp.destZ << ")"
		   << " radius=" << std::setprecision(1) << scatterRadius;
		if (grp.gathered)
			ss << " [pre-gathered]";
		LogMsg(ss.str());
	}
}


// =========================================================================
// Formation group polling
// =========================================================================

namespace formation_detail {

// Retire one group slot.
void DeactivateFormationGroup(int g)
{
	formationGroups[g].active = false;
}

} // namespace formation_detail
using namespace formation_detail;

// A group completing (every alive member within its own approach
// radius, PollFormationGroups below) closes any order_outcome.cpp record
// shared by its members that member-level retirement (arrival, knockout,
// cancel, squad departure) has not already closed on its own. Called only
// at completion, not at a drop or the stale timeout: both of those can fire
// while members are still genuinely walking (a leader leaving the squad
// mid-route, or a 29-cell order that outlives the group's own timeout), and
// closing the order record there would score an in-progress order as
// unrecovered. order_outcome.cpp's own per-frame poll and its safety
// timeout close a record on squad departure or old age instead.
static void NoteGroupOrderComplete(const FormationGroup& grp, double now)
{
	uintptr_t chars[MAX_FORMATION_MEMBERS_LIMIT];
	int n = 0;
	for (int m = 0; m < grp.count && n < MAX_FORMATION_MEMBERS_LIMIT; ++m)
		if (grp.members[m].character) chars[n++] = grp.members[m].character;
	if (n > 0) OrderOutcomeOnGroupComplete(chars, n, now);
}

namespace formation_detail
{

// Per-group state for one main-thread poll; no lock is held across phases.
struct PollFormationGroupCtx
{
	int g;
	double now;
	unsigned int scCount;
	uintptr_t* scStuff;
	int aliveCount, doneCount;
};

bool PollFormationLiveness(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// Liveness pass. Groups live up to FORMATION_TIMEOUT seconds (a merged
	// order's group, while a member still walks to the destination, up to
	// FORMATION_TIMEOUT_CEILING), so a
	// mid-session save load — or any character leaving the squad — can leave
	// members pointing at freed Character objects. Validate every member
	// against the live player list BEFORE anything below dereferences
	// mem.character (the timeout count, the "Running Together" check and the
	// phase bodies all read it). A member that is gone is dropped from the
	// group; if the leader (member 0) is gone, the whole group is retired.
	// The list is known readable here: PollFormationGroups returns early
	// otherwise, so nothing below ever dereferences an unvalidated member.
	{
		bool leaderGone = false;
		int aliveMembers = 0;
		for (int m = 0; m < grp.count; ++m)
		{
			if (!grp.members[m].character) continue;

			bool alive = false;
			for (unsigned int j = 0; j < c.scCount; ++j)
			{
				if (c.scStuff[j] == grp.members[m].character) { alive = true; break; }
			}
			if (!alive)
			{
				grp.members[m].character    = 0;
				grp.members[m].charMovement = 0;
				if (m == 0) leaderGone = true;
				continue;
			}
			aliveMembers++;
		}
		if (leaderGone || aliveMembers == 0)
		{
			std::ostringstream ss;
			ss << "Formation group dropped: "
			   << (leaderGone ? "leader" : "all members") << " no longer in the player list";
			LogMsg(ss.str());
			// Not an order-outcome close: a leader leaving the
			// player squad does not mean the remaining members stopped
			// walking. order_outcome.cpp's own per-frame poll closes
			// each member's record on its own squad-departure test.
			DeactivateFormationGroup(c.g);
			return true;
		}
	}
	return false;
}

// The members still walking to the group's destination: live, not yet dispatched to their arrival
// slot, outside the arrival radius, with a movement destination within FORMATION_WALK_DEST_TOL of the
// group's. Main thread, after the liveness pass validated every member.
static int CountWalkingToDest(const FormationGroup& grp)
{
	const float tolSq = FORMATION_WALK_DEST_TOL * FORMATION_WALK_DEST_TOL;
	int walking = 0;
	for (int m = 0; m < grp.count; ++m)
	{
		const FormationMember& mem = grp.members[m];
		if (!mem.character || mem.dispatched)
			continue;
		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, mem.character, Character_movement, OFF_CHAR_MOVEMENT));
		if (!cm)
			continue;
		float ddx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST)) - grp.destX;
		float ddz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8)) - grp.destZ;
		float pdx = GetCharPosX(mem.character) - grp.destX;
		float pdz = GetCharPosZ(mem.character) - grp.destZ;
		if (ddx * ddx + ddz * ddz <= tolSq && pdx * pdx + pdz * pdz >= SCATTER_APPROACH_DIST_SQ)
			++walking;
	}
	return walking;
}

bool PollFormationTimeout(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// Stale timeout; a merged order's group outlives it while a member still walks to the destination.
	double age = c.now - grp.createdTime;
	bool merged = s_gather[c.g].merged;
	int walking = (age > FORMATION_TIMEOUT && merged) ? CountWalkingToDest(grp) : 0;
	if (FormationTimeoutCancels(age, FORMATION_TIMEOUT, FORMATION_TIMEOUT_CEILING, merged, walking))
	{
		int pending = 0;
		for (int m = 0; m < grp.count; ++m)
			if (grp.members[m].character && !grp.members[m].dispatched) pending++;
		{
			std::ostringstream ss;
			ss << "Formation group timeout: " << pending
			   << "/" << grp.count << " pending after "
			   << std::fixed << std::setprecision(0) << age << "s";
			LogMsg(ss.str());
		}
		// Not an order-outcome close: the 120 s formation timeout is
		// shorter than a long order's own walk time (a 29-cell order can
		// run 90+ s end to end), so scoring it here would mark an
		// in-progress order as unrecovered mid-route. order_outcome.cpp's
		// own 600 s safety timeout is the record's real backstop.
		grp.active = false;
		return true;
	}
	return false;
}

bool PollFormationSpeed(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// Check that all members are still set to "Running Together".
	// If any member changed speed mode, deactivate the group and
	// let the game's regular movement logic handle it.
	{
		bool stillGrouped = true;
		for (int m = 0; m < grp.count; ++m)
		{
			if (!grp.members[m].character) continue;
			uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, grp.members[m].character, Character_movement, OFF_CHAR_MOVEMENT));
			if (!cm || *(int*)(KLIB_MEMBER(3, cm, AbstractMovementBase_speedOrders, OFF_CMOV_SPEED_MODE)) != MOVESPEED_GROUPED)
			{ stillGrouped = false; break; }
		}
		if (!stillGrouped)
		{
			grp.active = false;
			return true;
		}
	}
	return false;
}

// The members the travel send departs: every live one the merge did not leave alone, less a follower
// the follow probe owns (counted once, at FFS_SEND) and a member held since the order (cleared from the
// group). Their indices go to sendable[]; returns the count. Main thread, at the gather's completion.
static int CollectTravelMembers(FormationGroup& grp, int* sendable)
{
	int n = 0;
	for (int m = 0; m < grp.count; ++m)
	{
		FormationMember& mem = grp.members[m];
		if (!mem.character) continue;
		if (mem.alone) continue;   // already walking its own route to the destination
		if (FormationOwnsFollower(mem.charMovement, FFS_SEND)) continue;
		if (CharacterNewlyHeld(mem.character, mem.holdAtCreation, mem.inSomethingAtCreation))
		{
			mem.character    = 0;
			mem.charMovement = 0;
			continue;
		}
		sendable[n++] = m;
	}
	return n;
}

// The route planner re-plans each member the travel send departs from where it stands, before the send.
static void ResumePlannedMembers(const FormationGroup& grp, const int* sendable, int n, double now)
{
	uintptr_t gathered[MAX_FORMATION_MEMBERS_LIMIT];
	for (int i = 0; i < n; ++i)
		gathered[i] = grp.members[sendable[i]].character;
	planner::PlannerResumeFromGather(gathered, n, now);
}

// Sends a member's gather order to pos through playerMoveOrderDefault (vtable+792). The route planner
// keeps the member's plan through its walk to the gather point, stamped in game time, which stands still
// while the game is paused. Main thread, on a member the gather loop found alive.
static void DispatchGatherOrder(uintptr_t character, float pos[3], double now)
{
	uintptr_t charVtable = *(uintptr_t*)character;
	if (!charVtable)
		return;
	typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
	moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
	if (!fn_moveOrder)
		return;
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (cm)
		planner::PlannerNoteModSend(cm, pos, planner::PLAN_SEND_HOLD, OOT_ActiveNow(now, ZonePauseIsPaused()));
	KlibDispatchMoveOrder(fn_moveOrder, character, NULL, NULL, pos);
}

// A gathering member whose gather order the engine deleted short of the point is sent it again, at most
// once per REISSUE_COOLDOWN: nudged off its last destination so the engine does not drop it as a
// duplicate, and stamped on the tracker's cooldown. Main thread, after the member's proximity test.
static void ResendGatherIfDeleted(const FormationGroup& grp, FormationMember& mem, double& sendTime,
                                  float distSq, double now)
{
	const float farSq = GATHER_RESEND_FAR * GATHER_RESEND_FAR;
	bool gone = mem.gatherSent && IslandK7OrderGone(mem.character);
	if (!FormationGatherResendDue(mem.gatherSent, distSq, grp.gatherRadiusSq, farSq, gone, now - sendTime, REISSUE_COOLDOWN))
		return;
	float pos[3] = { grp.startX, grp.startY, grp.startZ };
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, mem.character, Character_movement, OFF_CHAR_MOVEMENT));
	if (cm)
	{
		float lx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
		float lz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
		IslandNudgeAwayFromLastDest(lx, lz, &pos[0], &pos[2]);
	}
	DispatchGatherOrder(mem.character, pos, now);
	IslandMarkReissued(mem.character, now);
	sendTime = now;
}

bool PollFormationGather(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// ============================================================
	// Phase 1: Gathering — send each member to the gather point (grp.start*), and again when the engine deleted its order
	// ============================================================
	if (!grp.gathered)
	{
		bool allNear = true;
		bool anySent = false;

		for (int m = 0; m < grp.count; ++m)
		{
			FormationMember& mem = grp.members[m];
			FormationPaceForget(c.g, m);   // noted again below only for a member the gather paces
			if (!mem.character) continue;

			// Validate alive
			bool alive = false;
			if (c.scStuff && c.scCount > 0 && c.scCount <= 200)
			{
				for (unsigned int j = 0; j < c.scCount; ++j)
				{
					if (c.scStuff[j] == mem.character) { alive = true; break; }
				}
			}
			if (!alive) { mem.character = 0; continue; }
			// A member the merge left alone walks its own route: no gather order, no gather test.
			if (mem.alone) continue;
			if (FormationOwnsFollower(mem.charMovement, FFS_GATHER)) continue;   // a follower walks on its follow task

			// Send gather order once per member
			if (!mem.gatherSent)
			{
				float gatherPos[3] = { grp.startX, grp.startY, grp.startZ };
				DispatchGatherOrder(mem.character, gatherPos, c.now);
				mem.gatherSent = true;
				s_gather[c.g].sendTime[m] = c.now;
				anySent = true;
			}

			// Check proximity to gather point
			float dx = GetCharPosX(mem.character) - grp.startX;
			float dz = GetCharPosZ(mem.character) - grp.startZ;
			float distSq = dx * dx + dz * dz;
			FormationPaceNote(c.g, m, distSq, grp.gatherRadiusSq, c.now);
			if (distSq > grp.gatherRadiusSq)
				allNear = false;
			ResendGatherIfDeleted(grp, mem, s_gather[c.g].sendTime[m], distSq, c.now);
		}

		bool gatherTimeout = (OOT_ActiveNow(c.now, ZonePauseIsPaused()) - s_gather[c.g].startActive > s_gather[c.g].timeout);
		if ((allNear && !anySent) || gatherTimeout)
		{
			grp.gathered = true;

			// Arm multi-call path probe to capture the path request flow
			// for this formation dispatch (8s capture window)
			if (pathfind::g_pathfindCfg.pathfindDiagEnabled)
				ArmPathProbe();

			int sendable[MAX_FORMATION_MEMBERS_LIMIT];
			int sendCount = CollectTravelMembers(grp, sendable);
			ResumePlannedMembers(grp, sendable, sendCount, c.now);
			// Send every departing member to the destination
			int departed = 0;
			for (int i = 0; i < sendCount; ++i)
			{
				FormationMember& mem = grp.members[sendable[i]];
				uintptr_t charVtable = *(uintptr_t*)mem.character;
				if (!charVtable) continue;

				typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
				moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
				if (fn_moveOrder)
				{
					float destPos[3] = { grp.destX, grp.destY, grp.destZ };
					KlibDispatchMoveOrder(fn_moveOrder, mem.character, NULL, NULL, destPos);
					departed++;
				}
			}

			{
				std::ostringstream ss;
				ss << "Formation gathered: " << departed << " departing";
				FormationPaceAppendGathered(c.g, grp, ss);
				ss << (gatherTimeout ? " (timeout)" : "");
				LogMsg(ss.str());
			}
		}
		// A group still gathering stages its members' factors; one that completed this poll stages
		// none, so its travel is never paced.
		if (!grp.gathered)
			FormationPaceStageGroup(c.g, grp);
		return true;  // end this poll even if the group just gathered
	}
	return false;
}

bool PollFormationScatter(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// ============================================================
	// Phase 2: Scatter — dispatch arrival spread at destination
	// ============================================================
	c.aliveCount = 0;
	c.doneCount = 0;

	for (int m = 0; m < grp.count; ++m)
	{
		FormationMember& mem = grp.members[m];
		if (!mem.character)
		{
			c.doneCount++;
			continue;
		}

		// Validate character is still alive
		bool alive = false;
		if (c.scStuff && c.scCount > 0 && c.scCount <= 200)
		{
			for (unsigned int j = 0; j < c.scCount; ++j)
			{
				if (c.scStuff[j] == mem.character) { alive = true; break; }
			}
		}
		if (!alive)
		{
			mem.character = 0;
			c.doneCount++;
			continue;
		}
		c.aliveCount++;

		if (mem.dispatched)
		{
			c.doneCount++;
			continue;
		}

		if (CharacterNewlyHeld(mem.character, mem.holdAtCreation, mem.inSomethingAtCreation))
		{
			mem.character    = 0;
			mem.charMovement = 0;
			c.doneCount++;
			continue;
		}

		// Check proximity to exact destination
		float charX = GetCharPosX(mem.character);
		float charZ = GetCharPosZ(mem.character);

		float dx = charX - grp.destX;
		float dz = charZ - grp.destZ;
		float distSq = dx * dx + dz * dz;

		if (distSq < SCATTER_APPROACH_DIST_SQ)
		{
			// Leader (scatter offset 0,0) needs no redirection
			if (mem.scatterX == 0.0f && mem.scatterZ == 0.0f)
			{
				mem.dispatched = true;
				c.doneCount++;
				continue;
			}

			// Dispatch scatter via playerMoveOrderDefault (vtable+792)
			uintptr_t charVtable = *(uintptr_t*)mem.character;
			if (!charVtable)
			{
				mem.character = 0;
				c.doneCount++;
				continue;
			}

			typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
			moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
			if (!fn_moveOrder)
			{
				mem.character = 0;
				c.doneCount++;
				continue;
			}

			float scatterDest[3];
			scatterDest[0] = grp.destX + mem.scatterX;
			scatterDest[1] = grp.destY;
			scatterDest[2] = grp.destZ + mem.scatterZ;

			FormationFollowRelease(&mem.character, 1, FFR_ARRIVAL);
			KlibDispatchMoveOrder(fn_moveOrder, mem.character, NULL, NULL, scatterDest);
			mem.dispatched = true;
			c.doneCount++;

			{
				std::ostringstream ss;
				ss << "Formation scatter: member " << m
				   << " dist=" << std::fixed << std::setprecision(0) << sqrtf(distSq)
				   << " offset=(" << std::setprecision(1)
				   << mem.scatterX << "," << mem.scatterZ << ")";
				LogMsg(ss.str());
			}
		}
	}
	return false;
}

bool PollFormationComplete(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// Group complete when all members dispatched or removed
	if (c.doneCount >= grp.count)
	{
		std::ostringstream ss;
		ss << "Formation group complete: "
		   << c.aliveCount << " alive, " << c.doneCount << "/" << grp.count << " done";
		LogMsg(ss.str());
		NoteGroupOrderComplete(grp, c.now);
		grp.active = false;
	}
	return true;
}

void PollFormationGroup(int g, double now, unsigned int scCount, uintptr_t* scStuff)
{
	if (!formationGroups[g].active)
		return;

	FormationGroup& grp = formationGroups[g];
	PollFormationGroupCtx c;
	c.g = g;
	c.now = now;
	c.scCount = scCount;
	c.scStuff = scStuff;
	if (PollFormationLiveness(grp, c))
		return;
	if (PollFormationTimeout(grp, c))
		return;
	if (PollFormationSpeed(grp, c))
		return;
	if (PollFormationGather(grp, c))
		return;
	if (PollFormationScatter(grp, c))
		return;
	if (PollFormationComplete(grp, c))
		return;
}

} // namespace formation_detail
using namespace formation_detail;

void PollFormationGroups()
{
	double now = ElapsedSec();

	// Read playerCharacters lektor once for validation. Without a readable list
	// no member can be validated, and no member may be dereferenced unvalidated,
	// so the whole poll is skipped for this frame. Groups keep their state and
	// are polled again as soon as the list comes back.
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return;
	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff   = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 200)
		return;

	FormationPaceFrameBegin();
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
		PollFormationGroup(g, now, scCount, scStuff);
	FormationFollowPoll(now, scCount, scStuff);
	FormationPaceFramePublish();
}
