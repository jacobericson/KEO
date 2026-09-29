#include "navmesh/generation/nm_misspar.h"
#include "navmesh/generation/misspar_math.h"
#include "base/core.h"
#include <iomanip>
#include <string.h>

static volatile LONG s_holder = MP_HOLD_NONE;
void MissParHolderSet(int kind) { InterlockedExchange(&s_holder, kind); }
int  MissParHolderGet()         { return (int)InterlockedCompareExchange(&s_holder, 0, 0); }

static __declspec(thread) LONGLONG t_genStart = 0;
static __declspec(thread) LONGLONG t_genEnd   = 0;

void MissParGenBegin() { t_genStart = QpcNow(); t_genEnd = 0; }
void MissParGenEnd()   { t_genEnd = QpcNow(); }

// Window sums, in QPC ticks; reset by the main thread at each print.
enum { PH_WAIT, PH_SETUP, PH_GEN, PH_TAIL, PH_FIX, PH_BC, PH_COUNT };
static volatile LONG     s_jobs = 0;
static volatile LONGLONG s_phase[PH_COUNT];
static volatile LONGLONG s_heldBy[MP_HOLD_KINDS];

// Tgen intervals of the window, for the overlap ratio.
static const int MP_RING = 64;
struct GenSlot { volatile LONG seq; LONGLONG start, end; };
static GenSlot       s_gen[MP_RING];
static volatile LONG s_genWritten = 0;
static LONG          s_genRead    = 0;   // main thread only

void MissParRecordJob(const MissParJob& j)
{
	// t_genStart/t_genEnd are stamped by the populate hook on this same
	// thread, but a bg-thread T234 dispatch (CallOrigDispatchUnderPj) also
	// runs processJobAlt and can populate without ever reaching this
	// function. Only trust the stamps when they fall inside this job's own
	// fn_processJobAlt window; otherwise this job's generate never reached
	// populate (an aborted or zero-face run) and Tgen is 0, same as if the
	// stamps had never been touched.
	LONGLONG genTicks = 0;
	if (t_genStart && t_genEnd > t_genStart && t_genStart >= j.jobAltStart && t_genEnd <= j.jobAltEnd)
		genTicks = t_genEnd - t_genStart;

	LONGLONG setup = (j.jobAltStart - j.locked);
	LONGLONG jobAlt = (j.jobAltEnd - j.jobAltStart);
	InterlockedIncrement(&s_jobs);
	InterlockedExchangeAdd64(&s_phase[PH_WAIT],  j.locked - j.waitStart);
	InterlockedExchangeAdd64(&s_phase[PH_SETUP], setup + (jobAlt - genTicks));   // processJobAlt outside realGenerate counts as setup
	InterlockedExchangeAdd64(&s_phase[PH_GEN],   genTicks);
	InterlockedExchangeAdd64(&s_phase[PH_FIX],   j.fixEnd - j.jobAltEnd);
	InterlockedExchangeAdd64(&s_phase[PH_TAIL],  j.released - j.fixEnd);
	InterlockedExchangeAdd64(&s_phase[PH_BC],    j.bcEnd - j.bcStart);
	if (j.holderAtWait >= 0 && j.holderAtWait < MP_HOLD_KINDS)
		InterlockedExchangeAdd64(&s_heldBy[j.holderAtWait], j.locked - j.waitStart);

	if (genTicks)
	{
		LONG idx = InterlockedIncrement(&s_genWritten) - 1;
		GenSlot* s = &s_gen[idx % MP_RING];
		InterlockedExchange(&s->seq, -1);
		s->start = t_genStart;
		s->end   = t_genEnd;
		InterlockedExchange(&s->seq, idx);
	}
	t_genStart = t_genEnd = 0;
}

