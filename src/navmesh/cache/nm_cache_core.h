// nm_cache_core.h — L1 in-memory navmesh cache + stats reporter (Layer 3)
// Depends on: nm_cache_types.h, nm_cache_slot_policy.h

#ifndef KENSHI_ZONE_OPT_NM_CACHE_CORE_H
#define KENSHI_ZONE_OPT_NM_CACHE_CORE_H

#include "navmesh/cache/nm_cache_types.h"
#include "navmesh/cache/nm_cache_slot_policy.h"


// A claimed job whose zone the game unloaded while the
// job waited on processJobCS. Ungated: the re-check restores vanilla's own test
// (dispatchJob_orig 0x3CE030 drops a job whose zone has no mapContent), and the
// counters are written on the NavMesh bg thread and the
// workers with Interlocked* only (no allocation, no logging there), read by the
// main-thread stats reporter, cumulative since session start.
//
// processJobCS wait, per acquisition site: time blocked acquiring the lock.
//   PJWAIT_CLONE  — the worker's CloneNMG snapshot lock
//   PJWAIT_WMISS  — missLock in ProcessNavMeshJob on a worker thread
//   PJWAIT_BGMISS — missLock in ProcessNavMeshJob on the NavMesh bg thread
// Reported as pjWait clone=<avg>/<max> wMiss=<avg>/<max> bgMiss=<avg>/<max>ms.
enum PjWaitSite
{
	PJWAIT_CLONE      = 0,
	PJWAIT_WMISS      = 1,
	PJWAIT_BGMISS     = 2,
	PJWAIT_SITE_COUNT = 3
};

// Claim age: QPC at the job's unlink from the generator queue -> the zone
// re-check. MISS: at the authoritative re-check right after missLock is
// acquired (worker and bg thread). HIT: at the re-check at the top of
// WorkerProcessHit, just before the reconstruct (worker HITs only; the bg
// thread's own HIT path never waits on processJobCS). Reported as
// claimAge miss=<avg>/<max> hit=<avg>/<max>ms and, for MISSes only, the bucket
// counts [<10:a <100:b <1s:c <5s:d >=5s:e].
// A HIT whose slot turned stale (hitStale=) is counted in hit= and
// again in miss= when it regenerates, so hit= + miss= is not a job count.
enum { CLAIMAGE_BUCKET_COUNT = 5 };

