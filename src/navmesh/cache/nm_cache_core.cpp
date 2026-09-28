// nm_cache_core.cpp - L1 navmesh ring, cache operations and lock ownership.

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



namespace navmesh {

NavMeshCacheState g_nmCache =
{
	2,                   // nmDiagStage
	-99,                 // nmDiagLastGridX
	-99,                 // nmDiagLastGridY
	-1,                  // nmDiagLastType
	-99,                 // nmDiagHitGrid
	-1,                  // nmStaleLastGridX
	-1,                  // nmStaleLastGridY
	-1,                  // nmStaleLastType
	STALE_REASON_NONE,   // nmStaleLastReason
	-1,                  // nmZeroFaceLastTri
	-1,                  // nmZeroFaceLastVert
	-1,                  // nmZeroFaceLastThings
	-99,                 // nmZeroFaceLastGridX
	-99,                 // nmZeroFaceLastGridY
	-1,                  // nmZeroFaceLastType
	-1                   // g_wbOverrideAfterPop
};
NavMeshL1Ring g_nmL1;

} // namespace navmesh

namespace nm_cache_core_detail {

union NavMeshCacheStatePodCheck { navmesh::NavMeshCacheState s; };
static_assert(__alignof(navmesh::NavMeshCacheState) >= 8, "NavMeshCacheState must be 8-byte aligned");
union NavMeshL1RingPodCheck { navmesh::NavMeshL1Ring s; };

} // namespace nm_cache_core_detail
using namespace nm_cache_core_detail;

static bool nmCacheCSInitialized = false;
CRITICAL_SECTION nmCacheCS;
CRITICAL_SECTION buildCollisionCS;
CRITICAL_SECTION processJobCS;

static std::string   nmDiskCacheDir;

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
	if (!navmesh::g_nmCache.nmDiskCacheDirChecked)
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
		if (nmDiskCacheDir.size() < sizeof(navmesh::g_nmCache.nmDiskCacheDirBuf))
			strcpy_s(navmesh::g_nmCache.nmDiskCacheDirBuf, sizeof(navmesh::g_nmCache.nmDiskCacheDirBuf), nmDiskCacheDir.c_str());
		navmesh::g_nmCache.nmDiskCacheDirChecked = true;
		LogDebug("Disk cache dir hoisted: " + nmDiskCacheDir);
		if (!dirOk)
		{
			std::ostringstream ds;
			ds << "L2 disk cache: cannot create \"" << nmDiskCacheDir
			   << "\" (err=" << dirErr << ") - nothing will be banked this session";
			LogMsg(ds.str());
		}
		if (!navmesh::g_nmCache.nmDiskCacheDirBuf[0])
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
		   << InterlockedCompareExchange(&navmesh::g_nmCache.l2CapEvicted, 0, 0) << " file(s)";
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
	InterlockedIncrement(&navmesh::g_nmCache.nmZeroFaceCount);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastTri, (long)inputTri);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastVert, (long)inputVert);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastThings, (long)inputThings);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastGridX, (long)key.gridX);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastGridY, (long)key.gridY);
	InterlockedExchange(&navmesh::g_nmCache.nmZeroFaceLastType, (long)key.jobType);

	if (inputTri == 0)
		InterlockedIncrement(&navmesh::g_nmCache.nmZeroFaceEmptyInput);
	else if (inputTri > 0)
		InterlockedIncrement(&navmesh::g_nmCache.nmZeroFaceAbort);
}


int FindCacheEntry(const NavMeshCacheKey& key)
{
	for (int i = 0; i < navmesh::g_nmL1.nmCacheFill; ++i)
	{
		if (!navmesh::g_nmL1.nmCache[i].valid || !KeysMatch(navmesh::g_nmL1.nmCache[i].key, key))
			continue;
		// Never serve a zero-face mesh, including one a build before this rule
		// left in the ring buffer. Regenerating is right whether the tile is
		// genuinely empty or the generation aborted.
		if (navmesh::g_nmL1.nmCache[i].faceCount <= 0)
			continue;
		return i;
	}
	return -1;
}

