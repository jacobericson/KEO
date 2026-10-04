// formation_follow.cpp - The follow probe: the Follow order through Character::addJob for a run-together
// group's members near the leader, the poll's capture, release and leader promotion, the reads behind the
// Follow: line, and the owner query the movement re-issuers and the order outcome consult. It steers
// nothing. Main thread, except FormationFollowNoteRequest and FormationFollowArmed (the AI back thread
// or the main thread: lock-free, no allocation, no log). The release build compiles the whole body out.
#include "movement/formation_follow.h"
#ifdef KEO_DEBUG
#include "movement/formation.h"
#include "movement/formation_members.h"
#include "movement/movement_config.h"
#include "movement/islands_reissue_internal.h"
#include "pathfind/pathfinding.h"
#include "planner/planner_tick.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace formation_follow_detail {

const int FOLLOW_RECORDS = MAX_FORMATION_GROUPS;

struct Follower
{
	uintptr_t character, cm, hc;
	int       following;      // 1 while the probe owns it
	int       seenTask;       // its current task has read the follow task since its capture
	double    lastTry;        // its last capture; 0 none
	int       lastMode2Poll;  // the poll of its last MOVE_DIRECTION read; -1 none
};

struct FollowRecord
{
	int       active;
	int       slot;                    // the formation group's slot and id at the order
	unsigned  groupId;
	int       leaderDetached;          // an order or the stop key took the leader
	double    created, lastLine, lastSample;
	uintptr_t leader, leaderCm;
	float     dest[3];
	float     gatherRadiusSq;
	int       count;
	Follower  members[MAX_FORMATION_MEMBERS];   // every group member but the leader at the order
	long      captures, released[FFR_COUNT];
	long      mode2Reads, mode2Gaps, mode2GapSum;   // this line's window
	float     samples[FOLLOW_SAMPLES_MAX];
	int       sampleCount;
};

typedef void (*addJob_t)(void* character, int task, void* subject, bool shift, bool addDontClear,
                         const float* location);

} // namespace formation_follow_detail
using namespace formation_follow_detail;

static const unsigned char kAddJobPrologue[16] =
	{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };

// The main thread's state; s_armed and the follower words are also read by the request counter.
static FollowRecord       s_records[FOLLOW_RECORDS];
static int                s_armState   = 0;   // 0 not tried, 1 armed, -1 off or refused
static volatile LONG      s_armed      = 0;
static addJob_t           fn_addJob    = NULL;
static int                s_poll       = 0;
static int                s_orphanDue  = 0;
static long               s_orphans    = 0;
static long               s_dropped    = 0;
static long               s_ownedSkips[FFS_COUNT];   // per seam
static volatile uintptr_t s_followHc[FOLLOW_RECORDS][MAX_FORMATION_MEMBERS];   // 0: not following
static volatile LONG      s_followReq[FOLLOW_RECORDS];

// The probe arms once, at its first use on the main thread: the key at probe and Character::addJob's
// first 16 bytes as read (the release build has no body to arm).
static void ArmOnce()
{
	if (s_armState != 0)
		return;
	s_armState = -1;
	if (movement::g_movementCfg.cfg_formationFollow != FF_PROBE)
		return;
	const void* addJob = GameAddr(RVA_CHARACTER_ADD_JOB);
	bool ok = memcmp(addJob, kAddJobPrologue, sizeof(kAddJobPrologue)) == 0;
	LogMsg(ok ? "Follow: probe armed addJob=ok" : "Follow: probe refused addJob=refused(addJob)");
	if (!ok)
		return;
	fn_addJob = (addJob_t)addJob;
	s_armState = 1;
	InterlockedExchange(&s_armed, 1);
}

static bool InList(const uintptr_t* stuff, unsigned count, uintptr_t character)
{
	for (unsigned i = 0; character && i < count; ++i)
		if (stuff[i] == character)
			return true;
	return false;
}

