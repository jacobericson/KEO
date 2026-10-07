// audit_ogre.cpp - The off-main probes' OgreMain half (OffMainDetail=1): ten call-site rows in
// OgreMain_x64.dll, the worker slots they fill and the main thread's Barrier::sync split by
// request. The animation pre-pass's four syncs run on the main thread; its two handlers and the
// worker loop's two waits and two releases run on Ogre's workers. The rows go in only for the
// OgreMain build they were read from, and only from the main thread, which alone starts a fork:
// while it installs, every worker is parked in the loop's top wait, whose call changes in one
// aligned exchange.
// No hook or callback takes a lock, allocates, or logs; the worker parts touch Interlocked words
// and their own slot only; the [AUDIT-SYNC] line is written from the main thread's
// once-a-second pass.

#include "audit_offmain.h"
#include "audit_steady.h"

namespace audit_ogre_detail {

// One Ogre worker's totals for the frame (Interlocked), and words only that worker touches.
struct __declspec(align(64)) OgreSlot
{
	volatile LONG64 wakeSum, wakeMax, workSum, workMax;
	volatile LONG   wakeN, workN;
	LONG            arriveGen;   // owner: s_fireGen when it last reached the bottom sync, 0 = never
	LONG            oaIdx;       // owner: the animation handler in flight's thread index + 1, 0 = none
	LONGLONG        wakeT;       // owner: when it last woke or passed the top sync, 0 = not since
};

// One animation handler thread index's totals for the frame (Interlocked).
struct __declspec(align(64)) HandlerSlot
{
	volatile LONG64 sum, max;
	volatile LONG   n;
};

// The main thread's Barrier::sync calls of one request bucket in the [AUDIT-SYNC] window.
struct SyncBucket
{
	int      fires, waits;
	LONGLONG fireTicks, waitTicks;
};

// Main-thread totals of the frame in flight.
struct OgreMainFrame
{
	LONGLONG oaFire, oaWait, oaBuild, oaTail, syncWait, mainWake;
	int      syncFireBlk;
};

} // audit_ogre_detail
using namespace audit_ogre_detail;

