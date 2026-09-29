#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/physx/hull_queue_guard.h"
#include "fixes/physx/hull_queue_guard_policy.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <intrin.h>
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

#pragma intrinsic(_ReturnAddress)

KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToMake_mainThreadData_count, OFF_HQG_MAKE_MAIN_COUNT);
KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToDestroy_mainThreadData_count, OFF_HQG_DESTROY_MAIN_COUNT);
KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToDestroy, OFF_HQG_DESTROY_MAIN_COUNT - 0x10);

// updateUT is called from GameWorld::mainLoop_GPUSensitiveStuff only after
// ThreadClass::isRunning reports the physics thread idle, and the thread is
// started again after it returns. The judge, the drops and the record below
// all run inside that window on the main thread; nothing here takes a lock
// or allocates.
//
// 2*calls == empty + judged + skipState + skipList + overflow + raced (two passes a call)
// dups    == inList + pending + freed == rejected + observed (+ an unfinished raced pass)
// postFlush: the dups the second pass found. In observe mode an entry that
// pass leaves in place is judged again (as freed) by the next first pass, so
// one duplicate can count twice there.
static volatile LONG s_calls     = 0;
static volatile LONG s_empty     = 0;
static volatile LONG s_judged    = 0;
static volatile LONG s_skipState = 0;
static volatile LONG s_skipList  = 0;
static volatile LONG s_overflow  = 0;
static volatile LONG s_raced     = 0;
static volatile LONG s_scanned   = 0;
static volatile LONG s_dups      = 0;
static volatile LONG s_inList    = 0;
static volatile LONG s_pending   = 0;
static volatile LONG s_freed     = 0;
static volatile LONG s_rejected  = 0;   // dropped (guard mode)
static volatile LONG s_observed  = 0;   // would have been dropped (observe mode)
static volatile LONG s_keptRemade = 0;  // in the deleted batch but made again: kept
static volatile LONG s_postFlush = 0;   // dups queued by updateUT after its own flush
static volatile LONG s_maxQueue  = 0;   // longest main list at a flush
static volatile LONG s_maxBatch  = 0;   // longest batch known deleted
static volatile LONG s_pushes    = 0;   // calls into the five hooked pushers
static volatile LONG s_fireLines = 0;
static const LONG kMaxFireLines  = 32;
static const unsigned kFindings  = 8;

// 0 = not attempted, 1 = installed, -1 = refused.
static int s_state = 0;
static const char* s_why = "";
static int s_pushersInstalled = 0;
static double s_lastBeat = -1.0;
static const double kBeatSeconds = 60.0;

// Read once at install. true drops a duplicate; false counts it and leaves
// the list unchanged.
static bool s_actMode = true;

// Sized for the longest batches measured (a few thousand hulls) with room to
// spare; a frame that needs more is counted as overflow and not judged.
static const unsigned kSlots = 1u << 17;
static HqgSlot  s_slots[kSlots];
static HqgState s_judge;

static volatile LONG s_flushSeq = 0;
static uintptr_t s_recPhys = 0;

// Any pusher thread publishes the last two callers of an address with
// independent Interlocked key/caller/flush updates. The flush-thread reporter
// reads with a key check before/after, not a coherent sequence copy. Mixed
// fields during overwrite are tolerated attribution diagnostics. Static
// zero initialization only; direct-mapped slots overwrite without a reset.
namespace hull_queue_guard_detail {
struct PushRec
{
	volatile LONG64 key;
	volatile LONG64 callers;
	volatile LONG   flushSeq;
};
} // namespace hull_queue_guard_detail
using namespace hull_queue_guard_detail;
static const unsigned kPushRecs = 1u << 14;
static PushRec s_pushRecs[kPushRecs];

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static void RaiseMax(volatile LONG* p, LONG v)
{
	for (;;)
	{
		LONG cur = Read(p);
		if (v <= cur || InterlockedCompareExchange(p, v, cur) == cur)
			return;
	}
}

static unsigned PushSlot(uintptr_t p)
{
	return (unsigned)((((unsigned long long)p >> 3) * 0x9E3779B97F4A7C15ull) >> 50) & (kPushRecs - 1);
}

static uintptr_t s_gameEnd = 0;

static unsigned ExeRva(uintptr_t a)
{
	return a >= gameBase && a < s_gameEnd ? (unsigned)(a - gameBase) : 0xFFFFFFFFu;
}