void MissParNoteHash(const NavMeshCacheEntry& e, bool worker)
{
	if (!navmesh::g_navmeshCfg.navmeshMissHashEnabled)
		return;
	MissParSpan spans[11] = {
		{ e.cachedFaces,    e.faceCount * HKAI_FACE_SIZE },
		{ e.cachedEdges,    e.edgeCount * HKAI_EDGE_SIZE },
		{ e.cachedVertices, e.vertexCount * HKAI_VERTEX_SIZE },
		{ e.cachedFaceData, e.faceDataCount * HKAI_FACEDATA_UNIT },
		{ e.cachedEdgeData, e.edgeDataCount * HKAI_EDGEDATA_UNIT },
		{ &e.faceDataStriding, 4 }, { &e.edgeDataStriding, 4 }, { &e.navMeshFlags, 1 },
		{ e.aabb, 32 }, { &e.erosionRadius, 4 }, { &e.userData, 8 } };
	unsigned __int64 h = MissParFnv64(spans, 11);
	double genMs = (t_genStart && t_genEnd > t_genStart) ? QpcToMs(t_genEnd - t_genStart) : -1.0;

	char line[224];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"MissHash: key=%d,%d,%d,%d,%08x,%08x faces=%d hash=%016llx gen=%.1fms %s",
		e.key.gridX, e.key.gridY, e.key.sectionTileId, e.key.jobType,
		e.key.aabbHash, e.key.buildingHash, e.faceCount, h, genMs, worker ? "w" : "bg");
	LogMsgDeferrable(line);
}

#include "game/game.h"

// The prologue bytes hkaiKeycode::validate opens with; only these 16 bytes
// are checked (VerifyPrologue), not the branches past them.
typedef int (*keycodeValidate_t)(unsigned char* flag);
static const unsigned char KEYCODE_VALIDATE_BYTES[16] =
	{ 0x40,0x53,0x56,0x57,0x41,0x55,0x41,0x56,0x48,0x83,0xEC,0x60,0x33,0xDB,0xC6,0x01 };
static const size_t KEYCODE_FLAGS[5] = { 0x2148504, 0x214853E, 0x214855F, 0x2148540, 0x214855D };

static volatile LONG s_keycodeChecked = 0;   // 0 = not yet checked, 1 = validator bytes match, 2 = mismatch
static volatile LONG s_keycodeRetries = 0;
static LONGLONG      s_keycodeLastTry = 0;   // written and read under processJobCS only

bool MissParKeycodesReady()
{
	for (int i = 0; i < 5; ++i)
		if (*(volatile unsigned char*)GameAddr(KEYCODE_FLAGS[i]) == 0)
			return false;
	return true;
}

// Runs the game's own validator on each flag that reads 0, single-threaded
// against every generation (the caller holds processJobCS). Never writes a
// flag itself and never re-runs the validator on one that already reads 1:
// re-running it on a set flag zeroes it under a concurrent reader.
void MissParKeycodeWarmup()
{
	LONG checked = InterlockedCompareExchange(&s_keycodeChecked, 0, 0);
	if (checked == 0)
	{
		checked = VerifyPrologue(RVA_HKAI_KEYCODE_VALIDATE, KEYCODE_VALIDATE_BYTES, "hkaiKeycode::validate") ? 1 : 2;
		InterlockedExchange(&s_keycodeChecked, checked);
	}
	if (checked != 1)
		return;

	LONGLONG now = QpcNow();
	if (s_keycodeLastTry && QpcToMs(now - s_keycodeLastTry) < 1000.0)
		return;
	s_keycodeLastTry = now;

	keycodeValidate_t validate = (keycodeValidate_t)GameAddr(RVA_HKAI_KEYCODE_VALIDATE);
	for (int i = 0; i < 5; ++i)
	{
		unsigned char* flag = (unsigned char*)GameAddr(KEYCODE_FLAGS[i]);
		if (*flag == 0)
			validate(flag);
	}
	if (!MissParKeycodesReady())
	{
		InterlockedIncrement(&s_keycodeRetries);
		LogMsgDeferrable("keycode warm-up: a flag still reads 0 (retrying on a later dispatch)");
	}
}

