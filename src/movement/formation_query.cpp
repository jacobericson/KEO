// formation_query.cpp - Formation queries, detachment and travel reissues.
// Main thread; no formation lock spans a game call or diagnostic output.

#include "movement/formation_internal.h"
#include "movement/formation_members.h"
// REISSUE_COOLDOWN, IslandNudgeAwayFromLastDest and the (a) discriminator trace.
#include "movement/islands.h"
#include "movement/order_outcome.h"
#include "planner/plan_store.h"

// =========================================================================
// Island re-issue helpers for islands_reissue.cpp. Main thread only.
// =========================================================================

int FormationSlotForCharacter(uintptr_t character)
{
	if (!character) return -1;
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		// Active groups only; a still-gathering group is reported too so its
		// members are never re-issued individually (FormationReissueTravel
		// declines until the group has gathered).
		const FormationGroup& grp = formationGroups[g];
		if (!grp.active) continue;
		for (int m = 0; m < grp.count; ++m)
			if (grp.members[m].character == character)
				return g;
	}
	return -1;
}

// The merge's alone flag of the character's member in its active group, by FormationSlotForCharacter's
// scan; false in no group.
bool FormationMemberAlone(uintptr_t character)
{
	if (!character) return false;
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		const FormationGroup& grp = formationGroups[g];
		if (!grp.active) continue;
		for (int m = 0; m < grp.count; ++m)
			if (grp.members[m].character == character)
				return grp.members[m].alone;
	}
	return false;
}

bool FormationGroupDestNear(int slot, float x, float z, float maxDistSq)
{
	if (slot < 0 || slot >= MAX_FORMATION_GROUPS) return false;
	const FormationGroup& grp = formationGroups[slot];
	if (!grp.active) return false;
	float dx = x - grp.destX;
	float dz = z - grp.destZ;
	return dx * dx + dz * dz <= maxDistSq;
}

// FormationRebaseReissueClocks shifts the group's reissue clock after a pause.
// A field that changes FormationGroup's size (so does
// MAX_FORMATION_MEMBERS_LIMIT) stops the build here: decide whether it is a
// clock the rebase must shift, then update the size. A field that fits in
// existing padding, or a same-size change, leaves the size as it was.
static_assert(sizeof(FormationGroup) == 2112, "FormationGroup changed: decide whether FormationRebaseReissueClocks must shift the new field");
void FormationRebaseReissueClocks(double pausedSeconds)
{
	if (pausedSeconds <= 0.0) return;
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		if (!formationGroups[g].active) continue;
		if (formationGroups[g].lastReissueTime > 0.0)
			formationGroups[g].lastReissueTime += pausedSeconds;
	}
}

// Zeroing is the only change FormationZeroMatchingMembers makes: every
// reader below already skips a zero character (liveness pass, gather and
// scatter, FormationSlotForCharacter, FormationFirstAliveMember), so a
// detached leader simply hands the representative role to the next alive
// member. When the detach leaves every member zeroed, FormationGroupExhausted
// says so and the group is retired immediately rather than waiting for the
// next poll to notice.
void FormationDetachCharacters(const uintptr_t* chars, int n)
{
	if (!chars || n <= 0) return;
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		if (!formationGroups[g].active) continue;
		FormationGroup& grp = formationGroups[g];

		uintptr_t memberChars[MAX_FORMATION_MEMBERS_LIMIT];
		for (int m = 0; m < grp.count; ++m)
		{
			if (grp.members[m].character)
				FormationZeroMatchingMembers(&grp.members[m].character, &grp.members[m].charMovement, 1, chars, n);
			memberChars[m] = grp.members[m].character;
		}

		if (FormationGroupExhausted(memberChars, grp.count))
			DeactivateFormationGroup(g);
	}
}

uintptr_t FormationFirstAliveMember(int slot)
{
	if (slot < 0 || slot >= MAX_FORMATION_GROUPS) return 0;
	const FormationGroup& grp = formationGroups[slot];
	if (!grp.active) return 0;

	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned int scCount = 0;
	uintptr_t* scStuff = NULL;
	if (playerIntf)
	{
		scCount = GetPlayerCharCount(playerIntf);
		scStuff = GetPlayerCharStuff(playerIntf);
	}
	if (!scStuff || scCount == 0 || scCount > 200) return 0;

	for (int m = 0; m < grp.count; ++m)
	{
		uintptr_t ch = grp.members[m].character;
		if (!ch) continue;
		for (unsigned int j = 0; j < scCount; ++j)
			if (scStuff[j] == ch)
				return ch;
	}
	return 0;
}

