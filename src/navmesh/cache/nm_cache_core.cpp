// nm_cache_core.cpp - L1 navmesh ring, cache operations and lock ownership.

#include "navmesh/cache/nm_cache_core_internal.h"
#include "fixes/world/destroy_list_defer.h"
#include "navmesh/cache/nm_cache_core.h"
#include "diag/mem_probe.h"
#include "navmesh/generation/nm_misspar.h"      // MissParAppendStats (missN=/missT=/genOv=/pjHeld= tokens)
#include "navmesh/generation/nm_quality.h"      // NmVanillaPruningActive (prune= token, settings lines)
#include "navmesh/jobs/nm_buildlock.h"    // BuildLockStatsSuffix (bcMode= ... stitch= tokens)
#include "navmesh/cache/nm_disk_cache.h"   // L2 format constants for the startup self-check
#include "navmesh/cache/nm_l2_writer.h"    // l2WrFail= token, the not-banking test
#include "navmesh/workers/nm_worker_gate_policy.h"
#include "navmesh/cache/nm_key_hash.h"


NavMeshCacheEntry nmCache[NM_CACHE_SIZE];
int nmCacheWriteIdx = 0;
int nmCacheFill = 0;
int nmDiagStage = 2;  // 0=bypass, 1=dequeue only, 2=full cache

static bool nmCacheCSInitialized = false;
CRITICAL_SECTION nmCacheCS;
CRITICAL_SECTION buildCollisionCS;
CRITICAL_SECTION processJobCS;
volatile long    nmCacheDisabled = 0;

volatile long nmJobCount = 0;
volatile long nmCacheHitCount = 0;
volatile long nmCacheMissCount = 0;
volatile long nmCacheSkipCount = 0;
volatile long nmTotalMsTimes10 = 0;
volatile long nmSavedMsTimes10 = 0;
volatile long nmDiagLastGridX = -99;
volatile long nmDiagLastGridY = -99;
volatile long nmDiagLastType = -1;
volatile long nmDiagStep = 0;
volatile long nmDiagHitGrid = -99;
namespace nm_cache_core_detail {
double lastNMLogTime = 0.0;
} // namespace nm_cache_core_detail

volatile long  nmSettingsDumped = 0;
volatile long  nmSettingsVerified = 0;
volatile float probeEMP[14];
volatile float probeGen[24];
volatile float probeMisc[8];
volatile float verifyEMP[7];

volatile long wbProbeDone = 0;
volatile long probeWBPtrLo = 0;
volatile long probeWBPtrHi = 0;
volatile long probeHavokPtrLo = 0;
volatile long probeHavokPtrHi = 0;
volatile long probeWBMatch = 0;
volatile long probeNMGPtrLo = 0;
volatile long probeNMGPtrHi = 0;
volatile long probeWBMsize = 0;
volatile long probeWBHeapSize = 0;
volatile long probeWBFieldScan = 0;

volatile long wbArrayScanDone = 0;
int           wbArrayOffsets[WB_MAX_ARRAYS];
volatile long wbArrayCount = 0;
int           wbWritableOffsets[WB_MAX_ARRAYS];
volatile long wbWritableCount = 0;
WBArrayProbe  wbArrayProbes[WB_MAX_ARRAYS];
int           wbSdkArrayOffsets[WB_MAX_SDK_ARRAYS];
int           wbSdkArrayCount = 0;

volatile long nmDiskHitCount = 0;
volatile long nmDiskMissCount = 0;
volatile long nmDiskWriteCount = 0;
volatile long nmDiskReadUsTimes1 = 0;
volatile long nmDiskWriteUsTimes1 = 0;

L2MissEntry   l2MissLog[L2_MISS_LOG_MAX];
volatile long l2MissLogCount = 0;
volatile long l2MissLogReported = 0;

volatile long workerBusyCount = 0;
volatile long nmBusyBridgeViolCount = 0;
volatile long g_slabAllocHits = 0;
volatile long g_slabAllocWorkerHits = 0;

volatile long g_workBufAllocSize = 0;

