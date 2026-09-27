#ifndef KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_POLICY_H

#include <stddef.h>

// Layout and classification for NavMesh::createInstance(NavMesh*, NavInstance* n).
//
// The function gives n a fresh navmesh instance, then walks the add list for
// an entry with n's uid and treats every match as a stale duplicate: it takes
// the entry out, destroys its instance, nulls it and frees the entry. Only
// then does it push n. When the match is n itself -- n already queued -- it
// frees n and pushes the freed pointer, and the add-list drain later reads
// n->instance out of freed memory. The walk compares uids, so a different
// object with n's uid is the case it was written for and still runs.
//
// No Windows header and no game pointer here, so a host test can fabricate
// the add list and drive every arm.

// NavMesh: the add list is a lektor<NavInstance*> at +0x270.
const size_t OFF_CI_NAVMESH_ADDLIST_COUNT = 0x278;
const size_t OFF_CI_NAVMESH_ADDLIST_STUFF = 0x280;
const size_t OFF_CI_NAVMESH_GENERATOR     = 0x290;

// NavInstance (0x48 bytes).
const size_t OFF_CI_NI_MESH     = 0x08;
const size_t OFF_CI_NI_INSTANCE = 0x28;
const size_t OFF_CI_NI_UID      = 0x3C;

// NavMeshGenerator: the done queue's front task, and the task's fields.
const size_t OFF_CI_NMG_DONE_FRONT = 0xB8;
const size_t OFF_CI_TASK_ZONE      = 0x00;
const size_t OFF_CI_TASK_MESH      = 0x48;
const size_t OFF_CI_TASK_OUTPUT    = 0x50;
const size_t OFF_CI_TASK_FLAGS     = 0x58;
const int    CI_TASK_TYPE_MASK     = 7;
const int    CI_TASK_TYPE_STITCH   = 4;

// ZoneMap::coordinates.
const size_t OFF_CI_ZONE_COORDS = 0x18;

// An add list longer than this is not a list the game built; the call is left
// unjudged rather than walked.
const unsigned int CI_MAX_ADDLIST = 1u << 16;

// Return addresses (RVAs) of the three calls into createInstance.
const size_t CI_RET_GENERATOR_UPDATE = 0x3C8D64;  // NavMeshGenerator::update, a finished task
const size_t CI_RET_ZONE_SECTOR      = 0x3AD9CC;  // createZone, the loaded sector
const size_t CI_RET_ZONE_INTERIOR    = 0x3ADBB7;  // createZone, a loaded interior

enum CreateInstanceArm
{
	// n is not in the add list by pointer: the original runs.
	CI_ARM_NEW = 0,

	// Nothing could be judged; the original runs.
	CI_ARM_UNJUDGED_ARGS,     // this or n is NULL
	CI_ARM_UNJUDGED_LIST,     // the add list's count or array is not believable

	// n is in the add list by pointer. The original frees n on both arms.
	CI_ARM_SELF_NULL,         // with no instance: skipping would leave a NULL for the drain, so it runs
	CI_ARM_SELF_LIVE          // with a live instance: already queued, so the call is redundant
};

// The one arm the guard skips.
bool CreateInstanceArmSkips(CreateInstanceArm arm);

// Whether the detour calls the original: always, except on the skip arm with
// the guard acting. Observe mode (actMode false) calls it on every arm.
bool CreateInstanceCallsOriginal(CreateInstanceArm arm, bool actMode);

struct CreateInstanceCall
{
	CreateInstanceArm arm;
	unsigned int      uid;          // n->uid, 0 when n is NULL
	const void*       instance;     // n->instance at entry
	const void*       mesh;         // n->mesh at entry
	unsigned int      count;        // add-list length at entry
	int               index;        // n's add-list index, -1 when absent
	bool              uidOther;     // another object with n's uid is queued
	bool              liveOutside;  // n has an instance but is not queued
};

// Walks the add list for n by pointer and by uid. Reads only.
void InspectCreateInstanceCall(const void* navMesh, const void* n, CreateInstanceCall* out);

enum CreateInstanceCaller
{
	CI_CALLER_UNKNOWN = 0,
	CI_CALLER_GENERATOR,
	CI_CALLER_ZONE_SECTOR,
	CI_CALLER_ZONE_INTERIOR
};

CreateInstanceCaller ClassifyCreateInstanceCaller(size_t returnRva);

// Whether a call is judged at all. The three known return addresses run only
// inside NavMesh::update on the path thread, so those calls are always
// judged (and name the path thread). An unknown caller is judged only on the
// thread a known caller last came from.
bool CreateInstanceJudged(CreateInstanceCaller caller, unsigned long tid, unsigned long pathTid);

// What the generator's drain was finishing when it made the call. Attribution
// only: the skip decision never reads it.
struct CreateInstanceTask
{
	bool         have;       // done.front exists and its output is n
	int          type;       // flags & 7; 4 is a stitch task
	bool         meshSame;   // task->mesh == n->mesh by pointer (never dereferenced)
	bool         haveZone;
	int          zoneX;
	int          zoneY;
};

// Only meaningful when the caller is CI_CALLER_GENERATOR: the task being
// finished is done.front, and it is not popped until after the call.
void InspectCreateInstanceTask(const void* navMesh, const void* n, CreateInstanceTask* out);

#endif // KENSHI_ZONE_OPT_FIXES_CREATE_INSTANCE_GUARD_POLICY_H