namespace kenshiframeaudit_detail {

using CallSiteProbe::SHAPE_INT;

// The OgreMain_x64.dll build every offset here was read from.
static const DWORD  OGRE_TIMESTAMP    = 0x5CA5F929;
static const DWORD  OGRE_IMAGE_SIZE   = 0x9C9000;
static const size_t OGRE_BARRIER_SYNC = 0x3DFF40;   // Barrier::sync
static const size_t OGRE_OLD_ANIMS    = 0x2CA920;   // SceneManager::updateAllOldAnimations

// Barrier: thread count, index (flips at every completed sync), arrivals at the current sync.
static const size_t BARRIER_THREADS = 0x00;
static const size_t BARRIER_INDEX   = 0x08;
static const size_t BARRIER_ARRIVED = 0x10;
// SceneManager: the request the workers switch on, and the animation handlers' per-thread entity
// lists (24 bytes each, the count at +8): request 5's owners, request 6's skeleton followers.
static const size_t SM_REQUEST       = 0x4B18;
static const size_t SM_ANIM_OWNERS   = 0x4B78;
static const size_t SM_ANIM_FOLLOW   = 0x4B90;
static const size_t ANIM_LIST_STRIDE = 24;
static const size_t ANIM_LIST_COUNT  = 8;
static const size_t ANIM_LIST_LIMIT  = 1 << 20;   // a larger count is a bad read
static const int    HANDLER_SLOTS    = 32;

// Call sites in OgreMain_x64.dll (RVAs from its image base).
static CallSiteProbe::Site s_ogreSites[] =
{
	// updateAllOldAnimations (main thread): request 5's fire and wait, then request 6's.
	{ "oaFire5",     0x2CAE1B, 0x3DFF40, SHAPE_INT, ST_OA_FIRE },             // Barrier::sync
	{ "oaWait5",     0x2CAE28, 0x3DFF40, SHAPE_INT, ST_OA_WAIT },
	{ "oaFire6",     0x2CAE41, 0x3DFF40, SHAPE_INT, ST_OA_FIRE },
	{ "oaWait6",     0x2CAE4E, 0x3DFF40, SHAPE_INT, ST_OA_WAIT },
	// _updateWorkerThread 0x2CD570 (the workers): the two handlers, rcx the scene manager, rdx the thread index.
	{ "oaWorker5",   0x2CD6B5, 0x2CA860, SHAPE_INT, ST_OA_WK5 },              // request 5's handler
	{ "oaWorker6",   0x2CD6C2, 0x2CA8C0, SHAPE_INT, ST_OA_WK6 },              // request 6's handler
	// ... and its two inlined Barrier::sync: the top one parks the worker until a fire, the bottom one joins.
	{ "ogreWake",    0x2CD5D9, 0x5B7130, SHAPE_INT, ST_OGRE_WAKE, 0, 1, 0, 1 },  // top: WaitForSingleObject (qword)
	{ "ogreLateTop", 0x2CD60F, 0x5B70E0, SHAPE_INT, ST_OGRE_LATETOP, 0, 1 },  // top, the last to arrive: ReleaseSemaphore
	{ "ogreArrive",  0x2CD74F, 0x5B7130, SHAPE_INT, ST_OGRE_ARRIVE, 0, 1 },   // bottom: WaitForSingleObject
	{ "ogreLast",    0x2CD785, 0x5B70E0, SHAPE_INT, ST_OGRE_LAST, 0, 1 },     // bottom, the last to arrive: ReleaseSemaphore
};
static_assert(sizeof(s_ogreSites) / sizeof(s_ogreSites[0]) == NUM_OGRE_ROWS, "the Ogre part's row count");

// The aligned qword holding the top wait's call: the last byte of the instruction before it, the
// call (FF 15 disp32), and the jmp after it.
static const unsigned long long OGRE_TOP_WAIT_QWORD = 0xEB002E9B5115FF18ULL;   // 18 | FF 15 51 9B 2E 00 | EB

static OgreSlot    s_slots[CPU_OGRE_SLOTS];     // by CpuOgreSlot
static HandlerSlot s_handlers[HANDLER_SLOTS];   // by the handler's thread index

// The main thread's last fire: published before the original releases the workers, so a worker
// woken by that fire reads its stamp. The main thread cannot fire again before its wait returns,
// which needs every worker at the bottom sync, so one stamp is enough; the generation proves it.
static volatile LONG64 s_fireQpc    = 0;
static volatile LONG   s_fireGen    = 0;
static volatile LONG64 s_lastArrive = 0;   // the last worker's bottom arrival
static volatile LONG64 s_critTicks  = 0;   // per frame: the last arrival less the fire
static volatile LONG   s_lateTop    = 0;
static volatile LONG   s_wakeDrop   = 0;
static volatile LONG   s_oaOwn      = 0;
static volatile LONG   s_oaFol      = 0;
static volatile LONG   s_oaListMax  = 0;

// Main thread.
static OgreMainFrame s_frame;
static SyncBucket    s_window[syncsplit::REQUEST_BUCKETS];
static int           s_winFire = 0, s_winWait = 0, s_winOdd = 0, s_winFireBlk = 0;
static LONGLONG      s_winStart = 0;
static int           s_passes   = 0;
static LONGLONG      s_oaEntry = 0, s_oaFirstFire = 0, s_oaLastWait = 0;

static void AtomicMaxLong(volatile LONG* p, LONG v)
{
	LONG cur = *p;
	while (v > cur)
	{
		LONG prev = InterlockedCompareExchange(p, v, cur);
		if (prev == cur)
			return;
		cur = prev;
	}
}

static bool OgreMainMatches(HMODULE ogre)
{
	uintptr_t base = (uintptr_t)ogre;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != OGRE_TIMESTAMP ||
	    nt->OptionalHeader.SizeOfImage != OGRE_IMAGE_SIZE)
		return false;
	return (const void*)GetProcAddress(ogre, SYM_BARRIER_SYNC) == (const void*)(base + OGRE_BARRIER_SYNC) &&
	       (const void*)GetProcAddress(ogre, SYM_OLD_ANIMS) == (const void*)(base + OGRE_OLD_ANIMS);
}