static void PosOf(uintptr_t character, float out[3])
{
	out[0] = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_x, OFF_CHAR_POS_X));
	out[1] = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_y, OFF_CHAR_POS_Y));
	out[2] = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_z, OFF_CHAR_POS_Z));
}

static float DistSqXz(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dz = a[2] - b[2];
	return dx * dx + dz * dz;
}

static bool Grouped(uintptr_t cm)
{
	return cm && *(int*)(KLIB_MEMBER(3, cm, AbstractMovementBase_speedOrders, OFF_CMOV_SPEED_MODE)) == MOVESPEED_GROUPED;
}

static uintptr_t HavokOf(uintptr_t cm)
{
	return cm ? *(uintptr_t*)(KLIB_MEMBER(3, cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR)) : 0;
}

// A move order through playerMoveOrderDefault (vtable+0x318), as the formation sends its own.
static void SendMove(uintptr_t character, const float dest[3])
{
	uintptr_t vt = *(uintptr_t*)character;
	if (!vt)
		return;
	typedef void (*moveOrderFn_t)(uintptr_t, void*, void*, const float*);
	moveOrderFn_t fn = (moveOrderFn_t)(*(uintptr_t*)(vt + 0x318));
	if (fn)
		KlibDispatchMoveOrder(fn, character, NULL, NULL, dest);
}

// The engine's own Follow order on the record's leader; addDontClear false replaces the member's orders.
static void Issue(const FollowRecord& r, const Follower& f)
{
	float at[3];
	PosOf(r.leader, at);
	fn_addJob((void*)f.character, FOLLOW_TASK_TYPE, (void*)r.leader, false, false, at);
}

static void Capture(FollowRecord& r, int ri, int i, double now)
{
	Follower& f = r.members[i];
	planner::PlannerDrop(f.character);
	Issue(r, f);
	f.following = 1;
	f.seenTask = 0;
	f.lastTry = now;
	f.lastMode2Poll = -1;
	s_followHc[ri][i] = f.hc;
	++r.captures;
}

static void Release(FollowRecord& r, int ri, int i, int reason, bool sendDest)
{
	Follower& f = r.members[i];
	if (!f.following)
		return;
	f.following = 0;
	s_followHc[ri][i] = 0;
	if (reason > FFR_NONE && reason < FFR_COUNT)
		++r.released[reason];
	if (reason == FFR_DROPPED)
		++s_dropped;
	if (sendDest)
		SendMove(f.character, r.dest);
}

static void EndRecord(FollowRecord& r, int ri)
{
	r.active = 0;
	for (int i = 0; i < MAX_FORMATION_MEMBERS; ++i)
		s_followHc[ri][i] = 0;
}

// A free record, else the oldest one, ended.
static int TakeRecord()
{
	int oldest = 0;
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
	{
		if (!s_records[ri].active)
			return ri;
		if (s_records[ri].created < s_records[oldest].created)
			oldest = ri;
	}
	EndRecord(s_records[oldest], oldest);
	return oldest;
}