volatile long lazyHooksInstalled = 0;
volatile long nmCloneConstructCount = 0;
volatile long nmCloneConstructFailCount = 0;
volatile long nmWorkerMissCount = 0;
volatile long nmBgMissCount = 0;
volatile long nmLateHitCount = 0;

volatile long l2RejCount[L2REJ_REASON_COUNT] = {};
volatile long l2CapEvicted = 0;
volatile long nmL2ZeroFaceSkip = 0;
volatile long nmReconFailCount = 0;

volatile long g_wbPruneSeen       = 0;
volatile long g_wbPruneAreaBits   = 0;
volatile long g_wbPruneSeedBits   = 0;
volatile long g_wbPruneBorderBits = 0;
volatile long g_wbPruneFlags      = 0;
volatile long g_wbPruneBad        = 0;
volatile long g_wbExtraVertexBad  = 0;
volatile long g_l2Bypass          = 0;
volatile long nmL2BypassReads     = 0;
volatile long nmL2BypassWrites    = 0;

volatile long g_nbrSeedHookState = 0;
volatile long nmNbrSeedLive      = 0;
volatile long nmNbrSeedTemp      = 0;
volatile long nmNbrSeedNone      = 0;
volatile long nmNbrSeedZero      = 0;

volatile long     g_nbrSeedStandInRefused = 0;
volatile long     nmNbrStandInShip   = 0;
volatile long     nmNbrStandInPlace  = 0;
volatile long     nmNbrStandInNoFile = 0;
volatile long     nmNbrStandInLate   = 0;
volatile long     nmNbrStandInHBad   = 0;
volatile LONGLONG nmNbrStandInSeeds  = 0;
volatile long     nmNbrLoadCount     = 0;
volatile LONGLONG nmNbrLoadTotalUs   = 0;
volatile long     nmNbrLoadMaxUs     = 0;
volatile long     nmNbrRecordCount   = 0;
volatile long     nmNbrL2LateSkip    = 0;

volatile long nmPartialRealCount = 0;

volatile long nmTripCount = 0;
volatile long nmTripInstalled = 0;
volatile long nmT234Count = 0;
volatile long nmT234TotalMsTimes10 = 0;
volatile long nmT234MaxMsTimes10 = 0;

volatile long     g_buildOverlapSeen = 0;
volatile long     nmBcCount = 0;
volatile LONGLONG nmBcWaitTotalUs = 0;
volatile long     nmBcWaitMaxUs = 0;
volatile LONGLONG nmBcHoldTotalUs = 0;
volatile long     nmBcHoldMaxUs = 0;

volatile long nmCloneHandleClosed = 0;
volatile long nmCloneHandleSkipped = 0;
volatile LONGLONG nmWbFreedBytes = 0;

volatile long nmHitStaleCount = 0;
volatile long nmL1ReplacedCount = 0;
volatile long nmDupL2Avoided = 0;
volatile long nmL2FlightFull = 0;

volatile long g_navMeshWorkersLive = 0;
volatile long g_navMeshPoolRefusal = 0;

volatile long     nmPjWaitCount[PJWAIT_SITE_COUNT]   = {};
volatile LONGLONG nmPjWaitTotalUs[PJWAIT_SITE_COUNT] = {};
volatile long     nmPjWaitMaxUs[PJWAIT_SITE_COUNT]   = {};
volatile long     nmPjYieldCount = 0;
volatile long     nmUlSkipJob   = 0;
volatile long     nmUlSkipClaim = 0;
volatile long     nmUlSkipPj    = 0;
volatile long     nmUlHeld      = 0;
volatile long     nmUlPrio      = 0;
volatile long     nmUlPrioWin   = 0;

volatile long     nmClaimAgeMissCount   = 0;
volatile LONGLONG nmClaimAgeMissTotalUs = 0;
volatile long     nmClaimAgeMissMaxUs   = 0;
volatile long     nmClaimAgeMissBucket[CLAIMAGE_BUCKET_COUNT] = {};
volatile long     nmClaimAgeHitCount    = 0;
volatile LONGLONG nmClaimAgeHitTotalUs  = 0;
volatile long     nmClaimAgeHitMaxUs    = 0;

