// formation_pace.cpp - The gather pacing: each member's arrival, distance and paced mark, the factor
// table the formation poll stages and publishes, the post-call detour on SpeedGroup::getSpeed that
// scales a gathering member's speed by its factor, the install step and the tokens. Main thread,
// except hook_getSpeed (the AI back thread, or the main thread with characterMultithreading off):
// one table read and a multiply, no lock, no allocation, no logging.
#include "movement/formation_pace.h"
#include "movement/formation_pace_policy.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include <iomanip>
#include <string>

namespace formation_pace_detail {

// One member slot's pace state. A slot keeps its index for its group's life: a member that leaves is
// zeroed in place, and only CreateFormationGroup fills slots, from 0, after FormationPaceResetGroup.
struct PaceMember
{
	double gatherArrive;   // when it first stood within the gather radius; 0 not yet
	float  distSq;         // this poll's squared x-z distance to the gather point; -1 not noted
	bool   paced;          // its gather speed was scaled below the group's at least once
};

} // namespace formation_pace_detail
using namespace formation_pace_detail;

typedef float (*getSpeed_t)(void* group, void* who);

static getSpeed_t    orig_getSpeed = NULL;
static volatile LONG s_armed       = 0;      // set once the detour is in; main thread writes
static PaceTable     s_table;                // published by the main thread, read by the detour
static PaceEntry     s_staged[PACE_TABLE_MAX];   // main thread
static int           s_stagedCount = 0;      // main thread
static float         s_minFactor   = 2.0f;   // the smallest factor staged since the last diag; 2: none
static volatile LONG s_hits        = 0;      // the detour's answers below the group's speed
static PaceMember    s_pace[MAX_FORMATION_GROUPS][MAX_FORMATION_MEMBERS_LIMIT];   // main thread

// Member slot m of group g, or NULL outside the table.
static PaceMember* PaceSlot(int g, int m)
{
	if (g < 0 || g >= MAX_FORMATION_GROUPS || m < 0 || m >= MAX_FORMATION_MEMBERS_LIMIT)
		return NULL;
	return &s_pace[g][m];
}

// The engine's speed for who, scaled by who's published factor. A NULL who asks for the group's own
// speed, which is never scaled.
static float hook_getSpeed(void* group, void* who)
{
	float speed = orig_getSpeed(group, who);
	if (!who)
		return speed;
	float f = PaceTableRead(&s_table, (size_t)who);
	if (!(f < 1.0f))
		return speed;
	InterlockedIncrement(&s_hits);
	return speed * f;
}

void InstallFormationPace(int* installed, int*)
{
	if (!HookRowWanted(HOOK_SPEED_GROUP_GET_SPEED))
	{
		LogMsg("Formation pace: pace=off");
		return;
	}
	const char* why = HookInstall(HOOK_SPEED_GROUP_GET_SPEED, hook_getSpeed, &orig_getSpeed, installed, true);
	if (why)
	{
		ErrorLog(std::string("Formation pace: pace=refused(") + why + ")");
		return;
	}
	InterlockedExchange(&s_armed, 1);
	LogMsg("Formation pace: pace=ok");
}

void FormationPaceResetGroup(int g)
{
	for (int m = 0; m < MAX_FORMATION_MEMBERS_LIMIT; ++m)
	{
		PaceMember* s = PaceSlot(g, m);
		if (!s)
			return;
		s->gatherArrive = 0.0;
		s->distSq = -1.0f;
		s->paced = false;
	}
}

void FormationPaceForget(int g, int m)
{
	PaceMember* s = PaceSlot(g, m);
	if (s)
		s->distSq = -1.0f;
}

void FormationPaceNote(int g, int m, float distSq, float gatherRadiusSq, double now)
{
	PaceMember* s = PaceSlot(g, m);
	if (!s)
		return;
	s->distSq = distSq;
	if (!(distSq > gatherRadiusSq) && s->gatherArrive == 0.0)
		s->gatherArrive = now;
}

void FormationPaceFrameBegin()
{
	if (!s_armed)
		return;
	s_stagedCount = 0;
}

// Whether the member's movement destination is the group's gather point; a member a later order sent
// elsewhere is not. Main thread, on a member the gather loop found alive this poll.
static bool MemberWalksGather(uintptr_t character, const FormationGroup& grp)
{
	uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
	if (!cm)
		return false;
	float x = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
	float z = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
	return FormationPaceWalksGather(x, z, grp.startX, grp.startZ);
}

void FormationPaceStageGroup(int g, const FormationGroup& grp)
{
	if (!s_armed)
		return;
	if (!PaceSlot(g, 0))
		return;
	int n = grp.count < MAX_FORMATION_MEMBERS_LIMIT ? grp.count : MAX_FORMATION_MEMBERS_LIMIT;
	float distSq[MAX_FORMATION_MEMBERS_LIMIT];
	for (int m = 0; m < n; ++m)
	{
		distSq[m] = grp.members[m].character ? s_pace[g][m].distSq : -1.0f;
		if (distSq[m] >= 0.0f && !MemberWalksGather(grp.members[m].character, grp))
			distSq[m] = -1.0f;
	}
	float maxSq = FormationPaceMaxDistSq(distSq, n);
	for (int m = 0; m < n; ++m)
	{
		if (distSq[m] < 0.0f)
			continue;
		float f = FormationPaceFactor(distSq[m], maxSq, grp.gatherRadiusSq);
		if (!(f < 1.0f) || s_stagedCount >= PACE_TABLE_MAX)
			continue;
		s_staged[s_stagedCount].character = (size_t)grp.members[m].character;
		s_staged[s_stagedCount].factor = f;
		++s_stagedCount;
		s_pace[g][m].paced = true;
		if (f < s_minFactor)
			s_minFactor = f;
	}
}

void FormationPaceFramePublish()
{
	if (!s_armed)
		return;
	if (s_stagedCount == 0 && s_table.count == 0)
		return;
	PaceTablePublish(&s_table, s_staged, s_stagedCount);
}

void FormationPaceClear()
{
	for (int g = 0; g < MAX_FORMATION_GROUPS; ++g)
		FormationPaceResetGroup(g);
	if (!s_armed)
		return;
	s_stagedCount = 0;
	PaceTablePublish(&s_table, NULL, 0);
}

void FormationPaceAppendGathered(int g, const FormationGroup& grp, std::ostringstream& ss)
{
	int n = grp.count < MAX_FORMATION_MEMBERS_LIMIT ? grp.count : MAX_FORMATION_MEMBERS_LIMIT;
	double arrive[MAX_FORMATION_MEMBERS_LIMIT];
	int paced = 0;
	for (int m = 0; m < n; ++m)
	{
		const PaceMember* s = PaceSlot(g, m);
		arrive[m] = s ? s->gatherArrive : 0.0;
		if (s && s->paced)
			++paced;
	}
	std::streamsize precision = ss.precision();
	ss << " spread=" << std::fixed << std::setprecision(1) << FormationGatherSpread(arrive, n)
	   << " paced=" << paced;
	ss.precision(precision);
}

void FormationPaceAppendDiag(std::ostringstream& ss)
{
	if (!s_armed)
	{
		ss << " paceMin=off";
		return;
	}
	std::streamsize precision = ss.precision();
	if (s_minFactor > 1.0f)
		ss << " paceMin=-";
	else
		ss << " paceMin=" << std::fixed << std::setprecision(2) << s_minFactor;
	ss << " paceHits=" << InterlockedExchange(&s_hits, 0);
	ss.precision(precision);
	s_minFactor = 2.0f;
}