// Why the worker-loop rows stay out, or an empty string: the top wait's call must lie inside one
// aligned qword that still holds the eight bytes it was read with.
static std::string TopWaitRefusal(uintptr_t base)
{
	for (int i = 0; i < NUM_OGRE_ROWS; ++i)
	{
		const CallSiteProbe::Site& s = s_ogreSites[i];
		if (s.tag != ST_OGRE_WAKE)
			continue;
		uintptr_t site = base + s.siteRva;
		if (!s.qword || !syncsplit::InOneQword((unsigned long long)site, 6))
			return Fmt("the top wait at +0x%X is not inside one aligned qword", (unsigned)s.siteRva);
		unsigned long long now = *(const volatile unsigned long long*)(site & ~(uintptr_t)7);
		if (now != OGRE_TOP_WAIT_QWORD)
			return Fmt("the qword at +0x%X reads %016llX, want %016llX", (unsigned)(s.siteRva & ~(size_t)7),
			           now, OGRE_TOP_WAIT_QWORD);
		return std::string();
	}
	return "no top wait row";
}

int InstallOgreProbes(HMODULE ogre)
{
	s_winStart = Now();
	for (int i = 0; i < NUM_OGRE_ROWS; ++i)
		s_ogreSites[i].id = -1;
	const char* why = !ogre ? "no module"
	                : !OgreMainMatches(ogre) ? "build mismatch"
	                : (!g_syncHooked || !g_oldAnimHooked) ? "needs RenderDetail=1 RenderDeep=1"
	                : !IsMain() ? "not the main thread"
	                : NULL;
	if (why)
	{
		AuditLine(Fmt("[Audit] offmain: Ogre part off (%s)", why));
		return 0;
	}
	// A parked worker returns into the top wait and runs its call again, so that call changes in one
	// aligned exchange (its qword row). The loop's other rows run only between a fire and its join,
	// and only the main thread fires; this runs on the main thread, so none of them is running now.
	std::string loopWhy = TopWaitRefusal((uintptr_t)ogre);
	if (!loopWhy.empty())
		AuditLine(Fmt("[Audit] offmain: Ogre worker-loop rows off (%s)", loopWhy.c_str()));
	int idx[NUM_OGRE_ROWS];
	int n = 0;
	for (int i = 0; i < NUM_OGRE_ROWS; ++i)
	{
		if (loopWhy.empty() || s_ogreSites[i].tag < ST_OGRE_WORKER_FIRST)
			idx[n++] = i;
		else
			_snprintf_s(s_ogreSites[i].status, sizeof(s_ogreSites[i].status), _TRUNCATE, "SKIP worker-loop rows off");
	}
	int ok = InstallOffMainRows(ogre, s_ogreSites, idx, n);
	MarkOffMainTags(s_ogreSites, NUM_OGRE_ROWS);
	AuditLine(Fmt("[Audit] offmain: Ogre part %s (%d/%d rows, page=%p)", OffMainPartWord(ok, NUM_OGRE_ROWS), ok,
	              NUM_OGRE_ROWS, CallSiteProbe::StubPageFor(ogre)));
	return ok;
}

// ---- Main thread: the Barrier::sync split ----

void OgreSyncEnter(void* barrier, OgreSyncCall* c)
{
	const char* b = (const char*)barrier;
	int threads = *(const int*)(b + BARRIER_THREADS);
	int index   = *(const int*)(b + BARRIER_INDEX);
	int arrived = *(const int*)(b + BARRIER_ARRIVED);
	uintptr_t sm = (uintptr_t)g_sceneMgr;
	int request = PlausiblePtr(sm) ? *(const int*)(sm + SM_REQUEST) : -1;
	c->kind    = syncsplit::KindOf(index);
	c->blocked = syncsplit::WouldBlock(arrived, threads);
	c->bucket  = syncsplit::RequestBucket(request);
	c->t0      = Now();
	if (c->kind == syncsplit::SK_FIRE)
	{
		InterlockedExchange64(&s_fireQpc, c->t0);
		InterlockedIncrement(&s_fireGen);
	}
}