volatile long nmStaleCount[STALE_SITE_COUNT] = {};
volatile long nmStaleLastGridX  = -1;
volatile long nmStaleLastGridY  = -1;
volatile long nmStaleLastType   = -1;
volatile long nmStaleLastReason = STALE_REASON_NONE;
volatile long nmStaleLastAgeUs  = 0;

volatile long nmZeroFaceCount = 0;
volatile long nmZeroFaceLastTri = -1;
volatile long nmZeroFaceLastVert = -1;
volatile long nmZeroFaceLastThings = -1;
volatile long nmZeroFaceLastGridX = -99;
volatile long nmZeroFaceLastGridY = -99;
volatile long nmZeroFaceLastType = -1;
volatile long nmZeroFaceEmptyInput = 0;
volatile long nmZeroFaceAbort = 0;

std::string   nmDiskCacheDir;
char          nmDiskCacheDirBuf[MAX_PATH] = {};
bool          nmDiskCacheDirChecked = false;

// The L2 directory under the DLL folder. Every L2 path (lookups, writes, the
// startup orphan sweep and the size cap) is built from nmDiskCacheDirBuf, which
// comes from this name alone. The settings hash is in each file's header, not
// its name, so files written under other generation settings sit here too and
// are refused on read (l2Rej h) until they are regenerated or the cap evicts
// them.
static const char NM_L2_DIR_NAME[] = "navmesh_cache\\";

void InitNavMeshCacheCS()
{
	if (!nmCacheCSInitialized)
	{
		InitializeCriticalSection(&nmCacheCS);
		InitializeCriticalSection(&buildCollisionCS);
		InitializeCriticalSection(&processJobCS);
		nmCacheCSInitialized = true;
		LogDebug("NavMesh cache CS initialized");
	}

	// Hoist disk cache dir init to main thread to eliminate lazy-init race
	if (!nmDiskCacheDirChecked)
	{
		nmDiskCacheDir = GetDLLDirectory() + NM_L2_DIR_NAME;
		// A directory that cannot be created loses every write of the session,
		// so say so here rather than leaving it to the first failed write. It
		// is not fatal: the session runs on L1 alone, and a directory that
		// appears later is picked up by the write path's own retry.
		bool dirOk = (CreateDirectoryA(nmDiskCacheDir.c_str(), NULL) != 0);
		unsigned long dirErr = dirOk ? 0 : GetLastError();
		if (!dirOk && dirErr == ERROR_ALREADY_EXISTS)
			dirOk = true;
		// char copy for the bg threads: they must not touch std::string
		if (nmDiskCacheDir.size() < sizeof(nmDiskCacheDirBuf))
			strcpy_s(nmDiskCacheDirBuf, sizeof(nmDiskCacheDirBuf), nmDiskCacheDir.c_str());
		nmDiskCacheDirChecked = true;
		LogDebug("Disk cache dir hoisted: " + nmDiskCacheDir);
		if (!dirOk)
		{
			std::ostringstream ds;
			ds << "L2 disk cache: cannot create \"" << nmDiskCacheDir
			   << "\" (err=" << dirErr << ") - nothing will be banked this session";
			LogMsg(ds.str());
		}
		if (!nmDiskCacheDirBuf[0])
			LogMsg("L2 disk cache: the directory path does not fit MAX_PATH - nothing will be banked this session");
	}

	// One-time self-check of the L2 primitives (DEV only). The CRC vector is
	// the standard "123456789" check value for CRC-32/ISO-HDLC.
	{
		std::ostringstream ss;
		// settingsHash: d3fe8bd2 with pruning off (navmeshVanillaPruning=false),
		// 5f777f1d with it on (L2SettingsHash). Never 3398be7f (a deleted quality-tuning arm's value).
		// The neighbour-seed stand-in folds "nbrseed1" on top: 8f42260b with
		// pruning on (f6a3409c with pruning off); navmeshNeighbourSeeds=false leaves the values above. This runs
		// before the neighbour-seed hook installs, so it assumes the install
		// succeeds; "neighbour seeds: L2 settingsHash=... in effect" (DEV and
		// PROD, at the first dispatch) is the value the session then uses.
		ss << "L2 format: ver=" << L2_CACHE_VERSION
		   << " settingsHash=" << std::hex << L2SettingsHash()
		   << " (prune=" << (NmVanillaPruningActive() ? "on" : "off")
		   << " step=" << std::dec << 2
		   << ", nbrSeed=" << (NmNbrSeedStandInActive() ? "on" : "off")
		   << " step=" << 2 << ")" << std::hex
		   << " modSet=" << g_modSetHash << std::dec
		   << " capMB=" << cfg_navmeshDiskCacheMaxMB
		   << " crcSelfTest=" << ((L2Crc32("123456789", 9) == 0xCBF43926u) ? "ok" : "FAIL");
		LogDebug(ss.str());
	}

	// Sweep stale ".tmp" leftovers and unreachable old-format entries, and
	// measure the directory, before any navmesh job runs. Without this a
	// resting cache is never trimmed, because the cap otherwise only runs on
	// a write, and orphans wait for the next MISS.
	L2StartupScan();
	{
		std::ostringstream ss;
		ss << "L2 startup scan: removed "
		   << InterlockedCompareExchange(&l2CapEvicted, 0, 0) << " file(s)";
		LogMsg(ss.str());
	}
}