static void NotePush(uintptr_t p, uintptr_t ret)
{
	InterlockedIncrement(&s_pushes);
	if (!p)
		return;
	PushRec* r = &s_pushRecs[PushSlot(p)];
	unsigned rva = ExeRva(ret);
	const LONG seq = Read(&s_flushSeq);
	// A record more than one flush old belongs to an earlier object at this
	// address; its callers are not this object's.
	if (r->key == (LONG64)p && seq - r->flushSeq <= 1)
	{
		LONG64 old, nw;
		do
		{
			old = r->callers;
			nw = (LONG64)HqgPushCallers((unsigned __int64)old, rva);
		} while (InterlockedCompareExchange64(&r->callers, nw, old) != old);
	}
	else
	{
		InterlockedExchange64(&r->key, 0);
		InterlockedExchange64(&r->callers, (LONG64)HqgPushCallers(0, rva));
		InterlockedExchange64(&r->key, (LONG64)p);
	}
	InterlockedExchange(&r->flushSeq, seq);
}

static bool LookupPush(uintptr_t p, unsigned __int64* callers, LONG* seq)
{
	PushRec* r = &s_pushRecs[PushSlot(p)];
	if (r->key != (LONG64)p)
		return false;
	*callers = (unsigned __int64)r->callers;
	*seq = r->flushSeq;
	return r->key == (LONG64)p;
}

// ---- The five pushers --------------------------------------------------------

namespace hull_queue_guard_detail {
typedef void* (*Push_t)(void*);
} // namespace hull_queue_guard_detail
using namespace hull_queue_guard_detail;
static const int kPushers = 5;
static Push_t s_origPush[kPushers];

template <int K> static void* PushDetour(void* self)
{
	NotePush((uintptr_t)self, (uintptr_t)_ReturnAddress());
	return s_origPush[K](self);
}

static const Push_t kPushDetours[kPushers] =
{
	&PushDetour<0>, &PushDetour<1>, &PushDetour<2>, &PushDetour<3>, &PushDetour<4>
};

static const HookRowId kPushRows[kPushers] =
{
	HOOK_HULL_PUSH_HULL, HOOK_HULL_PUSH_ENTITY, HOOK_HULL_PUSH_SCYTHE, HOOK_HULL_PUSH_ROOT, HOOK_HULL_PUSH_BASE
};

// ---- Logging ----------------------------------------------------------------

static void FlbCaller(FixedLogBuf* o, unsigned rva)
{
	if (rva == 0xFFFFFFFFu)
		FlbStr(o, "outside-exe");
	else
	{
		FlbStr(o, "exe+");
		FlbHex(o, rva);
	}
}

static void EmitFireLine(const HqgFinding& f, unsigned count, const HqgResult& r, bool post, bool dropped)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "HullQueueGuard FIRED: ");
	FlbStr(&o, HqgDupName(f.kind));
	FlbStr(&o, post ? " pass=postFlush" : " pass=preFlush");
	FlbStr(&o, " ptr=");     FlbHex(&o, (unsigned __int64)f.ptr);
	FlbStr(&o, " index=");   FlbDec(&o, f.index);
	FlbStr(&o, "/");         FlbDec(&o, count);
	FlbStr(&o, " batch=");   FlbDec(&o, r.batchCount);
	FlbStr(&o, " pendingList="); FlbDec(&o, r.pendingCount);
	unsigned __int64 callers = 0;
	LONG seq = 0;
	if (LookupPush(f.ptr, &callers, &seq))
	{
		// The pushers see every queueing of an object through its vtable; an
		// inline push by the game reaches the list without passing them.
		FlbStr(&o, " push=");    FlbCaller(&o, HqgLastCaller(callers));
		FlbStr(&o, " prevPush=");
		if (HqgPrevCaller(callers))
			FlbCaller(&o, HqgPrevCaller(callers));
		else
			FlbChar(&o, '-');   // no other hooked push of this object
		FlbStr(&o, " pushAgeFlushes="); FlbDec(&o, Read(&s_flushSeq) - seq);
	}
	else
	{
		FlbStr(&o, " push=inline prevPush=?");
	}
	if (dropped)
		FlbStr(&o, "; dropped, the first queueing destroys it once. dups=");
	else
		FlbStr(&o, "; not acted on (observe mode). dups=");
	FlbDec(&o, Read(&s_dups));
	LogMsgDeferrable(FlbDone(&o));
}

static const GuardCounter kBeatRows[] =
{
	{ "calls",    GF_COUNT, &s_calls,    0 },
	{ "empty",    GF_COUNT, &s_empty,    0 },
	{ "judged",   GF_COUNT, &s_judged,   0 },
	{ "pushes",   GF_COUNT, &s_pushes,   0 },
	{ "scanned",  GF_COUNT, &s_scanned,  0 },
	{ "dups",     GF_COUNT, &s_dups,     0 },
	{ "rejected", GF_COUNT, &s_rejected, 0 },
	{ "observed", GF_COUNT, &s_observed, 0 },
	{ "maxQueue", GF_COUNT, &s_maxQueue, 0 },
	{ "maxBatch", GF_COUNT, &s_maxBatch, 0 },
};

