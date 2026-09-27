// hulls_detours.cpp - Hull queue push, destruction and batch detours.
// Runs on main, physics and other caller threads; no allocation, lock or logging.

#include "hulls_detail.h"
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace audit {
namespace audithulls_detail {

// ---- Detours ----------------------------------------------------------------

template <int K> void* PushDetour(void* self)
{
	if (g_on)
		OnPush(K, (uintptr_t)self, (uintptr_t)_ReturnAddress());
	return oPush[K](self);
}

template <int K> void* DtorDetour(void* self, unsigned deleteFlags)
{
	if (g_on)
		OnDtor(EV_DTOR_HULL + K, (uintptr_t)self, (uintptr_t)_ReturnAddress(), deleteFlags);
	return oDtor[K](self, deleteFlags);
}

extern const Push_t PUSH_DETOURS[VPUSH_KINDS] =
{
	&PushDetour<0>, &PushDetour<1>, &PushDetour<2>, &PushDetour<3>, &PushDetour<4>
};
extern const Dtor_t DTOR_DETOURS[DTOR_KINDS] =
{
	&DtorDetour<0>, &DtorDetour<1>, &DtorDetour<2>, &DtorDetour<3>,
	&DtorDetour<4>, &DtorDetour<5>, &DtorDetour<6>, &DtorDetour<7>
};

// The release pushes inline. Entries it appended that a hooked pusher did not
// already report during the call are attributed to its caller.
void ReleaseDetour(void* owner)
{
	if (!g_on)
	{
		oRelease(owner);
		return;
	}
	uintptr_t ret = (uintptr_t)_ReturnAddress();
	uintptr_t phys = Physics();
	LONG64 t0 = Now();
	unsigned before = phys ? *(const volatile unsigned*)(phys + PI_DESTROY_MAIN_COUNT) : 0;
	oRelease(owner);
	if (!phys)
		return;
	unsigned after = *(const volatile unsigned*)(phys + PI_DESTROY_MAIN_COUNT);
	const uintptr_t* data = *(const uintptr_t* const*)(phys + PI_DESTROY_MAIN_DATA);
	if (after <= before || !PlausiblePtr((uintptr_t)data))
		return;
	for (unsigned i = before; i < after; ++i)
	{
		uintptr_t p = ListEntry(data, i);
		if (!p)
			continue;
		Slot* s = g_table.Find(p, false);
		if (s && StateOf(s->word) == ST_QMAIN && s->pushQpc >= t0)
			continue;
		OnPush(EV_PUSH_UNLOAD, p, ret);
	}
}

// Marks the batch before the delete loop and settles it after. The only
// writer of the back-thread list is the flush in updateUT, which the main
// thread calls only while the physics thread is not busy, so the list and its
// buffer are stable for the whole call.
void JunkDetour(void* self)
{
	if (!g_on)
	{
		oJunk(self);
		return;
	}
	uintptr_t phys = (uintptr_t)self;
	DWORD tid = GetCurrentThreadId();
	g_physTid = tid;
	unsigned n = *(const volatile unsigned*)(phys + PI_DESTROY_BACK_COUNT);
	const uintptr_t* data = *(const uintptr_t* const*)(phys + PI_DESTROY_BACK_DATA);
	if (!PlausiblePtr((uintptr_t)data))
		n = 0;
	if (n)
	{
		HullRec b = Ev(EV_BATCH, n, 0, tid, 0, 0);
		Emit(b);
		InterlockedIncrement(&g_batches);
		InterlockedExchangeAdd64(&g_batchEntries, n);
		if ((LONG)n > g_batchMax)
			g_batchMax = (LONG)n;
		for (unsigned i = 0; i < n; ++i)
		{
			uintptr_t p = data[i];
			if (!p)
				continue;
			Result r = g_table.BatchEntry(p);
			if (r.anomaly != AN_NONE)
			{
				HullRec ev = Ev(EV_BATCH_ENTRY, p, 0, tid, 0, VtRva(p));
				Emit(ev);
				Flag(r.anomaly, ev, r);
			}
		}
	}
	g_junkData  = data;
	g_junkCount = n;
	g_inJunk    = 1;
	oJunk(self);
	g_inJunk    = 0;
	for (unsigned i = 0; i < n; ++i)
		if (data[i])
			g_table.BatchExit(data[i]);
}

} // namespace
using namespace audithulls_detail;

} // namespace audit