// Claimed jobs dropped because their zone was unloaded, the way vanilla's early
// return drops them (not freed, not enqueued). Reported as
// stale=w<n>/bg<n>/hit<n>/early<n> staleLast=(x,y)t<type>/<reason>@<ms>ms.
// A job dropped because NavMesh::stop was seen takes the
// same exits with reason shutdown. Those are also counted on the
// worker retire line as stopDrop=, and since no stats line follows
// NavMesh::stop, stale= on the stats line in practice never includes them.
// A job that waited out a save-load reset and lost its zone's content counts
// at its path's site; dropped at a collision build, it is freed, not enqueued.
enum StaleSite
{
	STALE_SITE_WMISS  = 0,   // worker, after missLock
	STALE_SITE_BGMISS = 1,   // bg thread, after missLock
	// a HIT dropped for an unloaded zone: a worker's before the rebuild, any
	// thread's after a reset wait
	STALE_SITE_HIT    = 2,
	STALE_SITE_EARLY  = 3,   // worker MISS, before CloneNMG
	STALE_SITE_COUNT  = 4
};
enum StaleReason
{
	STALE_REASON_NONE       = 0,
	STALE_REASON_NO_ZONE    = 1,   // job+0 (ZoneMap*) is NULL
	STALE_REASON_NO_CONTENT = 2,   // ZoneMap+0 (mapContent) is NULL
	STALE_REASON_NO_TERRAIN = 3,   // ZoneMap+0xB8 (terrainCollision) is NULL
	STALE_REASON_SHUTDOWN   = 4    // NavMesh::stop seen (also stopDrop=)
};
// lazyHooksInstalled: the lazy install's progress. The NavMesh bg thread moves
// it from not started to running with a compare-exchange, and to done last.
enum NmLazyInstallState { NM_LAZY_NOT_STARTED = 0, NM_LAZY_RUNNING = 1, NM_LAZY_DONE = 2 };
// g_nbrSeedHookState: the neighbour-seed hook's install outcome.
enum NmNbrSeedHookState { NBRSEED_HOOK_UNTRIED = 0, NBRSEED_HOOK_OK = 1, NBRSEED_HOOK_FAILED = 2 };
namespace navmesh {

// The navmesh cache's counters, probes and flags. The counters and flags are
// written with Interlocked* (no allocation, no logging) on the NavMesh bg
// thread and the workers, by the collision builders, the work-buffer probe and
// the L2 reader and writer; on the main thread by the cache's init and clear,
// the zone unloader, the save-load reset and the stats reporter, which advances
// its own l2MissLogReported cursor. Three groups take plain stores into storage
// their writer claimed first: the settings and work-buffer probe buffers
// (probeEMP, probeGen, probeMisc, verifyEMP, wbArrayProbes, wbArrayOffsets,
// wbWritableOffsets), filled once behind an Interlocked latch; each L2 miss log
// record, in the slot its InterlockedIncrement of l2MissLogCount claimed; and
// nmDiskCacheDirBuf (strcpy_s) and nmDiskCacheDirChecked, on the main thread in
// InitNavMeshCacheCS. The main thread's stats reporter reads the counters
// through InterlockedCompareExchange(&x, 0, 0). Read off the main thread as
// behaviour inputs: nmDiskCacheDirBuf, written once before any hook installs,
// from which the L2 reader and writer build every path; nmCacheDisabled,
// nmDiagStage (fixed after start-up), workerBusyCount (also by the crash
// recorder), lazyHooksInstalled, g_l2Bypass, g_nbrSeedHookState,
// g_nbrSeedStandInRefused and g_navMeshWorkersLive. g_navMeshPoolRefusal is a
// main-thread diagnostic read; g_workBufAllocSize is a probe result with no
// reader. The "last" records (nmDiagLast*, nmStaleLast*, nmZeroFaceLast*) are
// separate stores that a racing reader can see torn; each reports only the most
// recent event, so that is tolerated.
struct NavMeshCacheState
{
	// Members with a non-zero initial value, in the order the initialiser in
	// nm_cache_core.cpp lists them.
	int           nmDiagStage;           // 0=bypass, 1=dequeue only, 2=full cache
	volatile long nmDiagLastGridX;
	volatile long nmDiagLastGridY;
	volatile long nmDiagLastType;
	volatile long nmDiagHitGrid;
	volatile long nmStaleLastGridX;
	volatile long nmStaleLastGridY;
	volatile long nmStaleLastType;
	volatile long nmStaleLastReason;
	volatile long nmZeroFaceLastTri;
	volatile long nmZeroFaceLastVert;
	volatile long nmZeroFaceLastThings;
	volatile long nmZeroFaceLastGridX;
	volatile long nmZeroFaceLastGridY;
	volatile long nmZeroFaceLastType;
	volatile long g_wbOverrideAfterPop;
	volatile long nmCacheDisabled;

	// Diagnostic counters (written by bg threads via Interlocked, read by main thread)
	volatile long nmJobCount;
	volatile long nmCacheHitCount;
	volatile long nmCacheMissCount;
	volatile long nmCacheSkipCount;
	volatile long nmTotalMsTimes10;
	volatile long nmSavedMsTimes10;
	volatile long nmDiagStep;

	// Settings probe buffers (written once by bg thread, dumped by stats reporter)
	volatile long nmSettingsDumped;
	volatile long nmSettingsVerified;
	volatile float probeEMP[14];
	volatile float probeGen[24];
	volatile float probeMisc[8];
	volatile float verifyEMP[7];

