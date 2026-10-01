// Drives the shipped createInstance guard policy over a fabricated add list
// and a stand-in for the original, and shows:
//
//   1. vanilla's uid walk, modelled over a NavInstance on its own page: with
//      n already queued it frees n and queues the freed pointer, and the
//      drain's two loads through it (+0x28, then +0x1A4) fault;
//   2. act mode over the same list: the policy names the call a
//      self-duplicate, the stand-in original is never called, and the drain's
//      loads through n succeed;
//   3. observe mode: the same classification, and the stand-in is called
//      once with the caller's own arguments -- so the fault still happens;
//   4. a genuinely new instance, and an entry sharing n's uid through a
//      different object, both reach the original in act mode;
//   5. an instance already in the world: vanilla replaces n->instance, the
//      add-list drain refuses the duplicate uid, and after the teardown a
//      query through the old slot faults on the deleted mediator; act mode
//      leaves n alone and the teardown empties the slot; observe mode faults;
//   6. an instance not in the world, n not queued, still reaches the original.
//
// Links src/fixes/streaming/create_instance_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/streaming/create_instance_guard_policy.h"

#include "check.h"

const size_t OFF_NMI_SECTION_UID = 0x1A0;   // hkaiNavMeshInstance: the uid the drain looks up
const size_t OFF_NI_MEDIATOR     = 0x20;    // NavInstance: the mediator the teardown deletes
const int    SLOTS               = 64;

struct World
{
	unsigned char navMesh[0x2C0];
	void*         list[16];
	unsigned char instance[0x200];   // the live instance a newer generation gave n
	unsigned char fresh[0x200];      // the one the stand-in hands out
	unsigned char other[0x48];       // a different object with n's uid
	unsigned char* n;                // n on its own page, so "free" can decommit it
	void*         slotInstance[SLOTS];  // the collection, as far as the model goes
	void*         slotMediator[SLOTS];
	unsigned char* mediator;         // n's mediator on its own page, so the teardown can decommit it
};

static unsigned int Count(World* w) { return *(unsigned int*)(w->navMesh + OFF_CI_NAVMESH_ADDLIST_COUNT); }
static void SetCount(World* w, unsigned int c) { *(unsigned int*)(w->navMesh + OFF_CI_NAVMESH_ADDLIST_COUNT) = c; }

// Reserves the page once, then commits and zeroes it.
static unsigned char* Page(unsigned char* page)
{
	if (!page)
		page = (unsigned char*)VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_READWRITE);
	VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE);
	memset(page, 0, 4096);
	return page;
}

static void Init(World* w)
{
	unsigned char* page = w->n;
	unsigned char* mediatorPage = w->mediator;
	memset(w, 0, sizeof(*w));
	w->n = Page(page);
	w->mediator = Page(mediatorPage);
	*(void**)w->mediator = w;
	*(void**)(w->n + OFF_NI_MEDIATOR) = w->mediator;
	*(unsigned int*)(w->n + OFF_CI_NI_UID) = 0x390a19u;
	*(void**)(w->n + OFF_CI_NI_INSTANCE)   = w->instance;
	*(int*)(w->instance + OFF_CI_NMI_RUNTIME_ID) = -1;
	*(int*)(w->fresh + OFF_CI_NMI_RUNTIME_ID)    = -1;
	*(unsigned int*)(w->other + OFF_CI_NI_UID) = 0x390a19u;
	*(void**)(w->navMesh + OFF_CI_NAVMESH_ADDLIST_STUFF) = w->list;
}

// Vanilla's body, as far as the add list goes: a new instance, then every
// queued entry with n's uid is removed, nulled and freed, then n is pushed.
static World* s_world = 0;
static int    s_origCalls = 0;
static void*  s_origNavMesh = 0;
static void*  s_origN = 0;

static void VanillaCreateInstance(void* navMesh, void* nPtr)
{
	++s_origCalls;
	s_origNavMesh = navMesh;
	s_origN = nPtr;
	World* w = s_world;
	unsigned char* n = (unsigned char*)nPtr;
	*(void**)(n + OFF_CI_NI_INSTANCE) = w->fresh;
	const unsigned int uid = *(unsigned int*)(n + OFF_CI_NI_UID);
	for (unsigned int i = 0; i < Count(w); ++i)
	{
		unsigned char* e = (unsigned char*)w->list[i];
		if (*(unsigned int*)(e + OFF_CI_NI_UID) != uid)
			continue;
		for (unsigned int j = i + 1; j < Count(w); ++j)
			w->list[j - 1] = w->list[j];
		SetCount(w, Count(w) - 1);
		*(void**)(e + OFF_CI_NI_INSTANCE) = 0;
		if (e == w->n)
			VirtualFree(e, 4096, MEM_DECOMMIT);
		--i;
	}
	w->list[Count(w)] = n;
	SetCount(w, Count(w) + 1);
}