void OgreSyncExit(const OgreSyncCall& c)
{
	LONGLONG end = Now();
	LONGLONG d = end - c.t0;
	SyncBucket& w = s_window[c.bucket];
	bool open = g_cur.open;
	if (c.kind == syncsplit::SK_FIRE)
	{
		++w.fires;
		w.fireTicks += d;
		++s_winFire;
		if (c.blocked)
		{
			++s_winFireBlk;
			if (open)
				++s_frame.syncFireBlk;
		}
	}
	else if (c.kind == syncsplit::SK_WAIT)
	{
		++w.waits;
		w.waitTicks += d;
		++s_winWait;
		if (open)
		{
			s_frame.syncWait += d;
			// The last worker's arrival, when it came after this fork's fire: main's own wake-up.
			LONGLONG last = s_lastArrive;
			if (last != 0 && last >= s_fireQpc && end >= last)
				s_frame.mainWake += end - last;
		}
	}
	else
		++s_winOdd;
}

void OgreOldAnimsEnter()
{
	if (!g_cfg.offMainDetail || !IsMain() || !g_cur.open)
		return;
	s_oaEntry     = Now();
	s_oaFirstFire = 0;
	s_oaLastWait  = 0;
}

void OgreOldAnimsExit()
{
	if (!g_cfg.offMainDetail || !IsMain() || !g_cur.open || s_oaEntry == 0)
		return;
	LONGLONG now = Now();
	if (s_oaFirstFire != 0)
		s_frame.oaBuild += s_oaFirstFire - s_oaEntry;
	if (s_oaLastWait != 0)
		s_frame.oaTail += now - s_oaLastWait;
	s_oaEntry = 0;
}

// ---- Ogre workers: Interlocked words and the caller's own slot only ----

static void HandlerEnter(bool owners, uintptr_t sm, int idx)
{
	if (idx < 0 || idx >= HANDLER_SLOTS)
		return;
	int slot = CpuOgreSlot();
	if (slot >= 0)
		s_slots[slot].oaIdx = idx + 1;
	if (!PlausiblePtr(sm))
		return;
	uintptr_t lists = *(const uintptr_t*)(sm + (owners ? SM_ANIM_OWNERS : SM_ANIM_FOLLOW));
	if (!PlausiblePtr(lists))
		return;
	size_t count = *(const size_t*)(lists + (size_t)idx * ANIM_LIST_STRIDE + ANIM_LIST_COUNT);
	if (count > ANIM_LIST_LIMIT)
		return;
	InterlockedExchangeAdd(owners ? &s_oaOwn : &s_oaFol, (LONG)count);
	AtomicMaxLong(&s_oaListMax, (LONG)count);
}

static void HandlerExit(LONGLONG d)
{
	int slot = CpuOgreSlot();
	if (slot < 0)
		return;
	LONG idx = s_slots[slot].oaIdx - 1;
	s_slots[slot].oaIdx = 0;
	if (idx < 0 || idx >= HANDLER_SLOTS)
		return;
	HandlerSlot& h = s_handlers[idx];
	InterlockedExchangeAdd64(&h.sum, d);
	AtomicMax64(&h.max, d);
	InterlockedIncrement(&h.n);
}

// The top wait returned at t1. A worker that did not see exactly one fire since its last join is
// a drop, with no wake recorded.
static void Wake(LONGLONG t1)
{
	int slot = CpuOgreSlot();
	if (slot < 0)
		return;
	OgreSlot& s = s_slots[slot];
	ULONG gen = (ULONG)s_fireGen;
	if (s.arriveGen != 0 && gen != (ULONG)s.arriveGen + 1u)
		InterlockedIncrement(&s_wakeDrop);
	else
	{
		LONGLONG fire = s_fireQpc;
		if (fire != 0 && t1 >= fire)
		{
			InterlockedExchangeAdd64(&s.wakeSum, t1 - fire);
			AtomicMax64(&s.wakeMax, t1 - fire);
			InterlockedIncrement(&s.wakeN);
		}
	}
	s.wakeT = t1;
}