// --------------------------------------------------------------------
// The populate split: processJobCS released around realGenerate
// --------------------------------------------------------------------
//
// processJobAlt's setup (scratch buffer, seed map, painters, the real work
// buffer) and its tail (finalize, the timing log, the L1 store that follows in
// ProcessNavMeshJob) stay under processJobCS. Only realGenerate, reached through
// populate, runs without it: realGenerate reads the work buffer it is given,
// the input geometry and the result mesh, and all three belong to the job
// whenever that work buffer is a fresh one built for it. A clone has its own;
// the swap path's is installed at realNMG+256 and is dealt with below. A
// generation on the generator's own work buffer (vanilla types 0/1/3) never
// releases.
//
// Lock order: processJobCS is dropped before the generation slot is taken and
// re-taken after the slot is returned, so no thread ever waits for a slot while
// holding processJobCS, and the re-acquire holds nothing else.
#include "navmesh/nm_workers.h"

enum { MISS_SERIAL = 0, MISS_ARMED, MISS_IN_GENERATE, MISS_REACQUIRED, MISS_LOCK_LOST };
static __declspec(thread) int   t_miss    = MISS_SERIAL;
static __declspec(thread) void* t_realNmg = NULL;
static __declspec(thread) int   t_arm     = MP_ARM_SERIAL;
static __declspec(thread) bool  t_swapCounted = false;

static HANDLE        g_genSlots = NULL;
static LONG          g_genCap   = 1;
static volatile LONG s_splitReleased = 0, s_splitPassed = 0, s_slotWaitN = 0, s_slotStopSkip = 0;
static volatile LONGLONG s_slotWaitTicks = 0, s_reacqTicks = 0;
static volatile LONG s_inflightWaits = 0, s_inflightFull = 0, s_inflightTimeouts = 0, s_inflightStopped = 0;
static volatile LONG s_lockLost = 0, s_splitBlocked = 0, s_partialSkip = 0;
static volatile LONGLONG s_reacqHeldBy[MP_HOLD_KINDS];

// --------------------------------------------------------------------
// Releasing a generation that runs on the real generator
// --------------------------------------------------------------------
//
// A MISS that runs on the real generator has its fresh work buffer installed at
// realNMG+256 for the length of processJobAlt. realGenerate takes that buffer as
// an argument and never reads the generator, so the only thing the install
// guards is the +256 slot itself: every other thread that reads it under
// processJobCS (a clone's work-buffer copy, a vanilla dispatch's own setup) must
// see the generator's own buffer. So the slot is put back to that buffer before
// the release and the fresh one re-installed after the re-acquire, both under
// the lock, and only one swap run may be released at a time.
//
// The re-acquire here is unconditional, unlike a clone's. A clone that meets the
// stop hands its job back and the worker retires; the bg thread cannot do that,
// because the caller's tail restores the +256 slot and NavMesh::stop's own
// teardown joins the thread. It re-acquires through NavMeshTryLockProcessJobFor,
// which bounds each attempt and keeps the same hold bookkeeping as a plain
// Enter.
static volatile LONG     s_swapArmed = 0;    // threads inside a swap-path processJobAlt
static volatile LONG     s_swapToken = 0;    // 1 while a swap run is released
static volatile LONG     s_relByKind[3];     // released generations, indexed by MP_ARM_*
static volatile LONG     s_bgN = 0;
static volatile LONGLONG s_bgReacqTicks = 0;
static volatile LONG     s_bgPassBlocked = 0, s_bgPassNoCanon = 0, s_bgPassToken = 0;
static volatile LONG     s_wbSlotRace = 0;
// Vanilla dispatches whose observation was refused because a swap run was armed.
// A count that keeps rising while no bg release happens means the arm counter is
// stuck, which is the difference between 'never proven' and 'cannot be proven'.
static volatile LONG     s_canonSkipArmed = 0;

// Written and read under processJobCS only (the stats line reads it for a
// diagnostic word, which is the one racy read and cannot mislead a generation).
// The single observation site is a vanilla dispatch's populate: it runs on the
// generator's own work buffer, and it only ever runs on the background thread,
// which is also the only thread that swaps one in, so it cannot observe its own
// fresh buffer. A worker whose clone construction failed swaps too, which is why
// an observation is still refused while any swap run is armed.
static MissParCanonState s_canon;
static const int  CANON_AGREE   = 3;
static const DWORD REACQ_ROUND_MS = 10000;

