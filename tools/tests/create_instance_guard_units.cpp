// Host tests for the createInstance self-duplicate policy: every arm, the
// caller table, the task attribution, and the act/observe call decision.

#include <cstdio>
#include <cstring>
#include "fixes/streaming/create_instance_guard_policy.h"

#include "check.h"

struct Fake
{
	unsigned char navMesh[0x2C0];
	unsigned char gen[0x160];
	unsigned char task[0x68];
	unsigned char zone[0x40];
	unsigned char ni[4][0x48];
	void*         list[8];
	int           meshA, meshB, inst;
};

static void SetList(Fake* f, unsigned int count)
{
	*(unsigned int*)(f->navMesh + OFF_CI_NAVMESH_ADDLIST_COUNT) = count;
	*(void**)(f->navMesh + OFF_CI_NAVMESH_ADDLIST_STUFF) = count ? (void*)f->list : 0;
}

static void Init(Fake* f)
{
	memset(f, 0, sizeof(*f));
	for (int i = 0; i < 4; ++i)
		*(unsigned int*)(f->ni[i] + OFF_CI_NI_UID) = 0x390a10u + i;
	*(void**)(f->navMesh + OFF_CI_NAVMESH_GENERATOR) = f->gen;
	*(int*)(f->zone + OFF_CI_ZONE_COORDS)     = 24;
	*(int*)(f->zone + OFF_CI_ZONE_COORDS + 4) = 40;
	*(void**)(f->task + OFF_CI_TASK_ZONE) = f->zone;
	SetList(f, 0);
}

static void TestArms()
{
	Fake f; Init(&f);
	CreateInstanceCall c;

	InspectCreateInstanceCall(0, f.ni[0], &c);
	Check(c.arm == CI_ARM_UNJUDGED_ARGS, "null navMesh is unjudged");
	InspectCreateInstanceCall(f.navMesh, 0, &c);
	Check(c.arm == CI_ARM_UNJUDGED_ARGS, "null n is unjudged");

	// Empty list: a genuinely new instance.
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_NEW && c.index == -1 && !c.uidOther && !c.liveOutside, "empty list is new");

	// Others queued, n not.
	f.list[0] = f.ni[1]; f.list[1] = f.ni[2]; SetList(&f, 2);
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_NEW && c.count == 2, "n absent among others is new");

	// Same uid, different object: vanilla's own duplicate case, passed.
	*(unsigned int*)(f.ni[3] + OFF_CI_NI_UID) = 0x390a10u;
	f.list[2] = f.ni[3]; SetList(&f, 3);
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_NEW && c.uidOther, "same uid, other pointer is new with uidOther");
	Check(!CreateInstanceArmSkips(c.arm), "same uid, other pointer is not skipped");

	// n itself queued with a live instance: the recorded case.
	*(void**)(f.ni[0] + OFF_CI_NI_INSTANCE) = &f.inst;
	f.list[1] = f.ni[0]; SetList(&f, 3);
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_SELF_LIVE && c.index == 1 && c.uid == 0x390a10u, "self live found at its index");
	Check(c.instance == &f.inst, "self live reports the instance");
	Check(CreateInstanceArmSkips(c.arm), "self live skips");

	// n queued with no instance.
	*(void**)(f.ni[0] + OFF_CI_NI_INSTANCE) = 0;
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_SELF_NULL, "self null");
	Check(!CreateInstanceArmSkips(c.arm), "self null is not skipped");

	// Live instance, not queued.
	*(void**)(f.ni[0] + OFF_CI_NI_INSTANCE) = &f.inst;
	f.list[1] = f.ni[2]; SetList(&f, 2);
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_NEW && c.liveOutside, "live but unqueued is new with liveOutside");

	// Unbelievable lists.
	SetList(&f, 2);
	*(void**)(f.navMesh + OFF_CI_NAVMESH_ADDLIST_STUFF) = 0;
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_UNJUDGED_LIST, "count with no array is unjudged");
	*(unsigned int*)(f.navMesh + OFF_CI_NAVMESH_ADDLIST_COUNT) = CI_MAX_ADDLIST + 1;
	*(void**)(f.navMesh + OFF_CI_NAVMESH_ADDLIST_STUFF) = f.list;
	InspectCreateInstanceCall(f.navMesh, f.ni[0], &c);
	Check(c.arm == CI_ARM_UNJUDGED_LIST, "oversized count is unjudged");
}