// The bottom sync, before its wait or release: the work since the wake, and the fork it finished.
static void Arrive(bool last)
{
	LONGLONG arrive = Now();
	LONG gen = s_fireGen;
	int slot = CpuOgreSlot();
	if (slot >= 0)
	{
		OgreSlot& s = s_slots[slot];
		if (s.wakeT != 0)
		{
			InterlockedExchangeAdd64(&s.workSum, arrive - s.wakeT);
			AtomicMax64(&s.workMax, arrive - s.wakeT);
			InterlockedIncrement(&s.workN);
			s.wakeT = 0;
		}
		s.arriveGen = gen;
	}
	if (last)
	{
		// Published before the release that ends main's wait.
		InterlockedExchange64(&s_lastArrive, arrive);
		LONGLONG fire = s_fireQpc;
		if (fire != 0 && arrive >= fire)
			InterlockedExchangeAdd64(&s_critTicks, arrive - fire);
	}
}

void OgreProbeEnter(int tag, CallSiteProbe::U64 a, CallSiteProbe::U64 b)
{
	switch (tag)
	{
	case ST_OA_WK5:
	case ST_OA_WK6:
		HandlerEnter(tag == ST_OA_WK5, (uintptr_t)a, (int)b);
		break;
	case ST_OGRE_LATETOP:
	{
		// The last to arrive at the top releases the others and runs on without waiting.
		InterlockedIncrement(&s_lateTop);
		int slot = CpuOgreSlot();
		if (slot >= 0)
			s_slots[slot].wakeT = Now();
		break;
	}
	case ST_OGRE_ARRIVE:
	case ST_OGRE_LAST:
		Arrive(tag == ST_OGRE_LAST);
		break;
	default:
		break;
	}
}

void OgreProbeExit(int tag, LONGLONG t0, LONGLONG t1)
{
	switch (tag)
	{
	case ST_OA_FIRE:   // main thread, frame open (OffMainProbeExit)
		s_frame.oaFire += t1 - t0;
		if (s_oaEntry != 0 && s_oaFirstFire == 0)
			s_oaFirstFire = t0;
		break;
	case ST_OA_WAIT:   // main thread, frame open
		s_frame.oaWait += t1 - t0;
		if (s_oaEntry != 0)
			s_oaLastWait = t1;
		break;
	case ST_OA_WK5:
	case ST_OA_WK6:
		HandlerExit(t1 - t0);
		break;
	case ST_OGRE_WAKE:
		Wake(t1);
		break;
	default:
		break;
	}
}

// ---- Main thread: the frame record and the [AUDIT-SYNC] line ----

static syncsplit::WorkerFrame Take(volatile LONG64* sum, volatile LONG64* mx, volatile LONG* n)
{
	syncsplit::WorkerFrame f;
	f.sum = InterlockedExchange64(sum, 0);
	f.max = InterlockedExchange64(mx, 0);
	f.n   = (int)InterlockedExchange(n, 0);
	return f;
}

static float MsOf(LONGLONG ticks, bool have)
{
	return have ? TicksToMs(ticks) : Nan();
}

static float UsOf(long long ticks, bool have)
{
	return have ? (float)syncsplit::TicksToUs(ticks, g_qpcFreq) : Nan();
}