// The save-load reset's drain. s_splitLive counts generations that released
// processJobCS and have not yet re-acquired it (or given it up at the stop);
// s_splitBlock is raised by a reset while it waits for that count to reach 0
// and unloads. A populate increments s_splitLive before it reads s_splitBlock
// and the reset raises s_splitBlock before it reads s_splitLive, both through
// interlocked (full-barrier) operations, so either the populate sees the block
// and stays under the lock or the reset sees the generation and waits for it.
static volatile LONG s_splitLive  = 0;
static volatile LONG s_splitBlock = 0;
static const DWORD   DRAIN_POLL_MS = 2;

// Rounds of WAITED before a job gives up on the key and proceeds unregistered
// (a late HIT under the lock still catches a finished duplicate).
static const int INFLIGHT_MAX_ROUNDS = 4;
static const DWORD INFLIGHT_WAIT_MS = 120000;
static const DWORD GEN_SLOT_POLL_MS = 50;

void MissParInit()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	g_genCap = MissParGenConcurrency(navmesh::g_navmeshCfg.cfg_navmeshGenConcurrency, (int)si.dwNumberOfProcessors, 4);
	LogMsg(MissParGenConcurrencyMessage(navmesh::g_navmeshCfg.cfg_navmeshGenConcurrency, g_genCap,
	                                     (int)si.dwNumberOfProcessors, !navmesh::g_navmeshCfg.navmeshMissSplitEnabled));
	// Room above the cap for the shutdown wake's releases.
	g_genSlots = CreateSemaphore(NULL, g_genCap, g_genCap + NAVMESH_WORKER_COUNT + 1, NULL);
	InflightInit();
	MissParCanonInit(s_canon);
}

static inline void** WbSlot(void* realNMG)
{
	return (void**)(KLIB_MEMBER(4, (uintptr_t)realNMG, NavMeshGenerator_settings, 256));
}

void MissParArm(bool clone, void* realNMG)
{
	t_miss    = MISS_ARMED;
	t_realNmg = realNMG;
	t_arm     = clone ? MP_ARM_CLONE : MP_ARM_SWAP;
	// Only while the swap release can actually be taken: with it off nothing
	// reads the count, and leaving it at zero keeps canonSkip= meaningful in
	// that arm. The flag makes the pair symmetric whatever the setting does
	// between the arm and the disarm.
	if (!clone && navmesh::g_navmeshCfg.navmeshMissSplitBgEnabled)
	{
		t_swapCounted = true;
		InterlockedIncrement(&s_swapArmed);
	}
}

void MissParDisarm()
{
	if (t_swapCounted)
	{
		InterlockedDecrement(&s_swapArmed);
		t_swapCounted = false;
	}
	t_miss    = MISS_SERIAL;
	t_realNmg = NULL;
	t_arm     = MP_ARM_SERIAL;
}

// The caller holds processJobCS.
static int ClassifyRelease(void* wb)
{
	if (t_miss != MISS_ARMED || t_realNmg == NULL)
		return MP_REL_NONE;
	MissParReleaseInputs in;
	in.armKind          = t_arm;
	in.splitEnabled     = navmesh::g_navmeshCfg.navmeshMissSplitEnabled;
	in.slotsReady       = (g_genSlots != NULL);
	in.pjDepth          = NavMeshProcessJobDepth();
	in.keycodesReady    = MissParKeycodesReady();
	in.stopSeen         = NavMeshStopSeen();
	// The hold this thread is inside was taken as a background MISS: the MISS
	// block stamps the holder from its own thread test and nobody else can
	// write it while this thread owns the lock.
	in.holderIsBg       = (MissParHolderGet() == MP_HOLD_BGMISS);
	in.wb               = wb;
	in.installedWb      = *WbSlot(t_realNmg);
	in.bgSplitEnabled   = navmesh::g_navmeshCfg.navmeshMissSplitBgEnabled;
	in.swapOutstanding  = InterlockedCompareExchange(&s_swapArmed, 0, 0);
	in.canonicalWb      = MissParCanonConfirmed(s_canon, CANON_AGREE);
	return MissParClassifyRelease(in);
}