	// WorkBuffer probe state
	volatile long wbProbeDone;
	volatile long probeWBPtrLo;
	volatile long probeWBPtrHi;
	volatile long probeHavokPtrLo;
	volatile long probeHavokPtrHi;
	volatile long probeWBMatch;
	volatile long probeNMGPtrLo;
	volatile long probeNMGPtrHi;
	volatile long probeWBMsize;
	volatile long probeWBHeapSize;
	volatile long probeWBFieldScan;

	// hkArray scanner results
	volatile long wbArrayScanDone;
	int wbArrayOffsets[WB_MAX_ARRAYS];
	volatile long wbArrayCount;
	int wbWritableOffsets[WB_MAX_ARRAYS];
	volatile long wbWritableCount;
	WBArrayProbe wbArrayProbes[WB_MAX_ARRAYS];
	int wbSdkArrayOffsets[WB_MAX_SDK_ARRAYS];
	int wbSdkArrayCount;

	// L2 disk cache counters
	volatile long nmDiskHitCount;
	volatile long nmDiskMissCount;
	volatile long nmDiskWriteCount;
	volatile long nmDiskReadUsTimes1;
	volatile long nmDiskWriteUsTimes1;

	// L2 miss log
	L2MissEntry l2MissLog[L2_MISS_LOG_MAX];
	volatile long l2MissLogCount;
	volatile long l2MissLogReported;

	// Worker counters (written by workers via Interlocked, read by stats reporter)
	volatile long workerBusyCount;
	// A busy-bridge transition that ended, under the queue lock, with the byte 0
	// while a claim was in flight. Reported as busyViol= (DEV); expected 0.
	volatile long nmBusyBridgeViolCount;
	volatile long g_slabAllocHits;
	volatile long g_slabAllocWorkerHits;

	// WorkBuffer clone size (set by the work buffer probe, currently unread)
	volatile long g_workBufAllocSize;

	// Lazy hook install marker (0=pending, 1=installing, 2=done) for the hooks
	// InstallNavMeshLazyHooks puts in on the first dispatch: the edgeProcess
	// clone-guard, the populate pass-through and the processJobAlt tripwire.
	volatile long lazyHooksInstalled;

	// Per-MISS NMG clone counters
	volatile long nmCloneConstructCount; // successful clone allocations
	volatile long nmCloneConstructFailCount; // HavokTlsAlloc returned NULL

	// Worker vs bg thread MISS split. Sum should equal nmCacheMissCount.
	volatile long nmWorkerMissCount; // MISSes processed by workers
	volatile long nmBgMissCount; // MISSes processed by bg thread

	// Duplicate jobs: L1 re-check under processJobCS found the key already generated by
	// another thread (counted in nmCacheHitCount as well).
	volatile long nmLateHitCount;

	// edgeProcess clone-guard diagnostics. "armed" = guard skipped cleanup on a
	// clone-context entry (safe). "unarmed" = guard forwarded to original (must be realWB
	// or game-owned — otherwise crash risk). Divergence from expected = guard misbehavior.
	volatile long g_edgeProcessArmedCount;
	volatile long g_edgeProcessUnarmedCount;

	// Material overrides on fresh work buffers. wbOv= reports the count
	// left in the +520 array after processJobAlt's finalizeDeep popped the entry
	// the job appended; 4 is correct. wbOvSlope= counts entries where the slope in
	// NM_MATERIAL_SLOPE_BITS disagreed with the one the game installed.
	volatile long g_wbOverrideInstalled;
	volatile long g_wbOverrideSlopeBad;
	// Teardown found more entries than ConstructFreshSettings can have appended, so
	// it left them alone. Reported as wbOvSkip= only when non-zero; expected 0.
	volatile long g_wbOverrideSkipped;

	// wb+328 (m_minCharacterWidth) on the last fresh work buffer, held as the raw
	// float bits. Reported as wbQ=; 0.90 is the game's own value, copied from the
	// real work buffer.
	volatile long g_wbQualityLast;

