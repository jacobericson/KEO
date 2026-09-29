// readiness_hook.cpp - Readiness classification and per-caller answers.
// Main, AI and spawn threads; try-lock +0x200 shared, release it, then try +0x1E0
// exclusive. Never hold both locks. Side effects stay on main; the answer is
// computed on every thread and the per-zone ready byte is never written.

#include "zone/preload/preload.h"
#include "zone/transition.h"
#include "zone/transition_hook.h"
#include "zone/readiness/readiness_hook.h"
#include "zone/hooks_internal.h"
#include "movement/islands.h"
#include "zone/readiness/zone_readiness_contract.h"
#include "zone/readiness/zone_readiness_ledger_bridge.h"
#include "zone/readiness/zone_readiness_classify.h"



// =========================================================================
// Readiness classification + per-caller rule
// =========================================================================
//
// Section-state read design:
//
// The rule needs one fact per zone: is the zone's outdoor navmesh instance in
// the world? The walk in contentStream (0x3AEA42) tests it as state+0x28 != NULL
// with +0x1A4 >= 0. Reading it through state+0x28 from another thread is not
// safe: removeNavInstance (0x3AB8A0, path thread) removes the instance from the
// world under +0x200, RELEASES +0x200, then deletes the instance and only then
// NULLs +0x28. And the map at +0x220 is not only written under +0x1E0: the
// path thread's addZoneSection (0x3AD800, from processZoneWorkItem) inserts
// with operator[] after processZoneWorkItem has released +0x1E0.
//
// So the outdoor test goes through the world instead, the way the game's own
// navmesh queries do: try-lock +0x200 SHARED with the exact inline sequence of
// NavMesh__snapToFace (0x3A1F70), give up if it fails, and scan the main
// world's streaming collection for the instance whose section uid is
// gridX | (gridY << 8) (hkaiStreamingCollection's own lookup, 0xD0CD40). Every
// add and every world removal takes +0x200 exclusively, and the delete comes
// after the removal, so while we hold it shared each instance in the
// collection is alive and its +0x1A4 is stable. Found with +0x1A4 >= 0 is
// exactly "outdoor instance in the world". Held for the scan only.
//
// Uid collisions. Building sub-instances share the collection and its uid
// space. A runtime building uid is container | (index+1) << 16 | serial << 26
// (0x3A1440, over the building's hand: container +0xC, index +0x14, serial
// +0x18); file-loaded ones are parsed from the navmesh file's info string
// (insertSection 0x3A822A), which carries the same value. A building can
// equal a zone key gx | gy << 8 (<= 0x3F3F) only if ((index+1) & 0xFFFF) == 0
// AND (serial & 0x3F) == 0 AND its container's low 16 bits happen to be that
// zone key: a handle index of exactly 65535 + k*65536, a serial divisible by
// 64 and a zone-shaped container, all at once, for a building whose navmesh is
// in the world. The game makes the same assumption itself: contentStream
// treats a section uid below 0xFFFF as a zone (0x3AE6AD, the NavBuildLock
// key). No cheaper per-instance discriminator exists: outdoor and building
// instances are created by the same function (0x3ACD20 via 0xD09EB0) and
// carry no type field; the only other tie, section state +0x28, is exactly
// the pointer that cannot be read safely (above).
//
// Only when the scan says "not in the world" is the map consulted, to tell
// "no section" from "section, outdoor mesh missing" (diagnostic only; the rule
// answers 0 for both). That takes +0x1E0 with a try-exclusive (the inline
// sequence the original uses on +0x200 at 0x3AB59D; the original itself takes
// +0x1E0 with a blocking timed lock), calls the game's lookupSection -- the
// same lookup the original has just done -- and compares pointers only. The
// state object is never dereferenced. If that try fails, or the split is
// skipped, the class is "not in world" (RZ_NOT_IN_WORLD): the fact the rule
// needs was read, only the split is missing, so the rule still answers 0.
// Off the main thread with the rule on the split is skipped altogether, so the
// rule path adds no contention against the blocking map lookups of the game's
// threads; it runs on the main thread and in the sampled rule-off calls.
//
// The two locks are never held together. Both are try-locks, so no thread ever
// blocks here. Releases go through the game's own unlock/unlock_shared.
//
// Rejected: a per-zone table built on the main thread and published by
// seqlock. It would read the same world under the same lock, one frame stale,
// and its build would walk the whole section map every frame, which widens the
// exposure to the path thread's unlocked map insert from one lookup to a full
// traversal.