// The holder sampled when a re-acquire wait starts, bucketed like pjHeld's
// first wait, so the two together account for a split generation's whole
// processJobCS wait.
static void NoteReacqWait(int holder, LONGLONG ticks)
{
	if (holder >= 0 && holder < MP_HOLD_KINDS)
		InterlockedExchangeAdd64(&s_reacqHeldBy[holder], ticks);
}

// No C++ objects with destructors here: __try/__finally cannot share a frame
// with them. The slot wait is polled so a stop seen while queued skips the
// generation; the outer __finally re-acquires processJobCS so the caller's
// lock accounting holds on every path out, unwinding included.
static void GenerateUnlocked(nmResultPopulate_t orig, void* wb, void* local, void* mesh, int param)
{
	NavMeshLeaveForGenerate();
	__try
	{
		LONGLONG w0 = QpcNow();
		bool slot = false, waitFailed = false;
		for (;;)
		{
			if (NavMeshStopSeen())
				break;
			DWORD r = WaitForSingleObject(g_genSlots, GEN_SLOT_POLL_MS);
			if (r == WAIT_OBJECT_0) { slot = true; break; }
			if (r != WAIT_TIMEOUT)  { waitFailed = true; break; }
		}
		InterlockedIncrement(&s_slotWaitN);
		InterlockedExchangeAdd64(&s_slotWaitTicks, QpcNow() - w0);
		__try
		{
			// Without the stop, a failed wait generates uncapped rather than
			// leaving the tile empty. After it, nothing Havok runs here; the
			// empty mesh is never cached.
			if ((slot || waitFailed) && !NavMeshStopSeen())
			{
				MissParGenBegin();
				orig(wb, local, mesh, param);
				MissParGenEnd();
			}
			else
				InterlockedIncrement(&s_slotStopSkip);
		}
		__finally
		{
			if (slot)
				ReleaseSemaphore(g_genSlots, 1, NULL);
		}
	}
	__finally
	{
		// Stop-aware: once the stop is seen the lock is not taken again (see
		// NavMeshReenterAfterGenerate). The live count drops only after this,
		// so a draining reset cannot take processJobCS before a re-acquired
		// thread has left it again, with its tail and L1 store done.
		LONGLONG r0 = QpcNow();
		int holder = MissParHolderGet();
		int re = NavMeshReenterAfterGenerate();
		LONGLONG waited = QpcNow() - r0;
		InterlockedExchangeAdd64(&s_reacqTicks, waited);
		NoteReacqWait(holder, waited);
		if (re != NM_REENTER_HELD)
		{
			t_miss = MISS_LOCK_LOST;
			InterlockedIncrement(&s_lockLost);
		}
		InterlockedDecrement(&s_splitLive);
		// A backstop: the retire returns only once no worker is live, so a
		// worker cannot get here. If one did, NavMesh::stop may already have
		// released the Havok heap and returning would run processJobAlt's tail
		// on it, so this thread never runs again.
		if (re == NM_REENTER_RETIRED)
			for (;;)
				Sleep(INFINITE);
	}
}

// Blocks until processJobCS is held again. Each attempt is bounded, so a round
// that loses the lock to another thread simply tries again; the loop ends only
// with the lock, because the caller's tail reinstalls the +256 slot and every
// other thread that reads it does so under this lock. Nothing this thread holds
// is waited on by a holder, so a holder always reaches its own release.
static void ReacquireForSwap()
{
	for (;;)
	{
		if (NavMeshTryLockProcessJobFor(REACQ_ROUND_MS, NULL) == NM_PJLOCK_HELD)
			break;
		// NM_PJLOCK_NONE answers without waiting (it means the lock was never
		// initialised, which cannot happen once a generation has run), so the
		// retry is paced here rather than spun.
		Sleep(1);
	}
	MissParHolderSet(MP_HOLD_BGMISS);
}