void EvictCacheEntry(int idx)
{
	if (idx < 0 || idx >= NM_CACHE_SIZE) return;
	if (!navmesh::g_nmL1.nmCache[idx].valid) return;

	if (navmesh::g_nmL1.nmCache[idx].cachedFaces)     fn_gameDelArr(navmesh::g_nmL1.nmCache[idx].cachedFaces);
	if (navmesh::g_nmL1.nmCache[idx].cachedEdges)     fn_gameDelArr(navmesh::g_nmL1.nmCache[idx].cachedEdges);
	if (navmesh::g_nmL1.nmCache[idx].cachedVertices)  fn_gameDelArr(navmesh::g_nmL1.nmCache[idx].cachedVertices);
	if (navmesh::g_nmL1.nmCache[idx].cachedFaceData)  fn_gameDelArr(navmesh::g_nmL1.nmCache[idx].cachedFaceData);
	if (navmesh::g_nmL1.nmCache[idx].cachedEdgeData)  fn_gameDelArr(navmesh::g_nmL1.nmCache[idx].cachedEdgeData);

	navmesh::g_nmL1.nmCache[idx].cachedFaces = NULL;
	navmesh::g_nmL1.nmCache[idx].cachedEdges = NULL;
	navmesh::g_nmL1.nmCache[idx].cachedVertices = NULL;
	navmesh::g_nmL1.nmCache[idx].cachedFaceData = NULL;
	navmesh::g_nmL1.nmCache[idx].cachedEdgeData = NULL;
	navmesh::g_nmL1.nmCache[idx].valid = false;
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
	if (navmesh::g_nmL1.nmCache[navmesh::g_nmL1.nmCacheWriteIdx].valid)
		EvictCacheEntry(navmesh::g_nmL1.nmCacheWriteIdx);

	int idx = navmesh::g_nmL1.nmCacheWriteIdx;
	navmesh::g_nmL1.nmCache[idx] = src;
	navmesh::g_nmL1.nmCache[idx].valid = true;

	navmesh::g_nmL1.nmCacheWriteIdx = (navmesh::g_nmL1.nmCacheWriteIdx + 1) % NM_CACHE_SIZE;
	if (navmesh::g_nmL1.nmCacheFill < NM_CACHE_SIZE)
		navmesh::g_nmL1.nmCacheFill++;

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
	if (!mem) { InterlockedIncrement(&navmesh::g_nmCache.nmReconFailCount); return NULL; }

	void* navMesh = fn_navMeshCtor(mem);
	if (!navMesh)
	{
		HavokTlsFree(mem, 176);
		InterlockedIncrement(&navmesh::g_nmCache.nmReconFailCount);
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
		InterlockedIncrement(&navmesh::g_nmCache.nmReconFailCount);
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
	const bool matches = CacheSlotMatchesAt(navmesh::g_nmL1.nmCache, idx, expected);
	if (replacedOut)
		*replacedOut = !matches;
	if (!matches)
	{
		InterlockedIncrement(&navmesh::g_nmCache.nmL1ReplacedCount);
		return NULL;
	}
	return ReconstructNavMesh(navmesh::g_nmL1.nmCache[idx]);
}

void ClearNavMeshCache()
{
	InterlockedExchange(&navmesh::g_nmCache.nmCacheDisabled, 1);

	if (nmCacheCSInitialized)
		EnterCriticalSection(&nmCacheCS);

	int cleared = navmesh::g_nmL1.nmCacheFill;
	for (int i = 0; i < NM_CACHE_SIZE; ++i)
		EvictCacheEntry(i);
	navmesh::g_nmL1.nmCacheWriteIdx = 0;
	navmesh::g_nmL1.nmCacheFill = 0;

	if (nmCacheCSInitialized)
		LeaveCriticalSection(&nmCacheCS);

	InterlockedExchange(&navmesh::g_nmCache.nmCacheDisabled, 0);

	if (cleared > 0)
	{
		std::ostringstream ss;
		ss << "NavMesh cache cleared, evicted " << cleared << " entries";
		LogDebug(ss.str());
	}
}
