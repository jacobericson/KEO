// nm_adjacency_observer.cpp - claim end and drain observer.
// Worker/bg claim end takes queue +152 then unlocks before wake; the NavMesh bg
// drain hook calls the original first, then takes +152 before done.mutex.
#include "navmesh/scheduling/nm_adjacency_internal.h"
using namespace nm_adjacency_detail;
namespace nm_adjacency_detail {
static const int      kDoneCap    = 512;
// ---------------------------------------------------------------------------
// Job end
// ---------------------------------------------------------------------------

static void OwnEnd(bool publish)
{
	const int idx = t_own;
	const unsigned __int64 task = t_ownTask;
	t_own = -1;
	t_ownTask = 0;
	uintptr_t nmg = g_navMeshGen;
	if (idx < 0 || !nmg)
		return;
	LockQueue(nmg);
	WBegin();
	// A registry reset could have reused the slot; act only on our own entry.
	if (g_reg.e[idx].state == NMADJ_CLAIMED && g_reg.e[idx].task == task)
	{
		if (publish && s_mode != MODE_COUNT_NO_OBSERVER)
			NmAdjPublish(&g_reg, idx);
		else
			NmAdjFree(&g_reg, idx);
	}
	WEnd();
	UnlockQueue(nmg);
	Wake();
}

} // namespace nm_adjacency_detail
void NmAdjOwnFinished() { OwnEnd(true); }
void NmAdjOwnDropped()  { OwnEnd(false); }

// ---------------------------------------------------------------------------
// Drain observer
// ---------------------------------------------------------------------------

namespace nm_adjacency_detail {
nmgUpdate_t orig_nmgUpdate = NULL;

static void ObserveDrain(uintptr_t nmg)
{
	if (s_mode == MODE_OFF || s_mode == MODE_COUNT_NO_OBSERVER || !Read(&g_pubHint))
		return;
	if (nmg != g_navMeshGen)
	{
		InterlockedIncrement(&s_obsOtherNmg);
		return;
	}

	unsigned __int64 done[kDoneCap];
	int n = 0;
	bool overflow = false;

	LockQueue(nmg);
	// Every publish happened under this lock, after its push to done had
	// returned; a task absent from done now has been popped by the drain.
	const long seq = g_reg.pubSeq;
	{
		char initBuf[16];
		void* t = fn_pathBuilderInit(initBuf);
		fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_done_mutex, 200)), t);
		for (uintptr_t node = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_done_front, 184));
		     node; node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96)))
		{
			if (n == kDoneCap) { overflow = true; break; }
			done[n++] = (unsigned __int64)node;
		}
		fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_done_mutex, 200)));
	}
	int freed = 0;
	if (!overflow)
	{
		WBegin();
		freed = NmAdjReleaseDrained(&g_reg, seq, done, n);
		WEnd();
	}
	UnlockQueue(nmg);

	if (overflow)
		InterlockedIncrement(&s_obsOverflow);
	if (freed)
	{
		InterlockedExchangeAdd(&s_obsFreed, freed);
		Wake();
	}
}

void hook_nmgUpdate(void* nmg)
{
	InterlockedIncrement(&s_calls);
	orig_nmgUpdate(nmg);
	// The original has returned; this thread holds nothing.
	ObserveDrain((uintptr_t)nmg);
}

} // namespace nm_adjacency_detail