	// Fresh-WB pruning proof token (nm_quality.h). The region-pruning scalars of the
	// session's first fresh work buffer, as read from it after
	// ConstructFreshSettings' copies, latched once (g_wbPruneSeen 0 -> 1 claimed ->
	// 2 once the four values are stored). Reported on every stats line as
	// prune=<on|off>(area=..,seed=..,border=..,pvb=..,pbt=..): on = navmeshVanillaPruning; the values read 1e8/0.40/0.00 with it on and
	// Havok's 5/1.00/0.10 with it off. Written on the NavMesh bg thread or a worker
	// (Interlocked only), read by the main-thread stats reporter.
	volatile long g_wbPruneSeen;
	volatile long g_wbPruneAreaBits;
	volatile long g_wbPruneSeedBits;
	volatile long g_wbPruneBorderBits;
	volatile long g_wbPruneFlags; // pvb | pbt << 8
	// Fresh work buffers built with a copied block (the real WB's actual bytes)
	// that differs from the nm_quality.h table the L2 settings hash describes
	// (pruneBad= / xvBad=, printed only when non-zero; expected absent). Any such
	// difference also turns L2 off for the session (g_l2Bypass below).
	volatile long g_wbPruneBad;
	volatile long g_wbExtraVertexBad;

	// L2 bypass. Set once, never cleared, when the real work
	// buffer's region-pruning or extra-vertex values differ from the tables the
	// settings hash describes: meshes generated from then on would not match their
	// cache key, so ReadDiskCache and BuildDiskCacheBlob refuse every read and
	// write for the rest of the session. L1 stays on (per session, generated with
	// the same values). Checked at the first dispatch (CheckGenerationSettingsKey,
	// nm_quality.h) and again on every fresh work buffer. Reported as
	// l2Bypass=on(r<reads>/w<writes>) when set, and by one PROD line
	// "NavMesh L2 bypassed: generation settings differ from the cache key (...)".
	volatile long g_l2Bypass;
	volatile long nmL2BypassReads;
	volatile long nmL2BypassWrites;


	// The hook's install state: 0 not tried yet (before the first dispatch),
	// 1 installed, 2 refused (prologue mismatch or AddHook failure). A refusal turns
	// the whole feature off for the session. Written once by
	// InstallNavMeshLazyHooks on the NavMesh bg thread, read anywhere.
	volatile long g_nbrSeedHookState;

	// Every call of the hooked getSeedPointsFromAdjacentZone, one per direction of
	// a type-0 generation, classified after the original ran. Disjoint:
	//   live  the original appended seeds (a live or completed neighbour mesh)
	//   zero  no seeds, but a live non-temp neighbour sector exists (it has no
	//         border edge on that side: cliff, water, or a town placeholder)
	//   temp  no seeds; the neighbour sector is temp (a stale tile awaiting
	//         regeneration, which the game refuses as a seed source)
	//   none  no seeds; no neighbour sector at all (unloaded)
	// Reported as nbrSeed=live<n>/temp<n>/none<n>/zero<n>. Interlocked only.
	volatile long nmNbrSeedLive;
	volatile long nmNbrSeedTemp;
	volatile long nmNbrSeedNone;
	volatile long nmNbrSeedZero;

	// Set once when a stand-in callee fails its byte check (SortedArray__grow, the
	// game's string release) or the hkContainerHeapAllocator vtable differs: the
	// stand-in is off for the session (the hook's instrumentation stays).
	volatile long g_nbrSeedStandInRefused;