static void TestDecision()
{
	Check(!CreateInstanceCallsOriginal(CI_ARM_SELF_LIVE, true), "act: self live not called");
	Check(CreateInstanceCallsOriginal(CI_ARM_SELF_LIVE, false), "observe: self live called");
	Check(CreateInstanceCallsOriginal(CI_ARM_SELF_NULL, true), "act: self null called");
	Check(CreateInstanceCallsOriginal(CI_ARM_NEW, true), "act: new called");
	Check(CreateInstanceCallsOriginal(CI_ARM_UNJUDGED_ARGS, true), "act: unjudged args called");
	Check(CreateInstanceCallsOriginal(CI_ARM_UNJUDGED_LIST, true), "act: unjudged list called");
	Check(CreateInstanceCallsOriginal(CI_ARM_NEW, false), "observe: new called");
}

static void TestCallers()
{
	Check(ClassifyCreateInstanceCaller(0x3C8D64) == CI_CALLER_GENERATOR, "generator return");
	Check(ClassifyCreateInstanceCaller(0x3AD9CC) == CI_CALLER_ZONE_SECTOR, "zone sector return");
	Check(ClassifyCreateInstanceCaller(0x3ADBB7) == CI_CALLER_ZONE_INTERIOR, "zone interior return");
	Check(ClassifyCreateInstanceCaller(0x3C8D5F) == CI_CALLER_UNKNOWN, "the call itself is not a return");
	Check(ClassifyCreateInstanceCaller(0) == CI_CALLER_UNKNOWN, "zero is unknown");

	// A known caller is judged on any thread, including after the path
	// thread is restarted under a new id; an unknown one only on the path
	// thread a known caller last named.
	Check(CreateInstanceJudged(CI_CALLER_GENERATOR, 7, 0), "known caller before any latch");
	Check(CreateInstanceJudged(CI_CALLER_ZONE_SECTOR, 9, 7), "known caller on a new thread");
	Check(CreateInstanceJudged(CI_CALLER_UNKNOWN, 7, 7), "unknown caller on the path thread");
	Check(!CreateInstanceJudged(CI_CALLER_UNKNOWN, 9, 7), "unknown caller elsewhere");
	Check(!CreateInstanceJudged(CI_CALLER_UNKNOWN, 7, 0), "unknown caller before any latch");
}

static void TestTask()
{
	Fake f; Init(&f);
	CreateInstanceTask t;

	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(!t.have && !t.haveZone && t.type == -1, "no done.front");

	*(void**)(f.gen + OFF_CI_NMG_DONE_FRONT) = f.task;
	*(void**)(f.task + OFF_CI_TASK_OUTPUT) = f.ni[1];
	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(!t.have, "front task for another output");

	// A stitch task whose mesh n has since replaced.
	*(void**)(f.task + OFF_CI_TASK_OUTPUT) = f.ni[0];
	*(int*)(f.task + OFF_CI_TASK_FLAGS) = 0x100 | CI_TASK_TYPE_STITCH;
	*(void**)(f.task + OFF_CI_TASK_MESH) = &f.meshA;
	*(void**)(f.ni[0] + OFF_CI_NI_MESH) = &f.meshB;
	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(t.have && t.type == CI_TASK_TYPE_STITCH, "stitch task type masked");
	Check(!t.meshSame, "stale stitch mesh");
	Check(t.haveZone && t.zoneX == 24 && t.zoneY == 40, "task zone");

	*(void**)(f.ni[0] + OFF_CI_NI_MESH) = &f.meshA;
	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(t.meshSame, "same stitch mesh");

	*(int*)(f.task + OFF_CI_TASK_FLAGS) = 3;
	*(void**)(f.task + OFF_CI_TASK_ZONE) = 0;
	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(t.have && t.type == 3 && !t.haveZone, "interior task, no zone");

	*(void**)(f.navMesh + OFF_CI_NAVMESH_GENERATOR) = 0;
	InspectCreateInstanceTask(f.navMesh, f.ni[0], &t);
	Check(!t.have, "no generator");
}

int main()
{
	TestArms();
	TestDecision();
	TestCallers();
	TestTask();
	return CheckExit("create_instance_guard_units");
}
