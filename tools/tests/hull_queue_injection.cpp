// Drives the shipped hull destroy-queue policy over a fabricated
// PhysicsInterface (the two MessageChains at their game offsets) and objects
// that each live on their own page, and shows:
//
//   1. vanilla: an object queued, flushed, queued again by updateUT's own
//      per-hull update, deleted by the physics batch -- and the next batch
//      calls its deleting destructor through the freed page and faults;
//   2. act mode (the detour's two passes): the second queueing is dropped,
//      the object is deleted once and nothing faults;
//   3. observe mode: the same duplicate is counted and the fault stays;
//   4. a second queueing made while the physics thread runs is dropped a
//      frame later, before the flush hands it over;
//   5. a push after drain of the same address, for a new object made there,
//      is accepted and that object is deleted once; distinct hulls pass.
//
// Links src/fixes/physx/hull_queue_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/physx/hull_queue_guard_policy.h"

#include "check.h"

// ---- The fabricated PhysicsInterface ----------------------------------------

static unsigned char g_phys[0x240];
static uintptr_t g_makeMain[64], g_destroyMain[64], g_destroyBack[64];

static unsigned& Count(size_t off) { return *(unsigned*)(g_phys + off); }

static void ResetPhys()
{
	memset(g_phys, 0, sizeof(g_phys));
	*(uintptr_t**)(g_phys + OFF_HQG_MAKE_MAIN_DATA)     = g_makeMain;
	*(uintptr_t**)(g_phys + OFF_HQG_DESTROY_MAIN_DATA)  = g_destroyMain;
	*(uintptr_t**)(g_phys + OFF_HQG_DESTROY_BACK_DATA)  = g_destroyBack;
}

static void PushDestroy(void* o) { g_destroyMain[Count(OFF_HQG_DESTROY_MAIN_COUNT)++] = (uintptr_t)o; }
static void PushMake(void* o)    { g_makeMain[Count(OFF_HQG_MAKE_MAIN_COUNT)++] = (uintptr_t)o; }

// MessageChain::flush for both chains, as updateUT opens.
static void FlushChains()
{
	Count(OFF_HQG_MAKE_MAIN_COUNT) = 0;
	unsigned& nb = Count(OFF_HQG_DESTROY_BACK_COUNT);
	unsigned& nm = Count(OFF_HQG_DESTROY_MAIN_COUNT);
	for (unsigned i = 0; i < nm; ++i)
		g_destroyBack[nb++] = g_destroyMain[i];
	nm = 0;
}

// ---- Objects on their own pages ----------------------------------------------

static int g_deletes = 0;
typedef void (*DeletingDtor)(void* self, unsigned flags);
static void DtorImpl(void* self, unsigned) { ++g_deletes; VirtualFree(self, 4096, MEM_DECOMMIT); }
static DeletingDtor g_vtable[1] = { &DtorImpl };

static void* Construct(void* page)
{
	VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE);
	*(DeletingDtor**)page = g_vtable;
	return page;
}

static void* NewObject()
{
	return Construct(VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_READWRITE));
}

// threadJunkPreBT's delete loop: `if (p) p->vt[0](p, 1)`, then the count is
// zeroed and the buffer left. True when a call faulted.
static bool PhysicsBatch()
{
	bool faulted = false;
	unsigned& nb = Count(OFF_HQG_DESTROY_BACK_COUNT);
	for (unsigned i = 0; i < nb; ++i)
	{
		void* p = (void*)g_destroyBack[i];
		if (!p)
			continue;
		__try
		{
			(**(DeletingDtor**)p)(p, 1);
		}
		__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
		{
			faulted = true;
		}
	}
	nb = 0;
	return faulted;
}

// ---- The detour, over the shipped policy ---------------------------------------

static HqgSlot  g_slots[1u << 12];
static HqgState g_state;
static unsigned g_dups = 0;

struct NullCtx { const uintptr_t* data; };

static bool NullEntry(void* ctx, unsigned index, uintptr_t expected)
{
	const NullCtx* c = (const NullCtx*)ctx;
	uintptr_t* data = *(uintptr_t**)(g_phys + OFF_HQG_DESTROY_MAIN_DATA);
	if (data != c->data || index >= Count(OFF_HQG_DESTROY_MAIN_COUNT))
		return false;
	return InterlockedCompareExchange64((volatile LONG64*)&data[index], 0, (LONG64)expected) == (LONG64)expected;
}