	// Stand-in outcomes, one per call where the game added nothing and the neighbour
	// was temp or missing (only on type-0 generations the mod runs):
	//   ship    the shipped tile's seeds for that side were appended
	//   place   the shipped tile gives nothing there: a placeholder (< 1000 faces,
	//           or no border-strip edge on any side) or no border-strip edge on
	//           this side
	//   nofile  no shipped tile (outside the 64x64 grid, or no file)
	//   late    the tile's record was not built yet (another thread still loading
	//           it past the bounded wait, the prefetch skipped for the stop, or the
	//           neighbour turned missing or temp after the prefetch saw it live)
	// Reported as standIn=ship<n>/place<n>/nofile<n>/late<n>, siSeeds=<avg seeds
	// per ship>, siMs=<avg>/<max> over the shipped-tile loads (lock wait, load,
	// extraction, delete), siRec=<records built>; siHBad=<n> (a record built with
	// another strip width than the target's; expected absent) only when non-zero.
	volatile long nmNbrStandInShip;
	volatile long nmNbrStandInPlace;
	volatile long nmNbrStandInNoFile;
	volatile long nmNbrStandInLate;
	volatile long nmNbrStandInHBad;
	volatile LONGLONG nmNbrStandInSeeds;
	volatile long nmNbrLoadCount;
	volatile LONGLONG nmNbrLoadTotalUs;
	volatile long nmNbrLoadMaxUs;
	volatile long nmNbrRecordCount;
	// Generations with a late stand-in whose mesh was kept out of
	// L2 (the L1 entry stays). Printed as l2LateSkip=<n> right after standIn=.
	volatile long nmNbrL2LateSkip;


	// Type-1 (partial) jobs whose partialGeneration call was made against the real
	// generator rather than a worker's clone. Reported as nbrLookup=.
	volatile long nmPartialRealCount;

	// processJobAlt called by a thread that did not own processJobCS. Reported as
	// trip=; expected 0 over a whole session.
	volatile long nmTripCount;

	// Time spent inside orig_dispatchJob under processJobCS for type 2/3/4 jobs,
	// in tenths of a millisecond. Reported as t234=<avg>/<max>ms.
	volatile long nmT234Count;
	volatile long nmT234TotalMsTimes10;
	volatile long nmT234MaxMsTimes10;

	// Entries into a collision-build region (either hooked builder, or the
	// partialGeneration call) that found another region already in flight, i.e.
	// build regions in flight > 1. Reported as bcOverlap=.
	//
	// NOT "races prevented": two threads can both be in a build region and still
	// be serialized by the game's own build mutex inside buildSectionCollision.
	// What this counts is concurrency at the region level, which is the necessary
	// condition for the unguarded tail races, not proof one occurred.
	volatile long g_buildOverlapSeen;

	// buildCollisionCS wait (blocked in Enter) and hold (Enter to Leave) across all
	// three regions. Totals are 64-bit microseconds; the maxima are microseconds.
	// Reported as bcWait=<avg>/<max>ms bcHold=<avg>/<max>ms.
	volatile long nmBcCount;
	volatile LONGLONG nmBcWaitTotalUs;
	volatile long nmBcWaitMaxUs;
	volatile LONGLONG nmBcHoldTotalUs;
	volatile long nmBcHoldMaxUs;

	// Semaphore handles closed when a worker's NMG clone is freed
	// (15 per clone), and handles skipped because they were still identical to the
	// real generator's, i.e. never re-created by fn_queueLockInit. hSkip= is
	// printed only when non-zero and should never appear.
	volatile long nmCloneHandleClosed;
	volatile long nmCloneHandleSkipped;

	// Bytes released per fresh-work-buffer teardown (the +520 guard,
	// the +288 material-map copy and the 544-byte block). DEV only, reported as
	// wbFreed=. It does NOT include what the dtor body itself frees, which is the
	// bulk of it and is not measurable from here.
	volatile LONGLONG nmWbFreedBytes;

	// A worker HIT that could not be served from its slot — the slot no longer held
	// the job's key by the time it reconstructed, the index was out of range, or the
	// reconstruct itself failed — so the job was regenerated instead of being
	// deleted with no mesh. Reported as hitStale=; expected small.
	volatile long nmHitStaleCount;

	// A rebuild from a saved slot index found the index unusable: outside the
	// ring, or naming a slot that no longer holds the key with a servable mesh
	// (the ring published over it, or the cache was cleared). The job regenerates.
	// Reported as l1Replaced=; expected small.
	volatile long nmL1ReplacedCount;

	// L2 file reads skipped because another worker was already reading that exact
	// key, which only happens for duplicate jobs on one zone. Reported as dupL2=.
	volatile long nmDupL2Avoided;

