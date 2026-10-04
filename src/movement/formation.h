// formation.h — Group cohesion: scatter patch, formation groups (Layer 3)
// Depends on: config.h

#ifndef KEO_FORMATION_H
#define KEO_FORMATION_H

#include "base/config.h"

// =========================================================================
// Formation constants
// =========================================================================

const int MAX_FORMATION_GROUPS       = 8;
const int MAX_FORMATION_MEMBERS      = 30;
const float SCATTER_APPROACH_DIST_SQ = 2500.0f;  // 50 world units squared
// Gather-completion radius: per group, sized to its member count
// (FormationGatherRadius, formation_members.h) and stored as grp.gatherRadiusSq.
const double GATHER_TIMEOUT          = 15.0;      // seconds before skipping gather
const double FORMATION_TIMEOUT       = 120.0;     // seconds before auto-cleanup


// =========================================================================
// Formation structs
// =========================================================================

struct FormationMember {
	uintptr_t character;
	uintptr_t charMovement;
	float scatterX;
	float scatterZ;
	bool dispatched;
	bool gatherSent;
	bool holdAtCreation;         // CharStats::_holdPositionMode when the group formed
	bool inSomethingAtCreation;  // Character::inSomething != 0 when the group formed
	bool alone;                  // the route planner's merge left it out of the gather: it walks its own route
};

struct FormationGroup {
	float destX, destY, destZ;
	float startX, startY, startZ;
	FormationMember members[MAX_FORMATION_MEMBERS_LIMIT];
	int count;
	double createdTime;
	bool active;
	bool gathered;
	double lastReissueTime;   // FormationReissueTravel cooldown (0 = never)
	float gatherRadiusSq;     // squared gather-completion radius, sized to this group's member count
	unsigned int groupId;     // stable display id (nextFormationGroupId at creation), for log lines
};


// =========================================================================
// Formation state (defined in formation.cpp)
// =========================================================================

extern bool scatterPatchApplied;
extern FormationGroup formationGroups[MAX_FORMATION_GROUPS];


// =========================================================================
// Formation functions (formation.cpp and scatter_patch.cpp)
// =========================================================================

bool ApplyScatterPatch();
void CreateFormationGroup(const float* dest, uintptr_t* chars, int charCount);
void PollFormationGroups();
void ClearFormationGroups();

// Island re-issue helpers (formation_query.cpp). Main thread only.
// Slot of the active formation group containing `character` (-1 = none).
int       FormationSlotForCharacter(uintptr_t character);
// True when `character` is a member of an active group that the route planner's merge left alone (it
// walks its own route to the destination while the group gathers); false in no group.
bool      FormationMemberAlone(uintptr_t character);
// First member of the group that is still in the player squad (0 = none).
uintptr_t FormationFirstAliveMember(int slot);
// Re-dispatch the travel order (grp.dest) to every alive member, once per
// boundary move: returns false while the per-group cooldown is active.
// A member whose last requested destination is within 8 units of grp.dest is
// sent a point 8 units from that last destination instead, so
// CharMovement::setDestination does not drop the order.
bool      FormationReissueTravel(int slot, const char* why, double now);
// True if (x,z) is within sqrt(maxDistSq) of the active group's destination.
bool      FormationGroupDestNear(int slot, float x, float z, float maxDistSq);
// Shift every active group's lastReissueTime forward by pausedSeconds
// (k7_reissue.cpp's K7RebasePausedClocks, on the frame a pause ends).
// Never touches createdTime: PollFormationGroups is not pause-gated, so that
// clock already runs through a pause.
void      FormationRebaseReissueClocks(double pausedSeconds);

// Drop `chars[0..n)` from every formation group they belong to: the member's
// character/charMovement become 0, so every consumer (liveness pass, gather,
// scatter, the two helpers above) treats it exactly like a member that left
// the player's squad. Called whenever a player order supersedes a member's
// squad membership (a newer move, a bed, an attack, a stop, Hold). Main
// thread only.
void      FormationDetachCharacters(const uintptr_t* chars, int n);

// Cohesion sample over the active groups (main thread). *groups = active
// groups, *liveMembers = their live members in total, *worstSpread = the
// largest max-pairwise XZ spread of any one group, *worstGroupId = which
// group that was. A group whose members are no longer in the player squad
// contributes no positions. With no active group every output is 0 and
// *worstGroupId is -1, so "no groups" and "one group, everyone together"
// are told apart by the group count, not by the spread.
void      FormationCohesionSample(int* groups, int* liveMembers,
                                  float* worstSpread, int* worstGroupId);

#endif // KEO_FORMATION_H
