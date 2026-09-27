// formation.cpp - Group state, creation, deactivation and main-thread polling.
// Called on the main thread, plus one clear at hook install.

#include "movement/formation.h"
#include "movement/formation_members.h"
#include "pathfind/pathfinding.h"
#include "movement/formation_internal.h"
#include "movement/order_outcome.h"

FormationGroup formationGroups[MAX_FORMATION_GROUPS];

bool scatterPatchApplied = false;

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
	{
		float gatherRadius = FormationGatherRadius(charCount);
		grp.gatherRadiusSq = gatherRadius * gatherRadius;
	}

	// Capture leader position as the gather point
	grp.startX = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_x, OFF_CHAR_POS_X));
	grp.startY = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_y, OFF_CHAR_POS_Y));
	grp.startZ = *(float*)(KLIB_MEMBER(3, chars[0], RootObjectBase_pos_z, OFF_CHAR_POS_Z));

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

		// Check if this member is already near the leader (gather point)
		if (allAlreadyNear)
		{
			float gdx = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_x, OFF_CHAR_POS_X)) - grp.startX;
			float gdz = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_z, OFF_CHAR_POS_Z)) - grp.startZ;
			if (gdx * gdx + gdz * gdz > grp.gatherRadiusSq)
				allAlreadyNear = false;
		}

		FormationMember& m = grp.members[grp.count];
		m.character = ch;
		m.charMovement = cm;
		m.dispatched = false;
		m.gatherSent = false;
		m.holdAtCreation        = CharacterIsHolding(ch);
		m.inSomethingAtCreation = CharacterIsInSomething(ch);

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
	// Liveness pass. Groups live up to FORMATION_TIMEOUT seconds, so a
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

bool PollFormationTimeout(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// Stale timeout
	if (c.now - grp.createdTime > FORMATION_TIMEOUT)
	{
		int pending = 0;
		for (int m = 0; m < grp.count; ++m)
			if (grp.members[m].character && !grp.members[m].dispatched) pending++;
		{
			std::ostringstream ss;
			ss << "Formation group timeout: " << pending
			   << "/" << grp.count << " pending after "
			   << std::fixed << std::setprecision(0) << FORMATION_TIMEOUT << "s";
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

bool PollFormationGather(FormationGroup& grp, PollFormationGroupCtx& c)
{
	// ============================================================
	// Phase 1: Gathering — send all members to leader's position
	// ============================================================
	if (!grp.gathered)
	{
		bool allNear = true;
		bool anySent = false;

		for (int m = 0; m < grp.count; ++m)
		{
			FormationMember& mem = grp.members[m];
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

			// Send gather order once per member
			if (!mem.gatherSent)
			{
				uintptr_t charVtable = *(uintptr_t*)mem.character;
				if (charVtable)
				{
					typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
					moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
					if (fn_moveOrder)
					{
						float gatherPos[3] = { grp.startX, grp.startY, grp.startZ };
						KlibDispatchMoveOrder(fn_moveOrder, mem.character, NULL, NULL, gatherPos);
					}
				}
				mem.gatherSent = true;
				anySent = true;
			}

			// Check proximity to gather point
			float dx = GetCharPosX(mem.character) - grp.startX;
			float dz = GetCharPosZ(mem.character) - grp.startZ;
			if (dx * dx + dz * dz > grp.gatherRadiusSq)
				allNear = false;
		}

		bool gatherTimeout = (c.now - grp.createdTime > GATHER_TIMEOUT);
		if ((allNear && !anySent) || gatherTimeout)
		{
			grp.gathered = true;

			// Arm multi-call path probe to capture the path request flow
			// for this formation dispatch (8s capture window)
			if (pathfindDiagEnabled)
				ArmPathProbe();

			// Send all alive members to the destination
			int departed = 0;
			for (int m = 0; m < grp.count; ++m)
			{
				FormationMember& mem = grp.members[m];
				if (!mem.character) continue;

				if (CharacterNewlyHeld(mem.character, mem.holdAtCreation, mem.inSomethingAtCreation))
				{
					mem.character    = 0;
					mem.charMovement = 0;
					continue;
				}

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
				ss << "Formation gathered: " << departed << " departing"
				   << (gatherTimeout ? " (timeout)" : "");
				LogMsg(ss.str());
			}
		}
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

	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
		PollFormationGroup(g, now, scCount, scStuff);
}