typedef void (*CreateInstanceFn)(void*, void*);

// The detour's decision, over the shipped policy.
static CreateInstanceArm GuardedCall(World* w, void* n, bool actMode, CreateInstanceFn orig)
{
	CreateInstanceCall c;
	InspectCreateInstanceCall(w->navMesh, n, &c);
	if (CreateInstanceCallsOriginal(c.arm, actMode))
		orig(w->navMesh, n);
	return c.arm;
}

// The drain's two loads through the last queued entry: n->instance, then
// its runtime id.
static bool DrainFaults(World* w, size_t* offset)
{
	unsigned char* e = (unsigned char*)w->list[Count(w) - 1];
	__try
	{
		*offset = OFF_CI_NI_INSTANCE;
		const unsigned char* inst = *(const unsigned char* const*)(e + OFF_CI_NI_INSTANCE);
		*offset = OFF_CI_NMI_RUNTIME_ID;
		volatile int id = *(const int*)(inst + OFF_CI_NMI_RUNTIME_ID);
		(void)id;
		return false;
	}
	__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		return true;
	}
}

static void QueueN(World* w)
{
	w->list[0] = w->n;
	SetCount(w, 1);
}

// n's instance already in the world at slot 56, n not queued.
static void InitRegistered(World* w)
{
	Init(w);
	*(unsigned int*)(w->instance + OFF_NMI_SECTION_UID) = 0x390a19u;
	*(int*)(w->instance + OFF_CI_NMI_RUNTIME_ID) = 56;
	w->slotInstance[56] = w->instance;
	w->slotMediator[56] = w->mediator;
	SetCount(w, 0);
}

// The add-list drain: an entry whose uid is already in the world is dropped
// as a duplicate, keeping the instance createInstance gave it; any other
// entry's instance takes the first free slot.
static void DrainAddList(World* w)
{
	for (unsigned int i = 0; i < Count(w); ++i)
	{
		unsigned char* e = (unsigned char*)w->list[i];
		unsigned char* inst = *(unsigned char**)(e + OFF_CI_NI_INSTANCE);
		const unsigned int uid = *(unsigned int*)(e + OFF_CI_NI_UID);
		int freeSlot = -1;
		bool dup = false;
		for (int s = 0; s < SLOTS; ++s)
		{
			if (!w->slotInstance[s]) { if (freeSlot < 0) freeSlot = s; continue; }
			if (*(unsigned int*)((unsigned char*)w->slotInstance[s] + OFF_NMI_SECTION_UID) == uid)
				dup = true;
		}
		if (dup || !inst || freeSlot < 0)
			continue;
		*(unsigned int*)(inst + OFF_NMI_SECTION_UID) = uid;
		*(int*)(inst + OFF_CI_NMI_RUNTIME_ID) = freeSlot;
		w->slotInstance[freeSlot] = inst;
		w->slotMediator[freeSlot] = *(void**)(e + OFF_NI_MEDIATOR);
	}
	SetCount(w, 0);
}

// The parent's teardown: the instance leaves the world only when its runtime
// id names a slot; then the mediator is deleted outright.
static void Teardown(World* w)
{
	unsigned char* inst = *(unsigned char**)(w->n + OFF_CI_NI_INSTANCE);
	if (inst)
	{
		const int id = *(int*)(inst + OFF_CI_NMI_RUNTIME_ID);
		if (id >= 0 && id < SLOTS)
		{
			w->slotInstance[id] = 0;
			w->slotMediator[id] = 0;
			*(int*)(inst + OFF_CI_NMI_RUNTIME_ID) = -1;
		}
		*(void**)(w->n + OFF_CI_NI_INSTANCE) = 0;
	}
	VirtualFree(w->mediator, 4096, MEM_DECOMMIT);
}

// A path query over every occupied slot: one load through each mediator.
static bool QueryFaults(World* w, int* slot)
{
	for (int s = 0; s < SLOTS; ++s)
	{
		if (!w->slotInstance[s])
			continue;
		__try
		{
			const void* volatile vt = *(const void* const*)w->slotMediator[s];
			(void)vt;
		}
		__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
		{
			*slot = s;
			return true;
		}
	}
	return false;
}