static const GuardCounter kDetailRows[] =
{
	{ "inList",     GF_COUNT,    &s_inList,     0 },
	{ "pending",    GF_COUNT,    &s_pending,    0 },
	{ "freed",      GF_COUNT,    &s_freed,      0 },
	{ "keptRemade", GF_COUNT,    &s_keptRemade, 0 },
	{ "postFlush",  GF_COUNT,    &s_postFlush,  0 },
	{ "skipState",  GF_COUNT,    &s_skipState,  0 },
	{ "skipList",   GF_COUNT,    &s_skipList,   0 },
	{ "overflow",   GF_COUNT,    &s_overflow,   0 },
	{ "raced",      GF_COUNT,    &s_raced,      0 },
	{ "lines",      GF_COUNT_OF, &s_fireLines,  kMaxFireLines },
};

static void EmitHeartbeat()
{
	const bool live = s_state == 1;
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "HullQueueGuard running:");
	if (!live)
	{
		FlbStr(&o, " installed=no("); FlbStr(&o, s_why); FlbStr(&o, ")");
	}
	FlbStr(&o, " mode="); FlbStr(&o, s_actMode ? "guard" : "observe");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), live);
	LogMsgDeferrable(FlbDone(&o));

	GuardHeartbeatBegin(&o, "HullQueueGuard detail:");
	GuardFields(&o, kDetailRows, (int)ARRAYSIZE(kDetailRows), live);
	FlbStr(&o, " pushers=");
	if (live)
		FlbDec(&o, s_pushersInstalled);
	else
		FlbChar(&o, '?');
	FlbStr(&o, "/"); FlbDec(&o, kPushers);
	LogMsgDeferrable(FlbDone(&o));
}

void HullQueueGuardTick(double now)
{
	if (s_state == 0)
		return;
	if (now - s_lastBeat < kBeatSeconds)
		return;
	s_lastBeat = now;
	EmitHeartbeat();
}

// ---- updateUT ---------------------------------------------------------------

namespace hull_queue_guard_detail {
struct NullCtx
{
	uintptr_t        phys;
	const uintptr_t* data;   // the main buffer the judge read
};
} // namespace hull_queue_guard_detail
using namespace hull_queue_guard_detail;

// Re-reads the buffer before each store: a push from another thread could
// have grown and freed it since the judge read it.
static bool NullEntry(void* ctx, unsigned index, uintptr_t expected)
{
	const NullCtx* c = (const NullCtx*)ctx;
	uintptr_t* data = *(uintptr_t* volatile*)(c->phys + OFF_HQG_DESTROY_MAIN_DATA);
	unsigned count = *(volatile unsigned*)(c->phys + OFF_HQG_DESTROY_MAIN_COUNT);
	if ((const uintptr_t*)data != c->data || index >= count)
		return false;
	return InterlockedCompareExchange64((volatile LONG64*)&data[index], 0, (LONG64)expected) == (LONG64)expected;
}

namespace hull_queue_guard_detail {
typedef void (*updateUT_t)(void* physics);
} // namespace hull_queue_guard_detail
using namespace hull_queue_guard_detail;
static updateUT_t orig_updateUT = NULL;

namespace hull_queue_guard_detail {
struct Pass
{
	HqgResult  r;
	unsigned   count;
	HqgFinding found[kFindings];
};
} // namespace hull_queue_guard_detail
using namespace hull_queue_guard_detail;