// The swap path's release. No generation slot: the cap bounds the clone
// generations, and one more from the single thread that can be here keeps the
// concurrency the measurements were taken at. The stop is not re-tested inside:
// this thread generates through a stop today as well, and unlike a clone it has
// no way to hand its job back.
static void GenerateUnlockedSwap(nmResultPopulate_t orig, void** wbSlot, void* canonical,
                                 void* wb, void* local, void* mesh, int param)
{
	*wbSlot = canonical;
	NavMeshLeaveForGenerate();
	__try
	{
		MissParGenBegin();
		orig(wb, local, mesh, param);
		MissParGenEnd();
	}
	__finally
	{
		LONGLONG r0 = QpcNow();
		int holder = MissParHolderGet();
		ReacquireForSwap();
		LONGLONG waited = QpcNow() - r0;
		InterlockedIncrement(&s_bgN);
		InterlockedExchangeAdd64(&s_bgReacqTicks, waited);
		NoteReacqWait(holder, waited);
		// Under the lock again: processJobAlt's tail reads this slot. Anything
		// but the canonical buffer here means a second swap run installed one
		// while this thread was released, which needs a failed clone
		// construction and is counted rather than worked around: the tail must
		// see this job's own buffer either way.
		if (*wbSlot != canonical)
			InterlockedIncrement(&s_wbSlotRace);
		*wbSlot = wb;
		InterlockedExchange(&s_swapToken, 0);
		InterlockedDecrement(&s_splitLive);
	}
}

void MissParPopulate(nmResultPopulate_t orig, void* wb, void* local, void* mesh, int param)
{
	int kind = ClassifyRelease(wb);
	// A vanilla dispatch generates on the generator's own work buffer, so this
	// is where that pointer is learned. Only while no swap run has a fresh one
	// installed, and only under the lock, which every populate holds on entry.
	if (t_arm == MP_ARM_SERIAL && NavMeshProcessJobDepth() >= 1)
	{
		if (InterlockedCompareExchange(&s_swapArmed, 0, 0) == 0)
			MissParCanonObserve(s_canon, wb, CANON_AGREE);
		else
			InterlockedIncrement(&s_canonSkipArmed);
	}
	if (kind != MP_REL_NONE)
	{
		InterlockedIncrement(&s_splitLive);
		if (InterlockedCompareExchange(&s_splitBlock, 0, 0) == 0)
		{
			if (kind == MP_REL_SWAP)
			{
				// Re-read under the same hold that classified it. A null here
				// would put null at realNMG+256, which processJobAlt's tail
				// dereferences, so the job generates under the lock instead.
				void* canonical = (void*)MissParCanonConfirmed(s_canon, CANON_AGREE);
				if (canonical != NULL && InterlockedCompareExchange(&s_swapToken, 1, 0) == 0)
				{
					t_miss = MISS_IN_GENERATE;
					InterlockedIncrement(&s_splitReleased);
					InterlockedIncrement(&s_relByKind[MP_ARM_SWAP]);
					GenerateUnlockedSwap(orig, WbSlot(t_realNmg), canonical,
					                     wb, local, mesh, param);
					t_miss = MISS_REACQUIRED;
					return;
				}
				InterlockedDecrement(&s_splitLive);
				if (canonical == NULL)
					InterlockedIncrement(&s_bgPassNoCanon);
				else
					InterlockedIncrement(&s_bgPassToken);
				InterlockedIncrement(&s_splitPassed);
				MissParGenBegin();
				orig(wb, local, mesh, param);
				MissParGenEnd();
				return;
			}
			InterlockedIncrement(&s_relByKind[MP_ARM_CLONE]);
			t_miss = MISS_IN_GENERATE;
			InterlockedIncrement(&s_splitReleased);
			GenerateUnlocked(orig, wb, local, mesh, param);
			if (t_miss == MISS_IN_GENERATE)
				t_miss = MISS_REACQUIRED;
			return;
		}
		// A save-load reset is draining: generate under the lock instead.
		InterlockedDecrement(&s_splitLive);
		InterlockedIncrement(&s_splitBlocked);
		if (kind == MP_REL_SWAP)
			InterlockedIncrement(&s_bgPassBlocked);
	}
	else if (t_arm == MP_ARM_SWAP && navmesh::g_navmeshCfg.navmeshMissSplitEnabled && navmesh::g_navmeshCfg.navmeshMissSplitBgEnabled
	         && MissParCanonConfirmed(s_canon, CANON_AGREE) == NULL)
		InterlockedIncrement(&s_bgPassNoCanon);
	InterlockedIncrement(&s_splitPassed);
	MissParGenBegin();
	orig(wb, local, mesh, param);
	MissParGenEnd();
}