	// The in-flight table had no free slot, so the read went ahead unregistered.
	// Distinct from dupL2: nobody was reading that key.
	volatile long nmL2FlightFull;


	volatile long nmPjWaitCount[PJWAIT_SITE_COUNT];
	volatile LONGLONG nmPjWaitTotalUs[PJWAIT_SITE_COUNT];
	volatile long nmPjWaitMaxUs[PJWAIT_SITE_COUNT];

	// MISS entries (CloneNMG or missLock) that stood aside because a bounded
	// processJobCS poll (NavMeshTryLockProcessJobFor with a timeout, the save-load
	// reset) wanted the lock. Their delay is inside the pjWait figures above.
	// Reported as pjYield= only when non-zero.
	volatile long nmPjYieldCount;

	// Mod-unload protocol (nm_workers.h). Cumulative, printed on the stats
	// line (DEV and PROD) so a session can see unloads being deferred:
	//   ulSkipJob=   NavMeshBeginZoneUnload refused: a job for the zone was queued
	//   ulSkipClaim= NavMeshBeginZoneUnload refused: a claim for the zone in flight
	//   ulSkipPj=    processJobCS try failed while an unload was published
	//   ulHeld=      claim attempts that left the unloading zone's job queued
	//                (printed only when non-zero)
	//   ulPrio=      unload priority requests raised after a pj deferral
	//                (NavMeshRequestPjPriority)
	//   ulPrioWin=   zero-wait tries that won while a request was still up
	volatile long nmUlSkipJob;
	volatile long nmUlSkipClaim;
	volatile long nmUlSkipPj;
	volatile long nmUlHeld;
	volatile long nmUlPrio;
	volatile long nmUlPrioWin;

	volatile long nmClaimAgeMissCount;
	volatile LONGLONG nmClaimAgeMissTotalUs;
	volatile long nmClaimAgeMissMaxUs;
	volatile long nmClaimAgeMissBucket[CLAIMAGE_BUCKET_COUNT];
	volatile long nmClaimAgeHitCount;
	volatile LONGLONG nmClaimAgeHitTotalUs;
	volatile long nmClaimAgeHitMaxUs;

	volatile long nmStaleCount[STALE_SITE_COUNT];
	// The last drop, as five plain volatile LONGs each written on its own (no lock
	// ties them together): a reader racing two drops can see fields from both (a
	// torn read), which is acceptable for a diagnostic that only reports the most
	// recent event.
	volatile long nmStaleLastAgeUs;

	// Workers that completed Havok thread init and have not exited. Reported as
	// workers=<live>.
	volatile long g_navMeshWorkersLive;

	// The worker pool's refusal for the session, an NmPoolDecision; 0 while the
	// pool is allowed or not yet decided. Set once on the NavMesh bg thread; a
	// refusal prints workers=<its token> in place of the live count.
	volatile long g_navMeshPoolRefusal;

	// L2 rejection counters, one per L2RejectReason, and the size-cap eviction count
	volatile long l2RejCount[L2REJ_REASON_COUNT];
	volatile long l2CapEvicted;

	// L2 writes refused because the entry had zero faces
	// (BuildDiskCacheBlob). Reported as l2ZeroSkip= only when
	// non-zero; expected absent, since L1 already refuses zero-face meshes.
	volatile long nmL2ZeroFaceSkip;

	// ReconstructNavMesh gave up on a Havok allocation failure and freed everything
	// it had taken. Reported as reconFail= on the stats line.
	volatile long nmReconFailCount;

	// Meshes that came back with zero faces, whether freshly generated or read out
	// of an old L2 file. A generation that aborts (a Havok keycode, out of memory)
	// is indistinguishable from a legitimately empty tile at this level, and
	// caching one makes that tile permanently empty, so none of
	// them are stored or served. Reported as zeroFace= on the stats line.
	volatile long nmZeroFaceCount;