// inputTri comes from the populate hook, which reads the input geometry's
// triangle count (geometry+40) on its way into realGenerate. realGenerate
// itself tests that field and refuses to generate when it is zero, asserting
// "Passed in empty triMesh to generateNavMesh", so the split below is the
// generator's own notion of an empty input, not an inference:
//   tri == 0  -> nothing was fed in, the empty result is correct for the tile
//   tri  > 0  -> geometry went in and no faces came out, so the run aborted
// inputTri is -1 when the count is unavailable (the hook failed to install, or
// the mesh came from an L2 file rather than a generation); neither counter
// moves then. inputThings is the zone's things count, kept as a cross-check.
void NoteZeroFaceMesh(const NavMeshCacheKey& key, int inputTri, int inputVert, int inputThings)
{
	InterlockedIncrement(&nmZeroFaceCount);
	InterlockedExchange(&nmZeroFaceLastTri, (long)inputTri);
	InterlockedExchange(&nmZeroFaceLastVert, (long)inputVert);
	InterlockedExchange(&nmZeroFaceLastThings, (long)inputThings);
	InterlockedExchange(&nmZeroFaceLastGridX, (long)key.gridX);
	InterlockedExchange(&nmZeroFaceLastGridY, (long)key.gridY);
	InterlockedExchange(&nmZeroFaceLastType, (long)key.jobType);

	if (inputTri == 0)
		InterlockedIncrement(&nmZeroFaceEmptyInput);
	else if (inputTri > 0)
		InterlockedIncrement(&nmZeroFaceAbort);
}


int FindCacheEntry(const NavMeshCacheKey& key)
{
	for (int i = 0; i < nmCacheFill; ++i)
	{
		if (!nmCache[i].valid || !KeysMatch(nmCache[i].key, key))
			continue;
		// Never serve a zero-face mesh, including one a build before this rule
		// left in the ring buffer. Regenerating is right whether the tile is
		// genuinely empty or the generation aborted.
		if (nmCache[i].faceCount <= 0)
			continue;
		return i;
	}
	return -1;
}

void EvictCacheEntry(int idx)
{
	if (idx < 0 || idx >= NM_CACHE_SIZE) return;
	if (!nmCache[idx].valid) return;

	if (nmCache[idx].cachedFaces)     fn_gameDelArr(nmCache[idx].cachedFaces);
	if (nmCache[idx].cachedEdges)     fn_gameDelArr(nmCache[idx].cachedEdges);
	if (nmCache[idx].cachedVertices)  fn_gameDelArr(nmCache[idx].cachedVertices);
	if (nmCache[idx].cachedFaceData)  fn_gameDelArr(nmCache[idx].cachedFaceData);
	if (nmCache[idx].cachedEdgeData)  fn_gameDelArr(nmCache[idx].cachedEdgeData);

	nmCache[idx].cachedFaces = NULL;
	nmCache[idx].cachedEdges = NULL;
	nmCache[idx].cachedVertices = NULL;
	nmCache[idx].cachedFaceData = NULL;
	nmCache[idx].cachedEdgeData = NULL;
	nmCache[idx].valid = false;
}