bool MissParLockLost() { return t_miss == MISS_LOCK_LOST; }

void MissParNotePartialSkip() { InterlockedIncrement(&s_partialSkip); }

bool MissParDrainBegin(DWORD timeoutMs, DWORD* waitedMs)
{
	InterlockedIncrement(&s_splitBlock);
	LONGLONG t0 = QpcNow();
	bool drained = false;
	for (;;)
	{
		if (InterlockedCompareExchange(&s_splitLive, 0, 0) == 0) { drained = true; break; }
		if (QpcToMs(QpcNow() - t0) >= (double)timeoutMs) break;
		Sleep(DRAIN_POLL_MS);
	}
	if (waitedMs) *waitedMs = (DWORD)QpcToMs(QpcNow() - t0);
	return drained;
}

void MissParDrainEnd() { InterlockedDecrement(&s_splitBlock); }

void MissParShutdownWake()
{
	if (g_genSlots)
		ReleaseSemaphore(g_genSlots, NAVMESH_WORKER_COUNT, NULL);
}

void InflightScope::Register(const NavMeshCacheKey& key)
{
	InflightKey k;
	k.v[0] = (unsigned)key.gridX;
	k.v[1] = (unsigned)key.gridY;
	k.v[2] = (unsigned)key.sectionTileId;
	k.v[3] = (unsigned)key.jobType;
	k.v[4] = key.aabbHash;
	k.v[5] = key.buildingHash;
	for (int round = 0; round < INFLIGHT_MAX_ROUNDS; ++round)
	{
		InflightResult r = InflightRegisterOrWait(k, INFLIGHT_WAIT_MS, &NavMeshStopSeen, &slot);
		switch (r)
		{
		case INFLIGHT_OWNER:   return;
		case INFLIGHT_WAITED:  InterlockedIncrement(&s_inflightWaits); break;
		case INFLIGHT_FULL:    InterlockedIncrement(&s_inflightFull); return;
		case INFLIGHT_TIMEOUT: InterlockedIncrement(&s_inflightTimeouts); return;
		default:               InterlockedIncrement(&s_inflightStopped); return;
		}
	}
}