// boost::shared_mutex state word (boost 1.4x win32 layout, confirmed by the
// game's unlock 0x25C3D0 and unlock_shared 0x168E10): bits 0-10 shared count,
// bit 22 exclusive, bit 31 exclusive_waiting_blocked. External linkage: other
// translation units take the same try-lock on other ZoneMapContent-family
// mutexes (declared in game.h).
const LONG BOOST_SHARED_MASK = 0x7FF;
const LONG BOOST_EXCLUSIVE   = 0x400000;

// try_lock_shared, as inlined in NavMesh__snapToFace 0x3A1F70: fails when an
// exclusive owner holds it or one is waiting (sign bit), else adds a reader.
bool BoostTryLockShared(volatile LONG* state)
{
	LONG cur = *state;
	for (;;)
	{
		if ((cur & BOOST_EXCLUSIVE) != 0 || cur < 0)
			return false;
		LONG next = (cur & ~BOOST_SHARED_MASK) | ((cur + 1) & BOOST_SHARED_MASK);
		if ((next & BOOST_SHARED_MASK) == 0)
			return false;                           // reader count would overflow
		LONG prev = InterlockedCompareExchange(state, next, cur);
		if (prev == cur)
			return true;
		cur = prev;
	}
}

// try_lock, as inlined in SectionManager__isContentPending 0x3AB59D: fails
// while any reader or the exclusive owner holds it.
static bool BoostTryLockExclusive(volatile LONG* state)
{
	LONG cur = *state;
	for (;;)
	{
		if ((cur & BOOST_SHARED_MASK) != 0 || (cur & BOOST_EXCLUSIVE) != 0)
			return false;
		LONG prev = InterlockedCompareExchange(state, cur | BOOST_EXCLUSIVE, cur);
		if (prev == cur)
			return true;
		cur = prev;
	}
}

namespace readiness_hook_detail {

enum ReadyCaller { RC_POLL4 = 0, RC_MAIN = 1, RC_OFF = 2, RC_COUNT = 3 };
// RZ_NOT_IN_WORLD: the scan read "outdoor instance not in the world", but the
// noSection/outdoorMissing split was skipped or its map try-lock failed.
// RZ_UNKNOWN: the +0x200 try failed (or the collection was unreadable), so
// the outdoor fact itself was not read.
enum ReadyClass  { RZ_NO_SECTION = 0, RZ_OUTDOOR_MISSING = 1, RZ_BUILDINGS_PENDING = 2,
                   RZ_NOT_IN_WORLD = 3, RZ_UNKNOWN = 4, RZ_COUNT = 5 };

// Written on any caller thread by hook_isContentPending; scan-cost fields
// also come from ClassifyZoneReadiness through RecordScanCost. The main
// ReadinessReportTick reads each atomic separately. Publication is each
// Interlocked update, not a coherent set; mixed diagnostic counters are
// tolerated. Never reset: cumulative for the session.
struct ReadinessCounters
{
	volatile LONG g_rdyCls[RC_COUNT][RZ_COUNT];
	volatile LONG g_rdyBypass;   // ready answers from the sections == 0 bypass
	volatile LONG g_rdyRuleReady;   // rule answers, ready
	volatile LONG g_rdyRuleWait;   // rule answers, not ready
	// Collection scan QPC cost on all caller threads; max uses a CAS loop.
	volatile LONG   g_rdyScans;
	volatile LONG64 g_rdyScanTicks;
	volatile LONG64 g_rdyScanMax;
	volatile LONG64 g_rdyScanSlots;   // collection size at each scan, summed
	// Samples off-main rule-off calls once per RDY_OFF_SAMPLE_MASK + 1.
	volatile LONG g_rdyOffSample;
};
static ReadinessCounters g_rdy;
union ReadinessCountersPodCheck { ReadinessCounters s; };
static_assert(__alignof(ReadinessCounters) >= 8, "ReadinessCounters must be 8-byte aligned");

#if ZONEHAND_STEP >= 2
// Any hook caller atomically counts the contract class before its answer:
// private, adopted or global bypass. The main reporter reads independently
// published counters, tolerating mixed diagnostic values. No session reset.
struct ReadinessContractCounters
{
	volatile LONG g_rdyContractPrivate;
	volatile LONG g_rdyContractAdopted;
	volatile LONG g_rdyContractGlobal;
};
static ReadinessContractCounters g_rdyContract;
union ReadinessContractCountersPodCheck { ReadinessContractCounters s; };
#endif

const LONG RDY_OFF_SAMPLE_MASK = 15;   // 1 in 16 off-main rule-off calls
const int RDY_SC_MAX_SCAN = 8192;      // defensive collection-scan cap

// Any thread: records one scan's cost (Interlocked only).
static void RecordScanCost(LONG64 ticks, int slots)
{
	if (ticks < 0)
		ticks = 0;
	InterlockedIncrement(&g_rdy.g_rdyScans);
	InterlockedExchangeAdd64(&g_rdy.g_rdyScanTicks, ticks);
	InterlockedExchangeAdd64(&g_rdy.g_rdyScanSlots, (LONG64)slots);
	LONG64 cur = InterlockedCompareExchange64(&g_rdy.g_rdyScanMax, 0, 0);
	while (ticks > cur)
	{
		LONG64 prev = InterlockedCompareExchange64(&g_rdy.g_rdyScanMax, ticks, cur);
		if (prev == cur)
			break;
		cur = prev;
	}
}

} // namespace
using namespace readiness_hook_detail;