// Deep-copies one array out of the generated mesh. Returns false on allocation
// failure so the caller can discard the whole entry.
static bool CopyMeshArray(uintptr_t navMeshPtr, int arrayOff, int count, int unit, void** dest)
{
	*dest = NULL;
	if (count <= 0) return true;
	const void* src = hkArrayGetPtr(navMeshPtr, arrayOff);
	if (!src) return false;
	size_t sz = (size_t)count * (size_t)unit;
	void* buf = fn_gameNewArr(sz);
	if (!buf) return false;
	memcpy(buf, src, sz);
	*dest = buf;
	return true;
}

static void FreeEntryArrays(NavMeshCacheEntry& e)
{
	if (e.cachedFaces)    { fn_gameDelArr(e.cachedFaces);    e.cachedFaces = NULL; }
	if (e.cachedEdges)    { fn_gameDelArr(e.cachedEdges);    e.cachedEdges = NULL; }
	if (e.cachedVertices) { fn_gameDelArr(e.cachedVertices); e.cachedVertices = NULL; }
	if (e.cachedFaceData) { fn_gameDelArr(e.cachedFaceData); e.cachedFaceData = NULL; }
	if (e.cachedEdgeData) { fn_gameDelArr(e.cachedEdgeData); e.cachedEdgeData = NULL; }
	e.valid = false;
}

// Publishes a fully built entry into the ring buffer. Returns its slot index.
static int PublishEntry(NavMeshCacheEntry& src)
{
	if (nmCache[nmCacheWriteIdx].valid)
		EvictCacheEntry(nmCacheWriteIdx);

	int idx = nmCacheWriteIdx;
	nmCache[idx] = src;
	nmCache[idx].valid = true;

	nmCacheWriteIdx = (nmCacheWriteIdx + 1) % NM_CACHE_SIZE;
	if (nmCacheFill < NM_CACHE_SIZE)
		nmCacheFill++;

	memset(&src, 0, sizeof(src));   // ownership moved into the ring buffer
	return idx;
}