void FormationFollowAfterOrder(const uintptr_t* chars, int n)
{
	ArmOnce();
	if (!s_armed || !chars || n < 2)
		return;
	int slot = FormationSlotForCharacter(chars[0]);
	if (slot < 0)
		return;
	const FormationGroup& grp = formationGroups[slot];
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
	{
		FollowRecord& r = s_records[ri];
		if (!r.active || r.slot != slot || r.groupId != grp.groupId)
			continue;
		// A repeat of the group's order: the original gave every member the move order again.
		r.dest[0] = grp.destX;
		r.dest[1] = grp.destY;
		r.dest[2] = grp.destZ;
		for (int i = 0; i < r.count; ++i)
			if (r.members[i].following)
				Issue(r, r.members[i]);
		return;
	}
	if (grp.count < 2 || grp.members[0].character != chars[0])
		return;
	double now = ElapsedSec();
	int ri = TakeRecord();
	FollowRecord& r = s_records[ri];
	memset(&r, 0, sizeof(r));
	r.active = 1;
	r.slot = slot;
	r.groupId = grp.groupId;
	r.created = now;
	r.lastLine = now;
	r.leader = grp.members[0].character;
	r.leaderCm = grp.members[0].charMovement;
	r.dest[0] = grp.destX;
	r.dest[1] = grp.destY;
	r.dest[2] = grp.destZ;
	r.gatherRadiusSq = grp.gatherRadiusSq;
	float lead[3];
	PosOf(r.leader, lead);
	for (int m = 1; m < grp.count && r.count < MAX_FORMATION_MEMBERS; ++m)
	{
		const FormationMember& mem = grp.members[m];
		if (!mem.character)
			continue;
		int i = r.count++;
		Follower& f = r.members[i];
		f.character = mem.character;
		f.cm = mem.charMovement;
		f.hc = HavokOf(mem.charMovement);
		f.lastMode2Poll = -1;
		float p[3];
		PosOf(f.character, p);
		if (FollowAtOrder(m, DistSqXz(p, lead), r.gatherRadiusSq))
			Capture(r, ri, i, now);
	}
}

// After a save load: the player characters still on a follow task, which the probe leaves alone.
static void CountOrphans(const uintptr_t* stuff, unsigned count)
{
	s_orphanDue = 0;
	for (unsigned i = 0; i < count; ++i)
		if (order_tracker_detail::ReadCharOrderType(stuff[i]) == FOLLOW_TASK_TYPE)
			++s_orphans;
}

// One follower's reads for the line: a MOVE_DIRECTION read and its gap in polls since the last, and its
// distance to the leader every FOLLOW_SAMPLE_SECONDS. Diagnostic only.
static void Observe(FollowRecord& r, Follower& f, const float lead[3], bool leaderLive, bool sample)
{
	int mode = *(int*)(KLIB_MEMBER(3, f.cm, CharMovement_movementMode, OFF_CMOV_MOVEMENT_MODE));
	if (mode == FOLLOW_MOVE_DIRECTION)
	{
		++r.mode2Reads;
		if (f.lastMode2Poll >= 0)
		{
			++r.mode2Gaps;
			r.mode2GapSum += s_poll - f.lastMode2Poll;
		}
		f.lastMode2Poll = s_poll;
	}
	if (sample && leaderLive && r.sampleCount < FOLLOW_SAMPLES_MAX)
	{
		float p[3];
		PosOf(f.character, p);
		r.samples[r.sampleCount++] = sqrtf(DistSqXz(p, lead));
	}
}

// The first live follower becomes the leader: it is sent the destination and the others follow it.
static void Promote(FollowRecord& r, int ri, const uintptr_t* stuff, unsigned count)
{
	int live[MAX_FORMATION_MEMBERS];
	for (int i = 0; i < r.count; ++i)
		live[i] = (r.members[i].following && InList(stuff, count, r.members[i].character)) ? 1 : 0;
	int next = FollowPromote(live, r.count);
	if (next < 0)
	{
		EndRecord(r, ri);
		return;
	}
	Release(r, ri, next, FFR_PROMOTED, true);
	r.leader = r.members[next].character;
	r.leaderCm = r.members[next].cm;
	r.leaderDetached = 0;
	r.members[next].character = 0;
	for (int i = 0; i < r.count; ++i)
		if (r.members[i].following)
			Issue(r, r.members[i]);
}