// Any thread: no allocation, no logging, never blocks. splitMap = false skips
// the +0x1E0 noSection/outdoorMissing split (returns RZ_NOT_IN_WORLD instead).
//
// External linkage: the island re-issue readiness backstop (island_orders.cpp,
// k7_observe.cpp, k7_reissue.cpp, via zone_readiness_classify.h) calls it. The
// RZ_* names and helpers it uses come from readiness_hook_detail above.
int ClassifyZoneReadiness(uintptr_t mgr, const int* pos, bool splitMap)
{
	if (!mgr || !pos || !fn_lookupSection || !fn_boostUnlock || !fn_boostUnlockShared)
		return RZ_UNKNOWN;
	int gx = *(const int*)KLIB_MEMBER(5, pos, iVector2_x, 0);
	int gy = *(const int*)KLIB_MEMBER(5, pos, iVector2_y, 4);
	if (gx < 0 || gx > ZONE_GRID_MAX || gy < 0 || gy > ZONE_GRID_MAX)
		return RZ_UNKNOWN;
	unsigned int uid = (unsigned int)gx | ((unsigned int)gy << 8);

	// 1. Outdoor instance in the world (under +0x200 shared).
	volatile LONG* worldLock = (volatile LONG*)(KLIB_MEMBER(4, mgr, NavMesh_changeMutex, OFF_RDY_SM_WORLD_LOCK));
	if (!BoostTryLockShared(worldLock))
		return RZ_UNKNOWN;
	int outdoor = -1;                              // -1 unreadable, 0 not in, 1 in
	int scanSlots = -1;                            // >= 0 once a scan ran
	LARGE_INTEGER t0, t1;
	t0.QuadPart = 0;
	t1.QuadPart = 0;
	uintptr_t world = *(uintptr_t*)(KLIB_MEMBER(4, mgr, NavMesh_world, OFF_RDY_SM_WORLD));
	uintptr_t coll  = world ? *(uintptr_t*)(world + OFF_RDY_WORLD_COLLECTION) : 0;
	if (coll)
	{
		int count      = *(int*)(KLIB_MEMBER(5, coll + 32, ByteArray_m_size, 8));
		uintptr_t data = *(uintptr_t*)(KLIB_MEMBER(5, coll + 32, ByteArray_m_data, 0));
		if (count >= 0 && count <= RDY_SC_MAX_SCAN && (count == 0 || data))
		{
			QueryPerformanceCounter(&t0);
			outdoor = 0;
			for (int k = 0; k < count; ++k)
			{
				uintptr_t inst = *(uintptr_t*)(data + (uintptr_t)k * RDY_SC_ENTRY_SIZE);
				if (!inst || *(unsigned int*)(inst + OFF_RDY_NMI_SECTION_UID) != uid)
					continue;
				if (*(int*)(inst + OFF_RDY_NMI_RUNTIME_INDEX) >= 0)
					outdoor = 1;
				break;                             // first match, as 0xD0CD40
			}
			QueryPerformanceCounter(&t1);
			scanSlots = count;
		}
	}
	fn_boostUnlockShared((void*)worldLock);
	if (scanSlots >= 0)
		RecordScanCost(t1.QuadPart - t0.QuadPart, scanSlots);   // after the release
	if (outdoor < 0)
		return RZ_UNKNOWN;
	if (outdoor > 0)
		return RZ_BUILDINGS_PENDING;   // the original said 0, so the ready byte is clear

	// 2. Section entry present? (under +0x1E0 exclusive; diagnostic split only)
	// The outdoor fact is known from here on: a skipped split or a lost try
	// is "not in world", never "unknown".
	if (!splitMap)
		return RZ_NOT_IN_WORLD;
	volatile LONG* mapLock = (volatile LONG*)(KLIB_MEMBER(4, mgr, NavMesh_mutex, OFF_RDY_SM_MAP_LOCK));
	if (!BoostTryLockExclusive(mapLock))
		return RZ_NOT_IN_WORLD;
	void* node = NULL;
	fn_lookupSection((void*)(KLIB_MEMBER(4, mgr, NavMesh_sectors, OFF_RDY_SM_SECTION_MAP)), &node, pos);
	void* head = *(void**)(KLIB_MEMBER(4, mgr, NavMesh_sectors__Myhead, OFF_RDY_SM_SECTION_MAP_HEAD));
	bool hasSection = node && node != head
	               && *(void**)(KLIB_MEMBER(4, (uintptr_t)node, SectorNode_mapped, OFF_RDY_MAPNODE_VALUE)) != NULL;
	fn_boostUnlock((void*)mapLock);
	return hasSection ? RZ_OUTDOOR_MISSING : RZ_NO_SECTION;
}