int StoreCacheEntry(const NavMeshCacheKey& key, uintptr_t navMeshPtr)
{
	if (!navMeshPtr) return -1;

	int faceCount    = hkArrayGetCount(navMeshPtr, NMOFF_FACES);
	int edgeCount    = hkArrayGetCount(navMeshPtr, NMOFF_EDGES);
	int vertexCount  = hkArrayGetCount(navMeshPtr, NMOFF_VERTICES);
	int faceStriding = *(int*)(navMeshPtr + 112);
	int edgeStriding = *(int*)(navMeshPtr + 116);
	int faceDataCnt  = hkArrayGetCount(navMeshPtr, NMOFF_FACEDATA);
	int edgeDataCnt  = hkArrayGetCount(navMeshPtr, NMOFF_EDGEDATA);

	// Zero faces means either a generation that aborted or a genuinely empty
	// tile, and nothing here can tell those apart (NoteZeroFaceMesh explains
	// why). Caching either one makes the tile permanently empty, so neither is
	// stored. Refusing here also keeps it out of L2: the disk blob is only
	// built from a slot this function published.
	if (faceCount == 0)
		return -1;

	if (faceCount    < 0 || faceCount    > L2_MAX_FACES)    return -1;
	if (edgeCount    < 0 || edgeCount    > L2_MAX_EDGES)    return -1;
	if (vertexCount  < 0 || vertexCount  > L2_MAX_VERTICES) return -1;
	if (faceDataCnt  < 0 || faceDataCnt  > L2_MAX_FACEDATA) return -1;
	if (edgeDataCnt  < 0 || edgeDataCnt  > L2_MAX_EDGEDATA) return -1;

	// Build the entry off to the side, then publish it. A failed allocation
	// frees everything taken so far and stores nothing, so a valid slot never
	// holds a NULL array.
	NavMeshCacheEntry e;
	memset(&e, 0, sizeof(e));
	e.key = key;
	e.faceCount     = faceCount;
	e.edgeCount     = edgeCount;
	e.vertexCount   = vertexCount;
	e.faceDataCount = faceDataCnt;
	e.edgeDataCount = edgeDataCnt;

	bool ok = true;
	if (ok) ok = CopyMeshArray(navMeshPtr, NMOFF_FACES,    faceCount,   HKAI_FACE_SIZE,     &e.cachedFaces);
	if (ok) ok = CopyMeshArray(navMeshPtr, NMOFF_EDGES,    edgeCount,   HKAI_EDGE_SIZE,     &e.cachedEdges);
	if (ok) ok = CopyMeshArray(navMeshPtr, NMOFF_VERTICES, vertexCount, HKAI_VERTEX_SIZE,   &e.cachedVertices);
	if (ok) ok = CopyMeshArray(navMeshPtr, NMOFF_FACEDATA, faceDataCnt, HKAI_FACEDATA_UNIT, &e.cachedFaceData);
	if (ok) ok = CopyMeshArray(navMeshPtr, NMOFF_EDGEDATA, edgeDataCnt, HKAI_EDGEDATA_UNIT, &e.cachedEdgeData);

	if (!ok)
	{
		FreeEntryArrays(e);
		return -1;
	}

	e.faceDataStriding = faceStriding;
	e.edgeDataStriding = edgeStriding;
	e.navMeshFlags = *(unsigned char*)(navMeshPtr + 120);
	memcpy(e.aabb, (void*)(navMeshPtr + 128), 32);
	e.erosionRadius = *(float*)(navMeshPtr + 160);
	e.userData = *(unsigned __int64*)(navMeshPtr + 168);

	return PublishEntry(e);
}

int PromoteDiskEntryToL1(NavMeshCacheEntry& e)
{
	// An L2 file written before this rule can still hold a zero-face mesh.
	// Drop it and let the job regenerate.
	if (e.valid && e.faceCount <= 0)
	{
		NoteZeroFaceMesh(e.key, -1, -1, -1);
		FreeEntryArrays(e);
		return -1;
	}
	if (!e.valid
	    || (e.faceCount     > 0 && !e.cachedFaces)
	    || (e.edgeCount     > 0 && !e.cachedEdges)
	    || (e.vertexCount   > 0 && !e.cachedVertices)
	    || (e.faceDataCount > 0 && !e.cachedFaceData)
	    || (e.edgeDataCount > 0 && !e.cachedEdgeData))
	{
		FreeEntryArrays(e);
		return -1;
	}
	return PublishEntry(e);
}

// Copies one cached array into a fresh Havok TLS buffer and installs it in the
// navmesh. Returns false on allocation failure, leaving the array slot empty.
static bool InstallMeshArray(uintptr_t nm, int arrayOff, const void* src, int count, int unit)
{
	if (count <= 0 || !src) return true;
	size_t sz = (size_t)count * (size_t)unit;
	void* buf = HavokTlsAlloc(sz);
	if (!buf) return false;
	memcpy(buf, src, sz);
	hkArraySet(nm, arrayOff, buf, count, count);
	return true;
}

// Releases the array buffers this function installed, then the navmesh block.
// The Havok destructor is deliberately not called: nothing else has seen the
// object, its refcount has never been raised, and the only allocations on it
// are the five arrays installed here.
//
// `mem` is the block HavokTlsAlloc returned, which is what gets freed; `nm` is
// what the constructor returned. The two are the same pointer today (the ctor
// returns `this`), but the free must name the allocation, not the object.
static void UnwindReconstruct(uintptr_t nm, void* mem)
{
	const int offs[5]  = { NMOFF_FACES, NMOFF_EDGES, NMOFF_VERTICES, NMOFF_FACEDATA, NMOFF_EDGEDATA };
	const int units[5] = { HKAI_FACE_SIZE, HKAI_EDGE_SIZE, HKAI_VERTEX_SIZE, HKAI_FACEDATA_UNIT, HKAI_EDGEDATA_UNIT };
	for (int i = 0; i < 5; ++i)
	{
		void* buf = hkArrayGetPtr(nm, offs[i]);
		int count = hkArrayGetCount(nm, offs[i]);
		if (buf && count > 0)
			HavokTlsFree(buf, (size_t)count * (size_t)units[i]);
		hkArraySet(nm, offs[i], NULL, 0, 0);
	}
	HavokTlsFree(mem, 176);
}