static void ReportLine(FollowRecord& r, int ri, double now, bool leaderLive, const float lead[3])
{
	r.lastLine = now;
	float xs[MAX_FORMATION_MEMBERS + 1], zs[MAX_FORMATION_MEMBERS + 1];
	uintptr_t who[MAX_FORMATION_MEMBERS + 1];
	int n = 0, followers = 0, seen = 0;
	if (leaderLive)
	{
		xs[n] = lead[0];
		zs[n] = lead[2];
		who[n++] = r.leader;
	}
	for (int i = 0; i < r.count; ++i)
	{
		if (!r.members[i].following)
			continue;
		float p[3];
		PosOf(r.members[i].character, p);
		xs[n] = p[0];
		zs[n] = p[2];
		who[n++] = r.members[i].character;
		++followers;
		if (r.members[i].seenTask)
			++seen;
	}
	float spread = FormationMaxPairwiseSpread(xs, zs, who, n);
	float p50 = FollowPercentile(r.samples, r.sampleCount, 50);
	float p90 = FollowPercentile(r.samples, r.sampleCount, 90);
	LONG req = InterlockedExchange(&s_followReq[ri], 0);
	double gap = r.mode2Gaps > 0 ? (double)r.mode2GapSum / (double)r.mode2Gaps : 0.0;
	char line[768];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Follow: group=%u leader=@%x followers=%d seen=%d captures=%ld rel=ord%ld/stop%ld/spd%ld/arr%ld"
	            "/cmp%ld/gone%ld/drop%ld/to%ld/promo%ld spread=%.0f dist50=%.0f dist90=%.0f mode2=%ld mode2Gap=%.1f"
	            " req=%ld followDropped=%ld followOrphans=%ld ownedSkips=park%ld/poll%ld/out%ld/travel%ld/gather%ld/send%ld",
	            r.groupId, (unsigned)(r.leader & 0xFFFF), followers, seen, r.captures, r.released[FFR_ORDER],
	            r.released[FFR_STOP], r.released[FFR_SPEED], r.released[FFR_ARRIVAL], r.released[FFR_COMPLETE],
	            r.released[FFR_GONE], r.released[FFR_DROPPED], r.released[FFR_TIMEOUT], r.released[FFR_PROMOTED],
	            spread, p50, p90, r.mode2Reads, gap, (long)req, s_dropped, s_orphans, s_ownedSkips[FFS_PARK],
	            s_ownedSkips[FFS_POLL], s_ownedSkips[FFS_OUTCOME], s_ownedSkips[FFS_TRAVEL], s_ownedSkips[FFS_GATHER],
	            s_ownedSkips[FFS_SEND]);
	LogMsg(line);
	r.mode2Reads = 0;
	r.mode2Gaps = 0;
	r.mode2GapSum = 0;
	r.sampleCount = 0;
}

// Whether the group still lists the member and has not dispatched it.
static bool GroupMayCapture(const FormationGroup& grp, uintptr_t character)
{
	for (int m = 0; m < grp.count; ++m)
		if (grp.members[m].character == character)
			return FollowMayCapture(true, grp.members[m].dispatched);
	return FollowMayCapture(false, false);
}