namespace readiness_hook_detail {

// Main thread only. The "state-4 poll" class: main thread and zone manager in
// state 4, for ANY zone. The zone's accessibility
// is deliberately not tested: the state machine (0xA0E950) can poll a zone
// that is already accessible, and giving that call the rule's per-zone answer
// would put an uncapped wait into the state-4 answer, which can freeze the
// game (the pause near inaccessible zones while the state is not 0). The
// accepted cost: a main-thread setDestination during state 4 keeps today's
// lenient sections == 0 answer. A flag set by a hook around
// ZoneManager__stateMachineDriver 0xA0E950 would tell the poll apart exactly
// (not done now).
static bool IsStatePoll4()
{
	void* zm = g_cachedZoneMgr;
	return zm && GetZoneState(zm) == 4;
}

static inline LONG ReadCounter(volatile LONG* c)
{
	return InterlockedCompareExchange(c, 0, 0);
}

} // namespace
using namespace readiness_hook_detail;

namespace hooks_detail
{ // namespace hooks_detail

// Main thread, every hook_updateCameraZone frame: the one-time config line,
// the deferred target line, and the periodic Readiness line (10 s DEV, 30 s
// PROD, only when a counter moved).
void ReadinessReportTick(double now)
{
	static bool configLogged = false;
	if (!configLogged)
	{
		configLogged = true;
		std::ostringstream cs;
		cs << "Readiness config: readinessOverrides="
		   << (zone::g_zoneCfg.readinessOverridesEnabled ? "ON" : "OFF")
		   << " islandReadinessRule=" << (zone::g_zoneCfg.islandReadinessRuleEnabled ? "ON" : "OFF")
		   << " deferral=" << (zone::g_zoneCfg.deferralEnabled ? "ON" : "OFF");
		LogMsg(cs.str());
	}

	if (InterlockedCompareExchange(&g_tgtLogPending, 0, 1) == 1)
		LogTransitionTarget();

#ifdef ZONEOPT_DEBUG
	const double kReadinessLogIntervalSec = 10.0;
#else
	const double kReadinessLogIntervalSec = 30.0;
#endif
	static double        lastLog = 0.0;
	static unsigned long lastSig = 0;
	if (now - lastLog < kReadinessLogIntervalSec)
		return;

	// Every counter only grows, so the (wrapping, unsigned) sum changes
	// whenever one of them moved.
	LONG v[RC_COUNT][RZ_COUNT];
	unsigned long sig = 0;
	for (int c = 0; c < RC_COUNT; ++c)
		for (int z = 0; z < RZ_COUNT; ++z)
		{
			v[c][z] = ReadCounter(&g_rdy.g_rdyCls[c][z]);
			sig += (unsigned long)v[c][z];
		}
	LONG bypass = ReadCounter(&g_rdy.g_rdyBypass);
	LONG rReady = ReadCounter(&g_rdy.g_rdyRuleReady);
	LONG rWait  = ReadCounter(&g_rdy.g_rdyRuleWait);
	LONG   scans     = ReadCounter(&g_rdy.g_rdyScans);
	LONG64 scanTicks = InterlockedCompareExchange64(&g_rdy.g_rdyScanTicks, 0, 0);
	LONG64 scanMax   = InterlockedCompareExchange64(&g_rdy.g_rdyScanMax, 0, 0);
	LONG64 scanSlots = InterlockedCompareExchange64(&g_rdy.g_rdyScanSlots, 0, 0);
#if ZONEHAND_STEP >= 2
	LONG cPriv = ReadCounter(&g_rdyContract.g_rdyContractPrivate);
	LONG cAdopt = ReadCounter(&g_rdyContract.g_rdyContractAdopted);
	LONG cGlobal = ReadCounter(&g_rdyContract.g_rdyContractGlobal);
#endif
	sig += (unsigned long)bypass + (unsigned long)rReady + (unsigned long)rWait
	     + (unsigned long)scans
#if ZONEHAND_STEP >= 2
	     + (unsigned long)cPriv + (unsigned long)cAdopt + (unsigned long)cGlobal
#endif
	     ;
	if (sig == lastSig)
		return;                                     // nothing new to report
	lastSig = sig;
	lastLog = now;

	// Off-main calls are sampled 1 in 16 while the rule is off (see
	// g_rdyOffSample); the label says so.
	const char* offName = zone::g_zoneCfg.islandReadinessRuleEnabled ? "off" : "off(1/16)";
	const char* callerName[RC_COUNT] = { "poll4", "main", offName };

	double freq     = (double)qpcFrequency.QuadPart;
	double scanAvg  = (scans > 0 && freq > 0.0) ? (double)scanTicks * 1e6 / freq / (double)scans : 0.0;
	double scanMaxU = (freq > 0.0) ? (double)scanMax * 1e6 / freq : 0.0;
	double slotsAvg = (scans > 0) ? (double)scanSlots / (double)scans : 0.0;

	std::ostringstream ss;
	ss << "Readiness:";
	for (int c = 0; c < RC_COUNT; ++c)
		ss << " " << callerName[c] << " ns/om/bp="
		   << v[c][RZ_NO_SECTION] << "/" << v[c][RZ_OUTDOOR_MISSING] << "/"
		   << v[c][RZ_BUILDINGS_PENDING];
	ss << " bypass=" << bypass
#if ZONEHAND_STEP >= 2
	   << " rule=superseded"   // the readiness contract answers every cell before this rule is reached
#else
	   << " rule=" << ((zone::g_zoneCfg.islandReadinessRuleEnabled && zone::g_zoneCfg.readinessOverridesEnabled) ? "on" : "off")
#endif
	   << " ruleReady=" << rReady << " ruleWait=" << rWait
#if ZONEHAND_STEP >= 2
	   << " contract=" << cPriv << "/" << cAdopt << "/" << cGlobal
#endif
	   << " unk=" << v[RC_POLL4][RZ_UNKNOWN] << "/" << v[RC_MAIN][RZ_UNKNOWN] << "/"
	   << v[RC_OFF][RZ_UNKNOWN]
	   << " niw=" << v[RC_POLL4][RZ_NOT_IN_WORLD] << "/" << v[RC_MAIN][RZ_NOT_IN_WORLD] << "/"
	   << v[RC_OFF][RZ_NOT_IN_WORLD];
	ss << std::fixed << std::setprecision(1)
	   << " scan=" << scanAvg << "/" << scanMaxU << "us"
	   << std::setprecision(0) << " slots=" << slotsAvg;
	if (!zone::g_zoneCfg.readinessOverridesEnabled)
		ss << " overrides=off";
	LogMsg(ss.str());
}

} // namespace hooks_detail
using namespace ::hooks_detail;