// Every fork joins before the frame closes, so each slot's totals belong to this frame.
void OgreFrameTotals(FrameRec& r)
{
	syncsplit::WorkerFrame wake[CPU_OGRE_SLOTS], work[CPU_OGRE_SLOTS], hand[HANDLER_SLOTS];
	for (int i = 0; i < CPU_OGRE_SLOTS; ++i)
	{
		wake[i] = Take(&s_slots[i].wakeSum, &s_slots[i].wakeMax, &s_slots[i].wakeN);
		work[i] = Take(&s_slots[i].workSum, &s_slots[i].workMax, &s_slots[i].workN);
	}
	for (int i = 0; i < HANDLER_SLOTS; ++i)
		hand[i] = Take(&s_handlers[i].sum, &s_handlers[i].max, &s_handlers[i].n);
	syncsplit::Reduced wk = syncsplit::Reduce(wake, CPU_OGRE_SLOTS);
	syncsplit::Reduced wo = syncsplit::Reduce(work, CPU_OGRE_SLOTS);
	syncsplit::Reduced ha = syncsplit::Reduce(hand, HANDLER_SLOTS);
	LONGLONG crit = InterlockedExchange64(&s_critTicks, 0);

	bool fire     = g_haveTag[ST_OA_FIRE];
	bool wait     = g_haveTag[ST_OA_WAIT];
	bool handlers = g_haveTag[ST_OA_WK5] && g_haveTag[ST_OA_WK6];
	bool wakeRow  = g_haveTag[ST_OGRE_WAKE];
	bool lastRow  = g_haveTag[ST_OGRE_LAST];
	bool workRows = wakeRow && lastRow && g_haveTag[ST_OGRE_LATETOP] && g_haveTag[ST_OGRE_ARRIVE];
	r.m[M_OM_OAFIRE]   = MsOf(s_frame.oaFire, fire);
	r.m[M_OM_OAWAIT]   = MsOf(s_frame.oaWait, wait);
	r.m[M_OM_OABUILD]  = MsOf(s_frame.oaBuild, fire && g_oldAnimHooked);
	r.m[M_OM_OATAIL]   = MsOf(s_frame.oaTail, wait && g_oldAnimHooked);
	r.m[M_OM_OAWKSUM]  = MsOf(ha.sum, handlers);
	r.m[M_OM_OAWKMAX]  = MsOf(ha.max, handlers);
	r.m[M_OM_SYNCWAIT] = MsOf(s_frame.syncWait, g_syncHooked);
	r.m[M_OM_WAKEMAX]  = UsOf(wk.max, wakeRow);
	r.m[M_OM_WAKEMEAN] = wakeRow ? (float)(wk.mean * 1000000.0 / (double)g_qpcFreq) : Nan();
	r.m[M_OM_WORKMAX]  = UsOf(wo.max, workRows);
	r.m[M_OM_WORKSUM]  = UsOf(wo.sum, workRows);
	r.m[M_OM_CRIT]     = UsOf(crit, lastRow);
	r.m[M_OM_MAINWAKE] = UsOf(s_frame.mainWake, lastRow);
	r.c[C_OAOWN]        = (int)InterlockedExchange(&s_oaOwn, 0);
	r.c[C_OAFOL]        = (int)InterlockedExchange(&s_oaFol, 0);
	r.c[C_OALISTMAX]    = (int)InterlockedExchange(&s_oaListMax, 0);
	r.c[C_SYNCFIREBLK]  = s_frame.syncFireBlk;
	r.c[C_OGRELATETOP]  = (int)InterlockedExchange(&s_lateTop, 0);
	r.c[C_OGREWAKEN]    = wk.n;
	r.c[C_OGREWAKEDROP] = (int)InterlockedExchange(&s_wakeDrop, 0);
	memset(&s_frame, 0, sizeof(s_frame));
}

// Every fifth pass: one line for the window, a bucket per worker request that had a call.
void OffMainOncePerSecond()
{
	if (!g_cfg.offMainDetail || ++s_passes < 5)
		return;
	s_passes = 0;
	LONGLONG now = Now();
	if (s_winStart == 0)
		s_winStart = now;
	std::string line = Fmt("[AUDIT-SYNC] win=%.1fs fire=%d wait=%d odd=%d fireBlk=%d |",
	                       (double)(now - s_winStart) / (double)g_qpcFreq, s_winFire, s_winWait, s_winOdd, s_winFireBlk);
	for (int k = 0; k < syncsplit::REQUEST_BUCKETS; ++k)
	{
		const SyncBucket& w = s_window[k];
		if (w.fires == 0 && w.waits == 0)
			continue;
		line += Fmt(" r%d=%d/%.2f/%.2f", k, w.fires, (double)TicksToMs(w.fireTicks), (double)TicksToMs(w.waitTicks));
	}
	AuditLine(line);
	memset(s_window, 0, sizeof(s_window));
	s_winFire = s_winWait = s_winOdd = s_winFireBlk = 0;
	s_winStart = now;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
