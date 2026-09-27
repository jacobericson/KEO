// PhysX hull destroy-queue diagnostic ([Audit] HullDiag, default off).
//
// PhysicsInterface::hullsToDestroy is a MessageChain with no lock. The main
// thread appends to its main-thread list, PhysicsActual::updateUT flushes that
// list into the back-thread list, and the physics thread's threadJunkPreBT
// deletes every back-thread entry through vtable slot 0 and clears the list.
// This file follows each hull address through that path in a lifecycle table
// (AuditHullsTable.h) and flags the misuses that can hand the physics thread a
// freed hull.
//
// Every push passes the scan just before the flush, whoever made it; the five
// virtual pushers and the zone-release helper are hooked as well, for the
// caller and the thread. Many game functions push inline, and those are known
// only by the scan (caller unknown, vtable recorded).
//
// Detours run on the main thread, the physics thread and any other caller:
// no allocation, lock or logging in them. The main thread prints.

#include "game/klib_members.h"
#include "KenshiFrameAudit_internal.h"
#include "AuditHullsTable.h"
#include "hulls_detail.h"
#include <intrin.h>

namespace audit {
namespace audithulls_detail {

const char* EVENT_NAMES[EV_COUNT] =
{
	"pushHull", "pushEntity", "pushScythe", "pushRoot", "pushBase", "pushUnload", "pushInline",
	"dtorHull", "dtorSimple", "dtorBox", "dtorCapsule", "dtorDoor", "dtorScythe", "dtorRoot", "dtorRagdoll",
	"flush", "batch", "batchEntry", "make"
};

const char* STATE_NAMES[ST_COUNT] = { "none", "made", "qmain", "qback", "destroying", "dead" };

const char* ANOMALY_NAMES[AN_COUNT] =
{
	"dupPush", "pushAfterDtor", "offMainPush", "dtorUnqueued", "dtorQueued", "batchUnqueued", "makeQueued"
};


} // namespace audithulls_detail
namespace audithulls_detail {

volatile LONG   g_on = 0;
Table           g_table;
uintptr_t       g_exeBase = 0, g_exeEnd = 0, g_gameWorld = 0, g_consumerRet = 0;
volatile DWORD  g_physTid = 0;

HullRec         g_ring[RING];
volatile LONG64 g_ringHead = 0;

// One ring copy per anomaly kind, taken at that kind's first anomaly on the
// detecting thread, so a routine kind cannot use up the copy another needs.
HullRec         g_snap[AN_COUNT][SNAP];
volatile LONG   g_snapState[AN_COUNT];   // 0 none, 1 copying, 2 ready, 3 printed
LONG64          g_snapEnd[AN_COUNT];

AnomRec         g_anom[AN_COUNT][ANOM_KEEP];
volatile LONG   g_anomCount[AN_COUNT];
int             g_anomPrinted[AN_COUNT];   // main thread

volatile LONG   g_push[PUSH_KINDS];
volatile LONG   g_dtor[DTOR_KINDS];
volatile LONG   g_dtorConsumer = 0, g_made = 0, g_flushes = 0, g_batches = 0;
LONG            g_batchMax = 0;            // physics thread writes
volatile LONG64 g_batchEntries = 0;

// The batch threadJunkPreBT is deleting, for the fault handler.
volatile LONG             g_inJunk = 0;
const uintptr_t* volatile g_junkData = NULL;
volatile unsigned         g_junkCount = 0;
volatile uintptr_t        g_lastConsumed = 0;   // last address a hooked destructor got from the loop
HANDLE                    g_crashFile = INVALID_HANDLE_VALUE;
volatile LONG             g_crashWritten = 0;
PVOID                     g_veh = NULL;

// Set while this module reads memory that may be freed, so a fault handler
// can tell a guarded read from a crash. (The optimizer's handler, first in
// the chain, records any first-chance access violation outside its own guard
// and cannot see this flag; see SafeField.)
__declspec(thread) int t_auditGuard = 0;

UnloadStamp g_unloads[UNLOADS];
int         g_unloadCount = 0;
LONGLONG    g_lastReport = 0;

inline unsigned ExeRva(uintptr_t a)
{
	if (!a)
		return 0;
	return a >= g_exeBase && a < g_exeEnd ? (unsigned)(a - g_exeBase) : 0xFFFFFFFFu;
}

// Reads a pointer-sized field of an object that may be freed; 0 on a fault.
// A fault here is first-chance: vectored handlers see it before this
// __except, and the optimizer's would record it as a crash. t_auditGuard marks
// the read for handlers that check it.
uintptr_t SafeField(uintptr_t obj, size_t off)
{
	if (!PlausiblePtr(obj))
		return 0;
	uintptr_t v = 0;
	t_auditGuard = 1;
	__try
	{
		v = *(const volatile uintptr_t*)(obj + off);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		v = 0;
	}
	t_auditGuard = 0;
	return v;
}


} // namespace audithulls_detail
namespace audithulls_detail {

unsigned VtRva(uintptr_t obj)
{
	uintptr_t vt = SafeField(obj, 0);
	return vt >= g_exeBase && vt < g_exeEnd ? (unsigned)(vt - g_exeBase) : 0;
}

uintptr_t Physics()
{
	uintptr_t p = *(const volatile uintptr_t*)(KLIB_MEMBER(5, g_gameWorld, GameWorld_physics, GW_PHYSICS));
	return PlausiblePtr(p) ? p : 0;
}

HullRec Ev(int kind, uintptr_t ptr, uintptr_t ret, DWORD tid, unsigned flags, unsigned vtRva)
{
	HullRec r;
	r.seq   = 0;
	r.qpc   = Now();
	r.ptr   = ptr;
	r.ret   = ret;
	r.tid   = tid;
	r.kind  = (unsigned short)kind;
	r.flags = (unsigned short)flags;
	r.vtRva = vtRva;
	r.pad   = 0;
	return r;
}

void Emit(HullRec& r)
{
	LONG64 i = InterlockedIncrement64(&g_ringHead) - 1;
	HullRec& slot = g_ring[i & (RING - 1)];
	slot.seq = 0;
	_WriteBarrier();
	slot.qpc   = r.qpc;
	slot.ptr   = r.ptr;
	slot.ret   = r.ret;
	slot.tid   = r.tid;
	slot.kind  = r.kind;
	slot.flags = r.flags;
	slot.vtRva = r.vtRva;
	_WriteBarrier();
	slot.seq = i + 1;
	r.seq = i + 1;
}

// Copies one ring record. False when the writer was mid-record or the slot
// was reused during the copy (seq differs before and after).
bool CopyRec(LONG64 i, HullRec* out)
{
	const HullRec& src = g_ring[i & (RING - 1)];
	LONG64 before = src.seq;
	_ReadBarrier();
	out->qpc   = src.qpc;
	out->ptr   = src.ptr;
	out->ret   = src.ret;
	out->tid   = src.tid;
	out->kind  = src.kind;
	out->flags = src.flags;
	out->vtRva = src.vtRva;
	_ReadBarrier();
	out->seq = before;
	return before == i + 1 && src.seq == before;
}

// Copies the ring's last SNAP records, once per anomaly kind.
void Snapshot(int an, LONG64 end)
{
	if (InterlockedCompareExchange(&g_snapState[an], 1, 0) != 0)
		return;
	for (int k = 0; k < SNAP; ++k)
	{
		LONG64 i = end - SNAP + k;
		if (i < 0 || !CopyRec(i, &g_snap[an][k]))
			g_snap[an][k].seq = 0;
	}
	g_snapEnd[an] = end;
	_WriteBarrier();
	InterlockedExchange(&g_snapState[an], 2);
}

void Flag(int an, const HullRec& ev, const Result& r)
{
	LONG n = InterlockedIncrement(&g_anomCount[an]);
	if (n <= ANOM_KEEP)
	{
		AnomRec& a = g_anom[an][n - 1];
		a.ev          = ev;
		a.prev        = r.prev;
		a.prevPushQpc = r.prevPushQpc;
		a.prevDtorQpc = r.prevDtorQpc;
		_WriteBarrier();
		InterlockedExchange(&a.ready, 1);
	}
	Snapshot(an, ev.seq);
}

void OnPush(int kind, uintptr_t obj, uintptr_t ret)
{
	DWORD tid = GetCurrentThreadId();
	unsigned flags = tid != g_mainThreadId ? F_OFFMAIN : 0;
	HullRec ev = Ev(kind, obj, ret, tid, flags, VtRva(obj));
	Emit(ev);
	InterlockedIncrement(&g_push[kind]);
	Result r = g_table.Push(obj, kind, ExeRva(ret), ev.qpc);
	if (flags & F_OFFMAIN)
		Flag(AN_OFF_MAIN_PUSH, ev, r);
	if (r.anomaly != AN_NONE)
		Flag(r.anomaly, ev, r);
}

void OnDtor(int kind, uintptr_t obj, uintptr_t ret, unsigned deleteFlags)
{
	DWORD tid = GetCurrentThreadId();
	bool consumer = ret == g_consumerRet;
	unsigned flags = (deleteFlags & 1) ? F_DELETE : 0;
	if (consumer)
		flags |= F_CONSUMER;
	if (tid != g_mainThreadId)
		flags |= F_OFFMAIN;
	if (kind >= EV_DTOR_SIMPLE && kind <= EV_DTOR_DOOR && SafeField(obj, ENTITY_MOVABLE) != 0)
		flags |= F_MOVABLE;
	HullRec ev = Ev(kind, obj, ret, tid, flags, VtRva(obj));
	Emit(ev);
	InterlockedIncrement(&g_dtor[kind - EV_DTOR_HULL]);
	if (consumer)
	{
		InterlockedIncrement(&g_dtorConsumer);
		g_lastConsumed = obj;
	}
	Result r = g_table.Dtor(obj, consumer, ev.qpc);
	if (r.anomaly != AN_NONE)
		Flag(r.anomaly, ev, r);
}

Push_t    oPush[VPUSH_KINDS];
Dtor_t    oDtor[DTOR_KINDS];
Junk_t    oJunk = NULL;
Release_t oRelease = NULL;

} // namespace
using namespace audithulls_detail;

void Hulls_BeforeUpdateUT(void* physics)
{
	if (!g_on)
		return;
	uintptr_t phys = (uintptr_t)physics;
	DWORD tid = GetCurrentThreadId();

	// Both lists are read entry by entry under SEH: an off-main push racing
	// this scan could reallocate them.
	// hullsToMake is flushed first: a new hull at a reused address.
	unsigned nm = *(const volatile unsigned*)(phys + PI_MAKE_MAIN_COUNT);
	const uintptr_t* made = *(const uintptr_t* const*)(phys + PI_MAKE_MAIN_DATA);
	if (PlausiblePtr((uintptr_t)made))
	{
		for (unsigned i = 0; i < nm; ++i)
		{
			uintptr_t p = ListEntry(made, i);
			if (!p)
				continue;
			InterlockedIncrement(&g_made);
			Result r = g_table.Make(p);
			if (r.anomaly != AN_NONE)
			{
				HullRec ev = Ev(EV_MAKE, p, 0, tid, 0, VtRva(p));
				Emit(ev);
				Flag(r.anomaly, ev, r);
			}
		}
	}

	unsigned nd = *(const volatile unsigned*)(phys + PI_DESTROY_MAIN_COUNT);
	const uintptr_t* data = *(const uintptr_t* const*)(phys + PI_DESTROY_MAIN_DATA);
	if (!nd || !PlausiblePtr((uintptr_t)data))
		return;
	HullRec f = Ev(EV_FLUSH, nd, 0, tid, 0, 0);
	Emit(f);
	InterlockedIncrement(&g_flushes);
	for (unsigned i = 0; i < nd; ++i)
	{
		uintptr_t p = ListEntry(data, i);
		if (!p)
			continue;
		Result r = g_table.FlushEntry(p, EV_PUSH_INLINE, f.qpc);
		if (!r.inlinePush && r.anomaly == AN_NONE)
			continue;
		HullRec ev = Ev(EV_PUSH_INLINE, p, 0, tid, 0, VtRva(p));
		Emit(ev);
		if (r.inlinePush)
			InterlockedIncrement(&g_push[EV_PUSH_INLINE]);
		if (r.anomaly != AN_NONE)
			Flag(r.anomaly, ev, r);
	}
}

void Hulls_NoteZoneUnload(int x, int y)
{
	if (!g_on)
		return;
	UnloadStamp& u = g_unloads[g_unloadCount % UNLOADS];
	u.qpc = Now();
	u.x = x;
	u.y = y;
	++g_unloadCount;
}

void Hulls_OnFrameStarted()
{
	if (!g_on)
		return;
	PrintAnomalies();
	for (int k = 0; k < AN_COUNT; ++k)
	{
		if (g_snapState[k] == 2)
		{
			PrintSnapshot(k);
			g_snapState[k] = 3;
		}
	}
	LONGLONG now = Now();
	if (now - g_lastReport < g_qpcFreq * 5)
		return;
	g_lastReport = now;
	PrintStats();
}

} // namespace audit