void MissParAppendStats(std::ostringstream& ss)
{
	LONG n = InterlockedExchange(&s_jobs, 0);
	double ph[PH_COUNT];
	for (int i = 0; i < PH_COUNT; ++i)
		ph[i] = QpcToMs(InterlockedExchange64(&s_phase[i], 0));
	double held[MP_HOLD_KINDS];
	for (int i = 0; i < MP_HOLD_KINDS; ++i)
		held[i] = QpcToMs(InterlockedExchange64(&s_heldBy[i], 0));

	MissParInterval iv[MP_RING];
	int m = 0;
	LONG written = InterlockedCompareExchange(&s_genWritten, 0, 0);
	if (written - s_genRead > MP_RING) s_genRead = written - MP_RING;
	for (; s_genRead < written; ++s_genRead)
	{
		const GenSlot* s = &s_gen[s_genRead % MP_RING];
		if (InterlockedCompareExchange((volatile LONG*)&s->seq, 0, 0) != s_genRead) continue;
		MissParInterval v = { s->start, s->end };
		if (InterlockedCompareExchange((volatile LONG*)&s->seq, 0, 0) != s_genRead) continue;
		iv[m++] = v;
	}
	__int64 sum = 0;
	__int64 uni = MissParUnion(iv, m, &sum);

	ss << std::fixed << std::setprecision(1) << " missN=" << n;
	if (n > 0)
		ss << " missT=" << ph[PH_WAIT] / n << "/" << ph[PH_SETUP] / n << "/" << ph[PH_GEN] / n
		   << "/" << ph[PH_TAIL] / n << "/" << ph[PH_FIX] / n << "/" << ph[PH_BC] / n << "ms";
	ss << " genOv=" << QpcToMs(uni) << "/" << QpcToMs(sum) << "ms"
	   << " pjHeld=w" << held[MP_HOLD_WMISS] << "/bg" << held[MP_HOLD_BGMISS]
	   << "/t234" << held[MP_HOLD_T234] << "/o" << held[MP_HOLD_OTHER] << "/none" << held[MP_HOLD_NONE] << "ms";

	if (InterlockedCompareExchange(&s_keycodeChecked, 0, 0) == 2)
		ss << " keycode=off";
	else if (MissParKeycodesReady())
		ss << " keycode=ok";
	else
		ss << " keycode=retry" << InterlockedCompareExchange(&s_keycodeRetries, 0, 0);

	LONG waitN = InterlockedExchange(&s_slotWaitN, 0);
	double waitMs = QpcToMs(InterlockedExchange64(&s_slotWaitTicks, 0));
	double reacqMs = QpcToMs(InterlockedExchange64(&s_reacqTicks, 0));
	ss << " split=" << InterlockedExchange(&s_splitReleased, 0) << "/" << InterlockedExchange(&s_splitPassed, 0)
	   << " inflight=" << InterlockedExchange(&s_inflightWaits, 0) << "/" << InterlockedExchange(&s_inflightFull, 0)
	   << "/" << InterlockedExchange(&s_inflightTimeouts, 0)
	   << " slotWait=" << (waitN > 0 ? waitMs / waitN : 0.0) << "ms"
	   << " reacq=" << (waitN > 0 ? reacqMs / waitN : 0.0) << "ms cap=" << g_genCap;
	double reacqHeld[MP_HOLD_KINDS];
	for (int i = 0; i < MP_HOLD_KINDS; ++i)
		reacqHeld[i] = QpcToMs(InterlockedExchange64(&s_reacqHeldBy[i], 0));
	ss << " pjReacqHeld=w" << reacqHeld[MP_HOLD_WMISS] << "/bg" << reacqHeld[MP_HOLD_BGMISS]
	   << "/t234" << reacqHeld[MP_HOLD_T234] << "/o" << reacqHeld[MP_HOLD_OTHER]
	   << "/none" << reacqHeld[MP_HOLD_NONE] << "ms";
	LONG bgN = InterlockedExchange(&s_bgN, 0);
	double bgReacqMs = QpcToMs(InterlockedExchange64(&s_bgReacqTicks, 0));
	ss << " splitBy=w" << InterlockedExchange(&s_relByKind[MP_ARM_CLONE], 0)
	   << "/bg" << InterlockedExchange(&s_relByKind[MP_ARM_SWAP], 0)
	   << " bgReacq=" << (bgN > 0 ? bgReacqMs / bgN : 0.0) << "ms"
	   << " wbCanon=" << (s_canon.disabled ? "off" : (s_canon.agree >= CANON_AGREE ? "ok" : "wait"));
	LONG bgBlk = InterlockedExchange(&s_bgPassBlocked, 0);
	LONG bgCan = InterlockedExchange(&s_bgPassNoCanon, 0);
	LONG bgTok = InterlockedExchange(&s_bgPassToken, 0);
	if (bgBlk || bgCan || bgTok)
		ss << " bgPass=" << bgBlk << "/" << bgCan << "/" << bgTok;
	LONG wbRace = InterlockedExchange(&s_wbSlotRace, 0);
	if (wbRace)
		ss << " wbSlotRace=" << wbRace;
	LONG canonSkip = InterlockedExchange(&s_canonSkipArmed, 0);
	if (canonSkip)
		ss << " canonSkip=" << canonSkip;
	LONG stopped = InterlockedExchange(&s_inflightStopped, 0);
	LONG skipped = InterlockedExchange(&s_slotStopSkip, 0);
	LONG lost    = InterlockedExchange(&s_lockLost, 0);
	if (stopped || skipped || lost)
		ss << " splitStop=" << stopped << "/" << skipped << "/" << lost;
	LONG blocked = InterlockedExchange(&s_splitBlocked, 0);
	if (blocked)
		ss << " splitBlk=" << blocked;
	LONG pgSkip = InterlockedExchange(&s_partialSkip, 0);
	if (pgSkip)
		ss << " pgSkip=" << pgSkip;
}