static void RunPass(uintptr_t phys, bool post, Pass* p)
{
	HqgLists in;
	in.destroyMainCount = *(volatile unsigned*)(phys + OFF_HQG_DESTROY_MAIN_COUNT);
	in.destroyMain      = *(uintptr_t* volatile*)(phys + OFF_HQG_DESTROY_MAIN_DATA);
	in.destroyBackCount = *(volatile unsigned*)(phys + OFF_HQG_DESTROY_BACK_COUNT);
	in.destroyBack      = *(const uintptr_t* volatile*)(phys + OFF_HQG_DESTROY_BACK_DATA);
	in.makeMainCount    = *(volatile unsigned*)(phys + OFF_HQG_MAKE_MAIN_COUNT);
	in.makeMain         = *(const uintptr_t* volatile*)(phys + OFF_HQG_MAKE_MAIN_DATA);

	NullCtx ctx;
	ctx.phys = phys;
	ctx.data = in.destroyMain;
	p->count = in.destroyMainCount;
	p->r = HqgJudge(&s_judge, &in, s_actMode, &NullEntry, &ctx, p->found, kFindings);
	const HqgResult& r = p->r;

	switch (r.outcome)
	{
	case HQG_OUT_EMPTY:         InterlockedIncrement(&s_empty); break;
	case HQG_OUT_JUDGED:        InterlockedIncrement(&s_judged); break;
	case HQG_OUT_SKIP_STATE:    InterlockedIncrement(&s_skipState); break;
	case HQG_OUT_SKIP_LIST:     InterlockedIncrement(&s_skipList); break;
	case HQG_OUT_SKIP_OVERFLOW: InterlockedIncrement(&s_overflow); break;
	case HQG_OUT_RACED:         InterlockedIncrement(&s_raced); break;
	}
	if (!post && in.destroyMainCount <= HQG_MAX_LIST)
		RaiseMax(&s_maxQueue, (LONG)in.destroyMainCount);
	RaiseMax(&s_maxBatch, (LONG)r.batchCount);
	InterlockedExchangeAdd(&s_scanned, (LONG)r.scanned);
	InterlockedExchangeAdd(&s_dups, (LONG)r.dups);
	InterlockedExchangeAdd(&s_inList, (LONG)r.inList);
	InterlockedExchangeAdd(&s_pending, (LONG)r.pending);
	InterlockedExchangeAdd(&s_freed, (LONG)r.freed);
	InterlockedExchangeAdd(&s_keptRemade, (LONG)r.keptRemade);
	if (post)
		InterlockedExchangeAdd(&s_postFlush, (LONG)r.dups);
	if (s_actMode)
		InterlockedExchangeAdd(&s_rejected, (LONG)r.nulled);
	else
		InterlockedExchangeAdd(&s_observed, (LONG)r.dups);
}

static void EmitPass(const Pass& p, bool post)
{
	for (unsigned i = 0; i < p.r.findings; ++i)
	{
		if (!GuardFireClaim(&s_fireLines, kMaxFireLines))
			return;
		const bool dropped = s_actMode && i < p.r.nulled;
		EmitFireLine(p.found[i], p.count, p.r, post, dropped);
	}
}

// Two passes. Before the flush, over everything queued since the last one.
// After it, over what updateUT itself queued once the flush was done (its
// per-hull update can queue an object whose owner has gone): the physics
// thread has not started, so the back list just flushed is still pending and
// a second queueing of one of its objects is caught before any free.
static void hook_updateUT(void* physics)
{
	InterlockedIncrement(&s_calls);
	const uintptr_t phys = (uintptr_t)physics;
	// A recorded back list belongs to the physics object that flushed it.
	if (phys != s_recPhys)
	{
		HqgForget(&s_judge);
		s_recPhys = phys;
	}

	Pass pre;
	RunPass(phys, false, &pre);

	orig_updateUT(physics);

	HqgRecord(&s_judge,
	          *(const uintptr_t* volatile*)(phys + OFF_HQG_DESTROY_BACK_DATA),
	          *(volatile unsigned*)(phys + OFF_HQG_DESTROY_BACK_COUNT));
	Pass post;
	RunPass(phys, true, &post);
	InterlockedIncrement(&s_flushSeq);

	// After the original, so the lines cost the flush nothing.
	EmitPass(pre, false);
	EmitPass(post, true);
}

// ---- Install -----------------------------------------------------------------

void InstallHullQueueGuard(int* installed, int*)
{
	// The key chooses drop or observe, never whether the site is watched.
	s_actMode = fixes::g_fixesCfg.hullDoublePushGuardEnabled;
	HqgInit(&s_judge, s_slots, kSlots);
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)
		(gameBase + ((const IMAGE_DOS_HEADER*)gameBase)->e_lfanew);
	s_gameEnd = gameBase + nt->OptionalHeader.SizeOfImage;

	const char* why = HookInstall(HOOK_PHYSICS_UPDATE_UT, hook_updateUT,
			&orig_updateUT, installed, true);

	if (!why)
	{
		s_state = 1;
		for (int k = 0; k < kPushers; ++k)
		{
			if (HookInstall(kPushRows[k], kPushDetours[k], &s_origPush[k],
			                installed, true) == NULL)
				++s_pushersInstalled;
		}
		LogMsg(std::string("Hull queue guard: installed, mode=")
		       + (s_actMode ? "guard" : "observe")
		       + " (a heartbeat line follows every minute)");
		if (s_pushersInstalled != kPushers)
			ErrorLog("Hull queue guard: not every pusher detour installed; drops still work, "
			         "but some fire lines will name the caller as inline");
	}
	else
	{
		orig_updateUT = NULL;
		s_state = -1;
		s_why = why;
		ErrorLog(std::string("Hull queue guard: not installed (") + why
		         + "); a hull queued twice for destruction is still deleted twice");
	}
	// The baseline line at zero calls; the tick carries on from here.
	EmitHeartbeat();
	s_lastBeat = ElapsedSec();
}
