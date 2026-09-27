#include "fixes/streaming/create_instance_guard_policy.h"

bool CreateInstanceArmSkips(CreateInstanceArm arm)
{
	return arm == CI_ARM_SELF_LIVE;
}

bool CreateInstanceCallsOriginal(CreateInstanceArm arm, bool actMode)
{
	return !(actMode && CreateInstanceArmSkips(arm));
}

void InspectCreateInstanceCall(const void* navMesh, const void* n, CreateInstanceCall* out)
{
	out->arm         = CI_ARM_NEW;
	out->uid         = 0;
	out->instance    = 0;
	out->mesh        = 0;
	out->count       = 0;
	out->index       = -1;
	out->uidOther    = false;
	out->liveOutside = false;

	if (!navMesh || !n)
	{
		out->arm = CI_ARM_UNJUDGED_ARGS;
		return;
	}

	const unsigned char* ni = (const unsigned char*)n;
	out->uid      = *(const unsigned int*)(ni + OFF_CI_NI_UID);
	out->instance = *(const void* const*)(ni + OFF_CI_NI_INSTANCE);
	out->mesh     = *(const void* const*)(ni + OFF_CI_NI_MESH);

	const unsigned char* nm = (const unsigned char*)navMesh;
	const unsigned int count = *(const unsigned int*)(nm + OFF_CI_NAVMESH_ADDLIST_COUNT);
	const void* const* stuff = *(const void* const* const*)(nm + OFF_CI_NAVMESH_ADDLIST_STUFF);
	out->count = count;

	if (count > CI_MAX_ADDLIST || (count && !stuff))
	{
		out->arm = CI_ARM_UNJUDGED_LIST;
		return;
	}

	for (unsigned int i = 0; i < count; ++i)
	{
		const void* e = stuff[i];
		if (e == n)
		{
			if (out->index < 0)
				out->index = (int)i;
		}
		else if (e && *(const unsigned int*)((const unsigned char*)e + OFF_CI_NI_UID) == out->uid)
		{
			out->uidOther = true;
		}
	}

	if (out->index >= 0)
		out->arm = out->instance ? CI_ARM_SELF_LIVE : CI_ARM_SELF_NULL;
	else
		out->liveOutside = out->instance != 0;
}

CreateInstanceCaller ClassifyCreateInstanceCaller(size_t returnRva)
{
	if (returnRva == CI_RET_GENERATOR_UPDATE) return CI_CALLER_GENERATOR;
	if (returnRva == CI_RET_ZONE_SECTOR)      return CI_CALLER_ZONE_SECTOR;
	if (returnRva == CI_RET_ZONE_INTERIOR)    return CI_CALLER_ZONE_INTERIOR;
	return CI_CALLER_UNKNOWN;
}

bool CreateInstanceJudged(CreateInstanceCaller caller, unsigned long tid, unsigned long pathTid)
{
	if (caller != CI_CALLER_UNKNOWN)
		return true;
	return pathTid != 0 && tid == pathTid;
}

void InspectCreateInstanceTask(const void* navMesh, const void* n, CreateInstanceTask* out)
{
	out->have     = false;
	out->type     = -1;
	out->meshSame = false;
	out->haveZone = false;
	out->zoneX    = 0;
	out->zoneY    = 0;

	if (!navMesh || !n)
		return;
	const unsigned char* gen =
		*(const unsigned char* const*)((const unsigned char*)navMesh + OFF_CI_NAVMESH_GENERATOR);
	if (!gen)
		return;
	const unsigned char* task = *(const unsigned char* const*)(gen + OFF_CI_NMG_DONE_FRONT);
	if (!task || *(const void* const*)(task + OFF_CI_TASK_OUTPUT) != n)
		return;

	out->have     = true;
	out->type     = *(const int*)(task + OFF_CI_TASK_FLAGS) & CI_TASK_TYPE_MASK;
	out->meshSame = *(const void* const*)(task + OFF_CI_TASK_MESH)
	             == *(const void* const*)((const unsigned char*)n + OFF_CI_NI_MESH);

	const unsigned char* zone = *(const unsigned char* const*)(task + OFF_CI_TASK_ZONE);
	if (zone)
	{
		out->haveZone = true;
		out->zoneX = *(const int*)(zone + OFF_CI_ZONE_COORDS);
		out->zoneY = *(const int*)(zone + OFF_CI_ZONE_COORDS + 4);
	}
}