	// The split the L2 rule is decided on: zero faces out of an empty input is
	// a legitimately empty tile, zero faces out of a real input is an aborted
	// generation. Reported as zfEmptyIn= and zfAbort=.
	volatile long nmZeroFaceEmptyInput;
	volatile long nmZeroFaceAbort;

	// Disk cache directory (initialized by InitNavMeshCacheCS, used by nm_disk_cache).
	// nmDiskCacheDirBuf is the char copy the bg threads use — no CRT string objects
	// off the main thread. Both end with a trailing backslash.
	char nmDiskCacheDirBuf[MAX_PATH];
	bool nmDiskCacheDirChecked;




	// --------------------------------------------------------------------
	// Claim-time content reads
	// --------------------------------------------------------------------
	//
	// The building hash walks the zone content's things list (content+0x60,
	// count +0x58) on the NavMesh bg thread and the workers, at claim time, with no
	// lock the main thread respects. The game's own unload runs on the main thread:
	// ZoneMapContent::deactivate (0x9FDAB0) first destroys every thing through
	// GameWorld::destroy_RootObject and zeroes the count (0x9FDEC3-0x9FDF05), then
	// the content is deleted, and only then is ZoneMap::mapContent (zone+0) NULLed.
	// A walk that overlaps any of that reads freed memory or a list being rewritten.
	//
	// So the walk runs under SEH (GuardEnter/GuardLeave, POD-only function) and
	// checks itself: zone+0, the things count and the things buffer are read before
	// and after it. It is trusted only when nothing faulted, the content is
	// non-NULL and all three are unchanged. An untrusted hash keys nothing: the job
	// is neither looked up in nor stored to L1 or L2 (so a wrong mesh can neither
	// be served nor published); it is still generated, or dropped by the stale-zone
	// re-check.
	//
	// hashRace= on the NM cache stats line counts every refusal: a hash that failed
	// its own check, and a store whose zone no longer held the content the hash was
	// computed from (ZoneContentUnchanged, below). One job can count twice (the
	// worker's claim-time lookup and ProcessNavMeshJob hash independently).
	volatile long nmHashRaceCount;
};

// The L1 entries and write index are guarded by nmCacheCS. The main-thread
// stats reporter samples nmCacheFill without the lock; that diagnostic may
// be stale and is not a snapshot of the entries.
struct NavMeshL1Ring
{
	NavMeshCacheEntry nmCache[NM_CACHE_SIZE];
	int               nmCacheWriteIdx;
	int               nmCacheFill;
};

extern NavMeshCacheState g_nmCache;
extern NavMeshL1Ring     g_nmL1;

} // namespace navmesh

// Critical sections (shared across navmesh cache modules)
extern CRITICAL_SECTION nmCacheCS;
extern CRITICAL_SECTION processJobCS;
extern CRITICAL_SECTION buildCollisionCS;

// A hold of nmCacheCS, the L1 ring's lock. Taken after processJobCS when both
// are held, never under the queue lock (+152). The ring's functions take a
// reference to one as the caller's statement that it holds the lock.
// Release() is for a region whose normal paths let go before the scope ends,
// at the statement that released it; the destructor releases only on a C++
// unwind.
struct NmCacheLock
{
	NmCacheLock() : held(true) { EnterCriticalSection(&nmCacheCS); }
	~NmCacheLock() { Release(); }
	void Release()
	{
		if (!held)
			return;
		held = false;
		LeaveCriticalSection(&nmCacheCS);
	}

private:
	bool held;
	NmCacheLock(const NmCacheLock&);
	NmCacheLock& operator=(const NmCacheLock&);
};

inline bool L2Bypassed() { return InterlockedCompareExchange(&navmesh::g_nmCache.g_l2Bypass, 0, 0) != 0; }

// True when the
// getSeedPointsFromAdjacentZone hook is wanted: not turned off in the INI. Read-only after LoadConfig, any thread.
inline bool NmNbrSeedHookWanted()
{
	return navmesh::g_navmeshCfg.navmeshNeighbourSeedsEnabled;
}