static void Pass(bool act)
{
	HqgLists in;
	in.destroyMainCount = Count(OFF_HQG_DESTROY_MAIN_COUNT);
	in.destroyMain      = *(uintptr_t**)(g_phys + OFF_HQG_DESTROY_MAIN_DATA);
	in.destroyBackCount = Count(OFF_HQG_DESTROY_BACK_COUNT);
	in.destroyBack      = *(uintptr_t**)(g_phys + OFF_HQG_DESTROY_BACK_DATA);
	in.makeMainCount    = Count(OFF_HQG_MAKE_MAIN_COUNT);
	in.makeMain         = *(uintptr_t**)(g_phys + OFF_HQG_MAKE_MAIN_DATA);
	NullCtx ctx = { in.destroyMain };
	HqgResult r = HqgJudge(&g_state, &in, act, &NullEntry, &ctx, 0, 0);
	g_dups += r.dups;
}

// updateUT: the pre-flush pass, the flush, the record, then whatever the
// per-hull update queues (lateQueue), then the post-flush pass.
static void UpdateUT(bool guard, bool act, void* lateQueue)
{
	if (guard)
		Pass(act);
	FlushChains();
	if (guard)
		HqgRecord(&g_state, g_destroyBack, Count(OFF_HQG_DESTROY_BACK_COUNT));
	if (lateQueue)
		PushDestroy(lateQueue);
	if (guard)
		Pass(act);
}

static void Fresh()
{
	ResetPhys();
	HqgInit(&g_state, g_slots, 1u << 12);
	g_deletes = 0;
	g_dups = 0;
}

// The recorded route: the entity's destroy queues the object, then its own
// update, inside updateUT after the flush, queues it again.
static bool RunRecordedRoute(bool guard, bool act)
{
	Fresh();
	void* scythe = NewObject();
	void* other = NewObject();
	PushDestroy(scythe);
	PushDestroy(other);
	UpdateUT(guard, act, scythe);
	bool f1 = PhysicsBatch();
	UpdateUT(guard, act, 0);
	bool f2 = PhysicsBatch();
	return f1 || f2;
}

int main()
{
	// 1. Vanilla.
	bool faulted = RunRecordedRoute(false, false);
	Check(faulted, "vanilla: the second batch faults through the freed object");
	Check(g_deletes == 2, "vanilla: both objects deleted once before the fault");
	printf("vanilla: second batch faulted calling the destructor of a freed object\n");

	// 2. Act mode.
	faulted = RunRecordedRoute(true, true);
	Check(!faulted, "act: nothing faults");
	Check(g_deletes == 2 && g_dups == 1, "act: the double push is rejected, each object deleted once");

	// 3. Observe mode.
	faulted = RunRecordedRoute(true, false);
	Check(faulted && g_dups >= 1, "observe: counted, and the fault stays");

	// 4. A second queueing while the physics thread runs, before the delete.
	Fresh();
	void* a = NewObject();
	PushDestroy(a);
	UpdateUT(true, true, 0);
	PushDestroy(a);                  // main thread, the batch not yet deleted
	faulted = PhysicsBatch();
	UpdateUT(true, true, 0);
	faulted = PhysicsBatch() || faulted;
	Check(!faulted && g_deletes == 1 && g_dups == 1, "act: a physics-time double push is rejected a frame later");

	// 5. Push after drain of the same address, and distinct hulls.
	Fresh();
	void* b = NewObject();
	PushDestroy(b);
	UpdateUT(true, true, 0);
	Check(!PhysicsBatch() && g_deletes == 1, "the first object is deleted");
	void* again = Construct(b);      // a new object at the freed address
	Check(again == b, "the address is reused");
	PushMake(again);
	PushDestroy(again);
	void* c = NewObject();
	void* d = NewObject();
	PushDestroy(c);
	PushDestroy(d);
	UpdateUT(true, true, 0);
	faulted = PhysicsBatch();
	Check(!faulted && g_deletes == 4 && g_dups == 0, "act: a push after drain of a re-made address is accepted, distinct hulls pass");

	return CheckExit("hull_queue_injection");
}