static void PollRecord(int ri, double now, const uintptr_t* stuff, unsigned count)
{
	FollowRecord& r = s_records[ri];
	const FormationGroup& grp = formationGroups[r.slot];
	bool groupActive = grp.active && grp.groupId == r.groupId;
	bool leaderLive = !r.leaderDetached && InList(stuff, count, r.leader);
	float lead[3] = { 0.0f, 0.0f, 0.0f };
	if (leaderLive)
		PosOf(r.leader, lead);
	bool sample = now - r.lastSample >= FOLLOW_SAMPLE_SECONDS;
	if (sample)
		r.lastSample = now;
	int following = 0;
	for (int i = 0; i < r.count; ++i)
	{
		Follower& f = r.members[i];
		if (!f.character)
			continue;
		bool live = InList(stuff, count, f.character);
		if (!f.following)
		{
			if (!live)
			{
				f.character = 0;
				continue;
			}
			float p[3];
			PosOf(f.character, p);
			if (groupActive && leaderLive && GroupMayCapture(grp, f.character)
			    && FollowCaptureDue(DistSqXz(p, lead), r.gatherRadiusSq, now, f.lastTry))
				Capture(r, ri, i, now);
			continue;
		}
		FollowerState s;
		s.live = live ? 1 : 0;
		s.grouped = (live && Grouped(f.cm)) ? 1 : 0;
		s.taskNow = (live && order_tracker_detail::ReadCharOrderType(f.character) == FOLLOW_TASK_TYPE) ? 1 : 0;
		if (s.taskNow)
			f.seenTask = 1;
		s.seenTask = f.seenTask;
		int why = FollowerStep(s);
		if (why != FFR_NONE)
		{
			Release(r, ri, i, why, false);
			if (!live)
				f.character = 0;
			continue;
		}
		++following;
		Observe(r, f, lead, leaderLive, sample);
	}
	FollowRecordState st;
	st.followers = following;
	st.leaderLive = leaderLive ? 1 : 0;
	st.groupActive = groupActive ? 1 : 0;
	st.leaderGrouped = (leaderLive && Grouped(r.leaderCm)) ? 1 : 0;
	st.leaderDistSqToDest = leaderLive ? DistSqXz(lead, r.dest) : 0.0f;
	st.age = now - r.created;
	int reason = FFR_NONE;
	int action = FollowRecordStep(st, &reason);
	if (action == FRA_PROMOTE)
	{
		Promote(r, ri, stuff, count);
		return;
	}
	if (action == FRA_RELEASE_SEND || action == FRA_RELEASE)
		for (int i = 0; i < r.count; ++i)
			Release(r, ri, i, reason, action == FRA_RELEASE_SEND);
	if (action != FRA_KEEP || now - r.lastLine >= FOLLOW_LINE_SECONDS)
		ReportLine(r, ri, now, leaderLive, lead);
	if (action != FRA_KEEP)
		EndRecord(r, ri);
}

void FormationFollowPoll(double now, unsigned int scCount, const uintptr_t* scStuff)
{
	ArmOnce();
	if (!s_armed || !scStuff)
		return;
	++s_poll;
	if (s_orphanDue)
		CountOrphans(scStuff, scCount);
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
		if (s_records[ri].active)
			PollRecord(ri, now, scStuff, scCount);
}

void FormationFollowRelease(const uintptr_t* chars, int n, int reason)
{
	if (!s_armed || !chars || n <= 0)
		return;
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
	{
		FollowRecord& r = s_records[ri];
		if (!r.active)
			continue;
		for (int k = 0; k < n; ++k)
		{
			if (chars[k] == r.leader && (reason == FFR_ORDER || reason == FFR_STOP))
				r.leaderDetached = 1;
			for (int i = 0; i < r.count; ++i)
				if (r.members[i].character == chars[k])
					Release(r, ri, i, reason, false);
		}
	}
}

void FormationFollowOnClear()
{
	if (!s_armed)
		return;
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
		EndRecord(s_records[ri], ri);
	s_orphanDue = 1;
}

bool FormationOwnsFollower(uintptr_t cm, int seam)
{
	if (!s_armed || !cm || seam < 0 || seam >= FFS_COUNT)
		return false;
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
	{
		const FollowRecord& r = s_records[ri];
		if (!r.active)
			continue;
		for (int i = 0; i < r.count; ++i)
		{
			if (r.members[i].cm != cm || !FollowOwns((int)s_armed, r.members[i].following))
				continue;
			++s_ownedSkips[seam];
			return true;
		}
	}
	return false;
}

bool FormationFollowArmed()
{
	return s_armed != 0;
}

void FormationFollowNoteRequest(void* havokChar)
{
	if (!s_armed || !havokChar)
		return;
	uintptr_t hc = (uintptr_t)havokChar;
	for (int ri = 0; ri < FOLLOW_RECORDS; ++ri)
	{
		for (int i = 0; i < MAX_FORMATION_MEMBERS; ++i)
		{
			if (s_followHc[ri][i] != hc)
				continue;
			InterlockedIncrement(&s_followReq[ri]);
			return;
		}
	}
}

#endif // KEO_DEBUG