// Allocate fresh hkaiNavMesh via Havok TLS, populate arrays from cache.
// Havok destructor will correctly free everything when refcount reaches 0.
//
// All-or-nothing: a failed Havok allocation frees everything taken so far and
// returns NULL. A partly filled mesh must never reach the game — the callers
// treat a non-NULL return as a complete result and hand it straight to
// buildCollision.
void* ReconstructNavMesh(const NavMeshCacheEntry& entry)
{
	void* mem = HavokTlsAlloc(176);
	if (!mem) { InterlockedIncrement(&nmReconFailCount); return NULL; }

	void* navMesh = fn_navMeshCtor(mem);
	if (!navMesh)
	{
		HavokTlsFree(mem, 176);
		InterlockedIncrement(&nmReconFailCount);
		return NULL;
	}
	uintptr_t nm = (uintptr_t)navMesh;

	bool ok = true;
	if (ok) ok = InstallMeshArray(nm, NMOFF_FACES,    entry.cachedFaces,    entry.faceCount,     HKAI_FACE_SIZE);
	if (ok) ok = InstallMeshArray(nm, NMOFF_EDGES,    entry.cachedEdges,    entry.edgeCount,     HKAI_EDGE_SIZE);
	if (ok) ok = InstallMeshArray(nm, NMOFF_VERTICES, entry.cachedVertices, entry.vertexCount,   HKAI_VERTEX_SIZE);
	if (ok) ok = InstallMeshArray(nm, NMOFF_FACEDATA, entry.cachedFaceData, entry.faceDataCount, HKAI_FACEDATA_UNIT);
	if (ok) ok = InstallMeshArray(nm, NMOFF_EDGEDATA, entry.cachedEdgeData, entry.edgeDataCount, HKAI_EDGEDATA_UNIT);

	if (!ok)
	{
		UnwindReconstruct(nm, mem);
		InterlockedIncrement(&nmReconFailCount);
		return NULL;
	}

	*(int*)(nm + 112) = entry.faceDataStriding;
	*(int*)(nm + 116) = entry.edgeDataStriding;
	*(unsigned char*)(nm + 120) = entry.navMeshFlags;
	memcpy((void*)(nm + 128), entry.aabb, 32);
	*(float*)(nm + 160) = entry.erosionRadius;
	*(unsigned __int64*)(nm + 168) = entry.userData;

	return navMesh;
}

void* ReconstructExpected(const NavMeshCacheKey& expected, int idx, bool* replacedOut)
{
	const bool matches = CacheSlotMatchesAt(nmCache, idx, expected);
	if (replacedOut)
		*replacedOut = !matches;
	if (!matches)
	{
		InterlockedIncrement(&nmL1ReplacedCount);
		return NULL;
	}
	return ReconstructNavMesh(nmCache[idx]);
}

void ClearNavMeshCache()
{
	InterlockedExchange(&nmCacheDisabled, 1);

	if (nmCacheCSInitialized)
		EnterCriticalSection(&nmCacheCS);

	int cleared = nmCacheFill;
	for (int i = 0; i < NM_CACHE_SIZE; ++i)
		EvictCacheEntry(i);
	nmCacheWriteIdx = 0;
	nmCacheFill = 0;

	if (nmCacheCSInitialized)
		LeaveCriticalSection(&nmCacheCS);

	InterlockedExchange(&nmCacheDisabled, 0);

	if (cleared > 0)
	{
		std::ostringstream ss;
		ss << "NavMesh cache cleared, evicted " << cleared << " entries";
		LogDebug(ss.str());
	}
}