// =========================================================================
// Hook 2: isContentPending -- THE navmesh deferral
// =========================================================================
//
// The original returns the zone's per-zone ready byte (section state +0x50,
// set by contentStream once the outdoor instance and every building
// sub-instance are in the world); its global fallback sets the byte when
// nothing is pending anywhere (sections +632 == 0, generator idle). Returns
// 1 = ready, 0 = still loading.
//
// When the original says 0 (and deferral is on):
//   - readinessOverrides=false: 0, for every caller.
//   - ZONEHAND_STEP >= 2: the readiness contract (zone_readiness_contract.h)
//     decides for every cell, and the rule below is never reached. While
//     ZM+8 (justLoadedAGame) is set, or the cell is classified private or
//     adopted, 0 unmodified; every other cell (unclassified, i.e. the mod
//     never reached it) falls to the sections==0 bypass below. Below step 2
//     the contract is skipped and the next two rules decide.
//   - islandReadinessRule=true, any caller but the state-4 poll (main thread
//     with the zone manager in state 4, any zone), outdoor fact read (class
//     not RZ_UNKNOWN): 1 iff the zone's outdoor instance is in the world.
//   - otherwise (today's rule): 1 once sections == 0. The navmesh generator
//     continues in the background.
// The ready byte is never written.

// Threading: isContentPending is also reached through isZoneStillLoading
// (0x3AC810) from CharMovement::setDestination (AI evaluation), the
// spawn-check thread and character creation. The return value is computed
// identically on every thread; the side effects (queue reprioritization,
// deferred-frame accounting, LogDebug's ostringstream) run only on the main
// thread. Per-thread call/not-ready counters feed the Islands diagnostic line.