// Read-only: no order is issued and no group state is touched, so a group
// whose members have scattered is reported, never changed.
void FormationCohesionSample(int* groups, int* liveMembers,
                             float* worstSpread, int* worstGroupId)
{
	int nGroups = 0, nLive = 0, worstId = -1;
	float worst = 0.0f;

	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned int scCount = 0;
	uintptr_t* scStuff = NULL;
	if (playerIntf)
	{
		scCount = GetPlayerCharCount(playerIntf);
		scStuff = GetPlayerCharStuff(playerIntf);
	}
	if (!scStuff || scCount == 0 || scCount > 200)
		scCount = 0;

	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
	{
		const FormationGroup& grp = formationGroups[g];
		if (!grp.active) continue;
		++nGroups;

		float xs[MAX_FORMATION_MEMBERS_LIMIT];
		float zs[MAX_FORMATION_MEMBERS_LIMIT];
		uintptr_t live[MAX_FORMATION_MEMBERS_LIMIT];
		int n = 0;
		for (int m = 0; m < grp.count && n < MAX_FORMATION_MEMBERS_LIMIT; ++m)
		{
			uintptr_t ch = grp.members[m].character;
			live[n] = 0;
			xs[n] = 0.0f;
			zs[n] = 0.0f;
			if (ch && scCount)
			{
				for (unsigned int j = 0; j < scCount; ++j)
				{
					if (scStuff[j] != ch) continue;
					live[n] = ch;
					xs[n] = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_x, OFF_CHAR_POS_X));
					zs[n] = *(float*)(KLIB_MEMBER(3, ch, RootObjectBase_pos_z, OFF_CHAR_POS_Z));
					++nLive;
					break;
				}
			}
			++n;
		}

		float spread = FormationMaxPairwiseSpread(xs, zs, live, n);
		if (spread > worst)
		{
			worst = spread;
			worstId = (int)grp.groupId;
		}
	}

	if (groups) *groups = nGroups;
	if (liveMembers) *liveMembers = nLive;
	if (worstSpread) *worstSpread = worst;
	if (worstGroupId) *worstGroupId = worstId;
}

// Main thread: the route planner holds this member at one of its portals.
static bool PlannerOwnsMember(uintptr_t cm)
{
	float px = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
	float pz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
	float wx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	float wz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	return planner::PlannerOwnsWait(cm, px, pz, wx, wz);
}

// The re-issue's preconditions: a gathered active group outside its cooldown, and the live player
// list (at most 200 characters) into *stuffOut and *countOut; false declines the re-issue.
static bool GatherReissue(int slot, double now, uintptr_t** stuffOut, unsigned int* countOut)
{
	if (slot < 0 || slot >= MAX_FORMATION_GROUPS) return false;
	FormationGroup& grp = formationGroups[slot];
	if (!grp.active || !grp.gathered) return false;
	if (grp.lastReissueTime > 0.0 && now - grp.lastReissueTime < REISSUE_COOLDOWN) return false;

	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned int scCount = 0;
	uintptr_t* scStuff = NULL;
	if (playerIntf)
	{
		scCount = GetPlayerCharCount(playerIntf);
		scStuff = GetPlayerCharStuff(playerIntf);
	}
	if (!scStuff || scCount == 0 || scCount > 200) return false;
	*stuffOut = scStuff;
	*countOut = scCount;
	return true;
}

// Records one member's re-send for the dispatch's post check.
static void RecordReissueMember(int slot, int m, uintptr_t character, const float destPos[3],
                                const IslandReissueTrace& trace, double now, int traceDispatch)
{
	std::ostringstream label;
	// The char@<hex low 16 bits> suffix (same form as the solo
	// label, and PLAYER TASK's char=@) ties a member's result to
	// that character's PLAYER TASK lines; the "group N member M"
	// prefix is kept for existing greps.
	label << "group " << slot << " member " << m
	      << " char@" << std::hex << (character & 0xFFFF) << std::dec;
	std::string labelStr = label.str();

	// Record, do not classify. fn_moveOrder is applied
	// asynchronously, so +0xDC read here still holds the previous
	// destination; island_reissue.cpp classifies (IslandClassifyReissuePost,
	// the single owner of post=) and logs 1 s later.
	IslandRecordReissueCheck(character, labelStr.c_str(), destPos[0], destPos[2],
	                         trace, now, traceDispatch);
}