// True when the stand-in seeds are wanted in the INI, and neither the hook nor a callee check refused them. Decides the
// L2 settings hash ("nbrseed1"). Before the first dispatch (hook state 0) it
// answers for the install that is about to happen; a refusal there flips it
// before any L2 file is read or written (every L2 access runs on a NavMesh
// thread after InstallNavMeshLazyHooks). Any thread.
inline bool NmNbrSeedStandInActive()
{
	return navmesh::g_navmeshCfg.navmeshNeighbourSeedsEnabled
	    && InterlockedCompareExchange(&navmesh::g_nmCache.g_nbrSeedHookState, 0, 0) != NBRSEED_HOOK_FAILED
	    && InterlockedCompareExchange(&navmesh::g_nmCache.g_nbrSeedStandInRefused, 0, 0) == 0;
}
// Records one zero-face mesh. inputTri is the generation's input triangle
// count, captured by the populate hook (-1 when it is unknown, e.g. a mesh read
// back from an L2 file); inputThings is the zone's things count, or -1.
// Safe on any thread.
void NoteZeroFaceMesh(const NavMeshCacheKey& key, int inputTri, int inputVert, int inputThings);

// Cache operations
void           InitNavMeshCacheCS();
void           ClearNavMeshCache();
int            FindCacheEntry(const NmCacheLock& held, const NavMeshCacheKey& key);
void           EvictCacheEntry(int idx);
// Deep-copies the generated mesh into the ring buffer. Returns the slot index,
// or -1 when the mesh is out of bounds or an allocation failed (nothing is
// stored in that case — never a valid entry with a NULL array).
// Caller must hold nmCacheCS.
int            StoreCacheEntry(const NmCacheLock& held, const NavMeshCacheKey& key, uintptr_t navMeshPtr);

// Moves a freshly read L2 entry into the ring buffer and returns its slot
// index, or -1 (the entry's arrays are freed) when it is inconsistent.
// Takes ownership of `e` on success. Caller must hold nmCacheCS.
int            PromoteDiskEntryToL1(const NmCacheLock& held, NavMeshCacheEntry& e);
void*          ReconstructNavMesh(const NavMeshCacheEntry& entry);
// Rebuilds the mesh of slot idx, an index taken under an earlier nmCacheCS
// hold, only if the slot still holds `expected` (CacheSlotMatches). Caller
// holds nmCacheCS. NULL when idx no longer names a servable slot for
// `expected` (out of range, another key or no faces; *replacedOut true,
// counted l1Replaced=) or the rebuild failed (*replacedOut false).
// replacedOut may be NULL.
void*          ReconstructExpected(const NmCacheLock& held, const NavMeshCacheKey& expected, int idx, bool* replacedOut);
unsigned int   HashAABB(const float* aabb6);


// Computes the order-independent building hash (see nm_building_hash.cpp) for the
// zone's current content. Returns true when the hash is trustworthy; *hashOut is
// the hash and *contentOut the content pointer it was computed from. Returns
// false (counted as hashRace=) when the walk faulted, the content was NULL, or
// the content / things count / things buffer changed during the walk; *hashOut
// and *contentOut are then 0 and the key must not be used for L1 or L2.
// Any thread; no allocation, no logging.
bool           ComputeBuildingHashChecked(uintptr_t jobZone, unsigned int* hashOut,
                                          uintptr_t* contentOut);

// True when the zone's mapContent is still `content` (and non-NULL). A plain
// read of zone+0: ZoneMap entries live in the ZoneManager's fixed array and are
// never freed. Used right before an L1 store, so a mesh keyed on content that
// has since been unloaded (or unloaded and reloaded) is never published.
bool           ZoneContentUnchanged(uintptr_t jobZone, uintptr_t content);

// The zone content's things count for diagnostics (the L2-miss log, the
// zero-face record), read under SEH. -1 when the content is NULL or the read
// faulted. Any thread.
int            SafeZoneThingsCount(uintptr_t jobZone);

// Stats reporter (called from main thread)
void LogNavMeshCacheStats(double now);


#endif // KENSHI_ZONE_OPT_NM_CACHE_CORE_H