bool hook_isContentPending(void* manager, void* zonePos)
{
	bool onMainThread = IsMainThread();

	// Prioritize navmesh queue once per transition, at start of state 4.
	// By state 4, processState3 has finished registerZoneSections ->
	// contentStream has submitted all navmesh jobs for this transition.
	// Reordering now promotes the transition zone's jobs to the front.
	if (onMainThread && isTransitionActive && !prioritizedThisTransition)
	{
		CallPrioritizeNavMeshQueue();
		prioritizedThisTransition = true;
	}

	bool result = game::g_hookOrig.orig_isContentPending(manager, zonePos);
	int sectionCount = *(int*)(KLIB_MEMBER(4, (uintptr_t)manager, NavMesh_addList_count, OFF_RDY_SM_PENDING_SECTIONS));
	IslandCountReadiness(!result, !result && sectionCount > 0);

#if ZONEHAND_STEP >= 2
	// The contract's two inputs, read for every call the hook is asked
	// about -- ahead of the answers that are already settled (the original
	// said ready, or deferral and the overrides switch are off). Both reads
	// are single aligned loads with no side effect and no lock, so no answer
	// on any path changes by taking them here; what changes is that the
	// counters report the class of every cell the hook was asked about,
	// instead of only the cells whose answer the contract got as far as
	// deciding. A cell class of zero means the hook was asked about a cell
	// the ledger does not know, which is most calls and takes no atomic.
	//
	// ZM+8 is read live rather than from a per-frame mirror: a save load
	// sets the flag partway through its own synchronous body, with no frame
	// boundary inside that call for a sample to observe it at, so a mirror
	// can read stale for the whole span of the load. Reading it once here
	// and using that value at the decision point below keeps a single call's
	// two uses consistent with each other.
	int gxContract = *(const int*)KLIB_MEMBER(5, (const int*)zonePos, iVector2_x, 0);
	int gyContract = *(const int*)KLIB_MEMBER(5, (const int*)zonePos, iVector2_y, 4);
	uintptr_t zmForContract = (uintptr_t)g_cachedZoneMgr;
	bool justLoadedAGame = zmForContract != 0
		&& *(unsigned char*)(KLIB_MEMBER(2, zmForContract, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;
	int cellClassContract = ZoneReadinessBridgeClassOf(gxContract, gyContract);
	switch (ZoneReadinessContractBucket(cellClassContract, justLoadedAGame))
	{
	case ZONE_READY_BUCKET_GLOBAL:  InterlockedIncrement(&g_rdyContract.g_rdyContractGlobal);  break;
	case ZONE_READY_BUCKET_ADOPTED: InterlockedIncrement(&g_rdyContract.g_rdyContractAdopted); break;
	case ZONE_READY_BUCKET_PRIVATE: InterlockedIncrement(&g_rdyContract.g_rdyContractPrivate); break;
	case ZONE_READY_BUCKET_NONE:    break;   // no counter: the bypass count already covers these
	}
#endif

	if (result)
		return true;

	if (!zone::g_zoneCfg.deferralEnabled)
		return false;

	// Classification. The caller class uses the thread and, on the main
	// thread, the zone manager state (never read off it, where the class is
	// always "off"). Main-thread calls are always classified, and so is every
	// call while the rule is on (its answer depends on the class). Off the
	// main thread with the rule off, only 1 call in 16 is classified; the other
	// 15 take neither lock and are not counted, and their answer (today's
	// rule, below) does not depend on the class.
	const int* pos = (const int*)zonePos;
	int caller = RC_OFF;
	if (onMainThread)
		caller = IsStatePoll4() ? RC_POLL4 : RC_MAIN;
	bool classify = onMainThread || zone::g_zoneCfg.islandReadinessRuleEnabled
	             || (InterlockedIncrement(&g_rdy.g_rdyOffSample) & RDY_OFF_SAMPLE_MASK) == 0;
	// The +0x1E0 split (diagnostic) runs on the main thread and in the sampled
	// rule-off calls; off the main thread with the rule on it is skipped, so
	// the rule path adds no contention against the game's blocking map lookups.
	bool splitMap = onMainThread || !zone::g_zoneCfg.islandReadinessRuleEnabled;
	int cls = RZ_UNKNOWN;
	if (classify)
	{
		cls = ClassifyZoneReadiness((uintptr_t)manager, pos, splitMap);
		InterlockedIncrement(&g_rdy.g_rdyCls[caller][cls]);
	}

	// readinessOverrides off: the original's answer for every caller. The queue
	// reprioritization above has already run.
	if (!zone::g_zoneCfg.readinessOverridesEnabled)
		return false;

#if ZONEHAND_STEP >= 2
	// The readiness contract: the per-cell class wins over islandReadinessRule
	// outright, for every cell -- see zone_readiness_contract.h. Below step 2
	// the contract does not exist at all (the #else arm below), and the rule
	// alone decides.
	//
	// The inputs were read at the top of this call and are used here as read:
	// neither depends on `classify`/`caller` (thread- and sample-dependent
	// diagnostics) or on which thread calls, so the same (gx, gy, classWord,
	// ZM+8) always gives the same answer. classWord is published once per
	// main-thread frame (lock-free, no lock), so a call on another thread can
	// see last frame's class until the next publish -- never a torn one.
	// Adopted and private cells, and every cell while ZM+8 is set, answer
	// false here (the original already said so, unmodified); every other cell
	// drops straight to today's sections==0 bypass below.
	if (ZoneReadinessContractDecide(cellClassContract, justLoadedAGame) == ZONE_READY_ANSWER_ORIGINAL)
		return false;
	// Today's bypass otherwise: fall to the sections==0 tail below.
	// islandReadinessRule is not consulted at this step -- see the #else arm.
#else
	// The per-caller rule: every caller but the state-4 poll (main thread in
	// state 4, any zone) is ready exactly when the zone's outdoor instance is
	// in the world, whatever the global section count. noSection,
	// outdoorMissing and notInWorld all answer 0; only RZ_UNKNOWN (the outdoor
	// fact was not read) falls through to today's rule.
	if (zone::g_zoneCfg.islandReadinessRuleEnabled && classify && caller != RC_POLL4 && cls != RZ_UNKNOWN)
	{
		bool ready = (cls == RZ_BUILDINGS_PENDING);
		InterlockedIncrement(ready ? &g_rdy.g_rdyRuleReady : &g_rdy.g_rdyRuleWait);
		return ready;
	}
#endif

	if (sectionCount > 0)
		return false;
	InterlockedIncrement(&g_rdy.g_rdyBypass);

	// Sections drained: return ready regardless of navmesh state.
	// Macro-transitions: the original navmesh-wait deferral (95% faster state 4).
	// Micro-transitions (platoon activation bumps state 0->1 without
	// showLoadingMessage): prevents NavMeshGenerator idle stall from
	// causing state 4 pause. NPCs use fallback pathfinding briefly.
	if (!onMainThread)
		return true;

	if (isTransitionActive)
		deferredFrameCount++;
	else
	{
		static double lastMicroDeferLog = 0.0;
		double t = ElapsedSec();
		if (t - lastMicroDeferLog > 5.0)
		{
			LogDebug("Micro-transition deferral: sections=0, skipping navmesh wait");
			lastMicroDeferLog = t;
		}
	}
	return true;
}