int main()
{
	static World w;
	s_world = &w;

	// 1. Vanilla over n already queued with a live instance.
	Init(&w); QueueN(&w);
	s_origCalls = 0;
	VanillaCreateInstance(w.navMesh, w.n);
	size_t off = 0;
	Check(Count(&w) == 1 && w.list[0] == w.n, "vanilla re-queues the pointer it freed");
	Check(DrainFaults(&w, &off), "vanilla: the drain faults through the freed entry");
	printf("vanilla: drain faulted at +0x%X through the re-queued entry\n", (unsigned)off);

	// 2. Act mode over the same list.
	Init(&w); QueueN(&w);
	s_origCalls = 0;
	CreateInstanceArm arm = GuardedCall(&w, w.n, true, VanillaCreateInstance);
	Check(arm == CI_ARM_SELF_LIVE, "act: classified self-duplicate");
	Check(s_origCalls == 0, "act: the original is not called");
	Check(Count(&w) == 1 && w.list[0] == w.n, "act: n stays queued once");
	Check(*(void**)(w.n + OFF_CI_NI_INSTANCE) == w.instance, "act: n keeps its live instance");
	Check(!DrainFaults(&w, &off), "act: the drain reads n cleanly");

	// 3. Observe mode: counted, then passed unchanged -- the fault stays.
	Init(&w); QueueN(&w);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, false, VanillaCreateInstance);
	Check(arm == CI_ARM_SELF_LIVE, "observe: classified self-duplicate");
	Check(s_origCalls == 1 && s_origNavMesh == w.navMesh && s_origN == w.n,
	      "observe: the original is called once with the caller's arguments");
	Check(DrainFaults(&w, &off), "observe: the drain still faults");

	// 4. A new instance, alone and next to a same-uid other object.
	Init(&w);
	*(void**)(w.n + OFF_CI_NI_INSTANCE) = 0;
	SetCount(&w, 0);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, true, VanillaCreateInstance);
	Check(arm == CI_ARM_NEW && s_origCalls == 1, "act: a new instance reaches the original");
	Check(Count(&w) == 1 && w.list[0] == w.n && !DrainFaults(&w, &off), "act: the new instance is queued and readable");

	Init(&w);
	*(void**)(w.n + OFF_CI_NI_INSTANCE) = 0;
	*(void**)(w.other + OFF_CI_NI_INSTANCE) = w.instance;
	w.list[0] = w.other; SetCount(&w, 1);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, true, VanillaCreateInstance);
	Check(arm == CI_ARM_NEW && s_origCalls == 1, "act: a same-uid other object still reaches the original");
	Check(Count(&w) == 1 && w.list[0] == w.n && !DrainFaults(&w, &off), "act: vanilla replaces the other object with n");

	// 5. n not queued, its instance already in the world: vanilla.
	InitRegistered(&w);
	s_origCalls = 0;
	VanillaCreateInstance(w.navMesh, w.n);
	DrainAddList(&w);
	Check(w.slotInstance[56] == w.instance && *(void**)(w.n + OFF_CI_NI_INSTANCE) == w.fresh,
	      "registered vanilla: the drain refuses n and the slot keeps the old instance");
	Teardown(&w);
	int slot = -1;
	Check(QueryFaults(&w, &slot) && slot == 56, "registered vanilla: a query through the orphaned slot faults");
	printf("vanilla: slot %d query faulted through the deleted mediator\n", slot);

	// Act mode over the same state.
	InitRegistered(&w);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, true, VanillaCreateInstance);
	Check(arm == CI_ARM_LIVE_REGISTERED, "registered act: classified registered");
	Check(s_origCalls == 0, "registered act: the original is not called");
	Check(*(void**)(w.n + OFF_CI_NI_INSTANCE) == w.instance && Count(&w) == 0,
	      "registered act: n keeps its instance and is not queued");
	DrainAddList(&w);
	Teardown(&w);
	Check(w.slotInstance[56] == 0, "registered act: the teardown takes the instance out of the world");
	Check(!QueryFaults(&w, &slot), "registered act: no slot reaches the deleted mediator");

	// Observe mode: counted, then passed unchanged -- the fault stays.
	InitRegistered(&w);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, false, VanillaCreateInstance);
	Check(arm == CI_ARM_LIVE_REGISTERED, "registered observe: classified registered");
	Check(s_origCalls == 1 && s_origNavMesh == w.navMesh && s_origN == w.n,
	      "registered observe: the original is called once with the caller's arguments");
	DrainAddList(&w);
	Teardown(&w);
	Check(QueryFaults(&w, &slot), "registered observe: the query still faults");

	// 6. An instance not in the world, n not queued.
	Init(&w);
	SetCount(&w, 0);
	s_origCalls = 0;
	arm = GuardedCall(&w, w.n, true, VanillaCreateInstance);
	Check(arm == CI_ARM_NEW && s_origCalls == 1 && Count(&w) == 1 && w.list[0] == w.n
	      && *(void**)(w.n + OFF_CI_NI_INSTANCE) == w.fresh,
	      "unregistered outside: the original runs and queues n");

	return CheckExit("create_instance_injection");
}