// Sends each eligible member of the group the group's destination, nudged, records the dispatch and
// stamps the cooldown; false when no member was sent.
static bool SendReissue(int slot, const char* why, double now, const uintptr_t* scStuff, unsigned int scCount)
{
	FormationGroup& grp = formationGroups[slot];

	// Same dispatch as the gather->travel transition in PollFormationGroups.
	int sent = 0;
	int nudged = 0;
	// (a): one line per member, or (group larger than 6) one summary line
	// plus only the members whose post is not "sent". The results are
	// resolved 1 s later from IslandTick (islands.cpp), so the
	// summary is tracked there as a dispatch: every member recorded below
	// joins it, and the summary prints once all of them have resolved or been
	// dropped. traceDispatch is -1 outside summary mode.
	int traceDispatch = IslandBeginReissueDispatch(slot, grp.count > 6);
	for (int m = 0; m < grp.count; ++m)
	{
		FormationMember& mem = grp.members[m];
		if (!mem.character) continue;

		bool alive = false;
		for (unsigned int j = 0; j < scCount; ++j)
			if (scStuff[j] == mem.character) { alive = true; break; }
		if (!alive) { mem.character = 0; continue; }

		if (CharacterNewlyHeld(mem.character, mem.holdAtCreation, mem.inSomethingAtCreation))
		{
			mem.character    = 0;
			mem.charMovement = 0;
			continue;
		}

		// Never send a member a second move order within one cooldown window
		// (e.g. it was already re-issued solo this cycle via the (c)
		// per-member path in PollOrders, and the representative then parked
		// too in the same or very next poll).
		if (IslandRecentlyReissued(mem.character, now))
			continue;
		// A member another task is preempting (a freeze, self-
		// preservation, a stumble) still has its move queued and resumes it;
		// an order sent now would be appended behind the preempting task.
		if (IslandK7Preempted(mem.character))
			continue;
		uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, mem.character, Character_movement, OFF_CHAR_MOVEMENT));
		// A member the route planner holds at a portal is not re-sent.
		if (cm && PlannerOwnsMember(cm)) { InterlockedIncrement(&planner::PlannerCountersGet()->ownedSkips); continue; }

		uintptr_t charVtable = *(uintptr_t*)mem.character;
		if (!charVtable) continue;
		typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
		moveOrderFn_t fn_moveOrder = (moveOrderFn_t)(*(uintptr_t*)(charVtable + 0x318));
		if (!fn_moveOrder) continue;

		float destPos[3] = { grp.destX, grp.destY, grp.destZ };

		// CharMovement::setDestination drops a new order within 2 units of the
		// last requested destination while it is routing to an island edge.
		// Every member received the exact grp.dest (scatter patch), so nudge.
		// (a)/(b): capture the pre-call trace and use the direction-aware nudge.
		IslandReissueTrace trace;
		IslandCaptureReissueTrace(mem.character, &trace);
		if (cm)
		{
			float lx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
			float lz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
			IslandNudgeAwayFromLastDest(lx, lz, &destPos[0], &destPos[2]);
			if (destPos[0] != grp.destX || destPos[2] != grp.destZ)
				nudged++;
		}
		if (cm) planner::PlannerNoteModSend(cm, destPos, planner::PLAN_SEND_RESEND, now);

		KlibDispatchMoveOrder(fn_moveOrder, mem.character, NULL, NULL, destPos);
		sent++;

		// Stamp every member this dispatch actually reached (representative
		// included -- ReissueOrder stamps the same field on the
		// representative's own entry right after this call returns, so this
		// is a harmless duplicate write there, not a second timestamp
		// source). A member whose own order is swallowed by this blast still
		// carries a fresh cooldown, so the PollOrders (c) solo path defers
		// its own reissue until the cooldown clears instead of firing
		// immediately on its first fresh park check.
		IslandMarkReissued(mem.character, now);
		// Every member this dispatch reached, not just the representative
		// ReissueOrder's own success point credits -- otherwise a group
		// member's K7-family recovery through this path falls into
		// selfRec instead of k7rec.
		OrderOutcomeNoteReissueSent(mem.character, why, now);

		RecordReissueMember(slot, m, mem.character, destPos, trace, now, traceDispatch);
	}

	// Close the dispatch (every Begin needs its End, even with nothing sent:
	// an empty dispatch is freed without a line, as before).
	IslandEndReissueDispatch(traceDispatch);

	if (sent == 0) return false;
	grp.lastReissueTime = now;

	std::ostringstream ss;
	ss << "Formation reissue: group slot " << slot
	   << " " << sent << " members (" << nudged << " nudged)"
	   << " dest=(" << std::fixed << std::setprecision(0)
	   << grp.destX << "," << grp.destZ << ")";
	LogMsg(ss.str());
	return true;
}

bool FormationReissueTravel(int slot, const char* why, double now)
{
	uintptr_t* scStuff = NULL;
	unsigned int scCount = 0;
	if (!GatherReissue(slot, now, &scStuff, &scCount)) return false;
	return SendReissue(slot, why, now, scStuff, scCount);
}
