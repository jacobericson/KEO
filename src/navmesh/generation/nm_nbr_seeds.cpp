// nm_nbr_seeds.cpp - neighbour seed prefetch on the bg thread and workers.
// prefetch takes the build mutex alone, never processJobCS.
#include "navmesh/nm_workers.h"
#include "zone/geometry/zone_geometry_epoch.h"
#include "zone/reset/zone_reset_gate.h"
#include "navmesh/generation/nm_misspar.h"
#include "navmesh/jobs/nm_buildlock.h"
#include "navmesh/scheduling/nm_adjacency.h"
#include "plugin/hook_manifest.h"
#include "diag/exit_capture.h"
#include "navmesh/jobs/nm_busy_bridge_policy.h"
#include "navmesh/workers/nm_worker_gate_policy.h"
#include "navmesh/workers/nm_retire_policy.h"
#include "navmesh/nm_workers_internal.h"
using namespace nm_workers_detail;
// --------------------------------------------------------------------
// Neighbour seeds
// --------------------------------------------------------------------
//
// Region pruning (NMPRUNE) keeps only the navmesh regions a seed point reaches.
// A tile's seeds include, per side, the midpoints of the neighbour mesh's
// boundary edges on the shared border: NavMeshGenerator::
// getSeedPointsFromAdjacentZone (0x3C96E0), called four times (W E S N) from
// generateTaskBT's type-0 branch. The game takes a neighbour mesh only when one
// is completed (the generator's done queue or the navmesh add list) or in
// NavMesh::sectors and not `temp`; an unloaded or stale neighbour gives
// nothing, and a plateau at the seam is then pruned (and frozen into L2).
//
// The pass-through records, per call, the seeds the original
// appended and the neighbour's availability class, read after the original
// from NavMesh::getSector(coords, create=false) (0x3AB1F0) exactly as the
// original reads it (the sector's `temp` byte after the lookup released
// NavMesh::mutex). Never getCompletedTask: it sets the task's 0x100 pin bit,
// which the original never clears for its own lookup either. A completed task
// that is not yet a sector and has no border edge on this side therefore reads
// as none/temp here, where the game saw a live neighbour with nothing to give.
//
// Thread: the NavMesh bg thread or a worker, inside processJobAlt, under
// processJobCS. Only Interlocked counters, thread-local state and the game's
// own getSector (which the original itself calls there): no CRT strings, no
// LogMsg. NavMesh::mutex is taken under processJobCS exactly as the original
// takes it, so the lock order is unchanged.

enum { NBR_CLASS_UNSEEN = 0, NBR_CLASS_NONE, NBR_CLASS_TEMP, NBR_CLASS_LIVE, NBR_CLASS_ZERO };

// The direction index of a unit step, -1 for anything else.
static inline int NbrDirIndex(int dx, int dy)
{
	if (dy == 0 && dx == -1) return NBR_DIR_W;
	if (dy == 0 && dx ==  1) return NBR_DIR_E;
	if (dx == 0 && dy == -1) return NBR_DIR_S;
	if (dx == 0 && dy ==  1) return NBR_DIR_N;
	return -1;
}

namespace nm_workers_detail {
__declspec(thread) NbrSeedJobTls t_nbrJob;
} // namespace nm_workers_detail

namespace nm_workers_detail {
nmgGetSeedPointsAdj_t orig_getSeedPointsAdj = NULL;
} // namespace nm_workers_detail

// --------------------------------------------------------------------
// Stand-in seeds from the neighbour's shipped tile
// --------------------------------------------------------------------
//
// The rule: after the original, when it added nothing and the
// neighbour is not a live non-temp sector (class none or temp), append the
// seeds the neighbour's SHIPPED tile gives that side, by the original's own
// rule, to the work buffer's m_regionSeedPoints with the original's own growth
// call. A live neighbour always wins, including one with no border edge there
// (class zero), which is vanilla's answer. Only type-0 generations the mod runs
// (the thread's record is armed); the four calls exist only in that branch.
// The result is the mesh vanilla generates when that neighbour is loaded with
// its shipped tile, which vanilla does whenever the neighbour's hash matches.
//
// The shipped tile's seeds come from an immutable per-cell record, built by the
// prefetch before the generation (never under processJobCS; below) and
// published once into a 64x64 table. A record holds four seed lists, one per
// side of its cell, each already shifted into the frame of the tile across
// that side. Records are freed at NavMesh::stop, after the game has joined its
// own NavMesh threads, and only when every worker has exited.

enum { NBR_SI_NONE = 0, NBR_SI_SHIP, NBR_SI_PLACE, NBR_SI_NOFILE, NBR_SI_LATE, NBR_SI_HBAD };
enum { NBR_REC_SHIP = 1, NBR_REC_PLACE, NBR_REC_NOFILE };

// Unit step of side s (W E S N), the same order as the hook's directions.
static const int kNbrSideDx[NBR_DIR_COUNT] = { -1, 1,  0, 0 };
static const int kNbrSideDy[NBR_DIR_COUNT] = {  0, 0, -1, 1 };

// The placeholder guard: shipped town tiles are 80-260-face placeholders with no
// boundary edge in any border strip; a real outdoor tile has
// thousands of faces.
static const int NBR_PLACEHOLDER_FACES = 1000;

// How long a MISS waits for a record another thread is building before it goes
// on without it (standIn=late at the hook). One shipped-tile load is tens of
// milliseconds; the wait holds no lock, and the MISS is about to wait on
// processJobCS anyway.
static const DWORD NBR_INFLIGHT_WAIT_MS = 1000;

// m_regionSeedPoints is nm_quality.h's WB_OFF_REGION_SEED_POINTS (+0xA0), the
// hkArray<hkVector4> getSeedPointsFromAdjacentZone appends to at 0x3C9A02-
// 0x3C9A4C (settings+0xA0 data, +0xA8 size, +0xAC capacity and flags).

struct NbrSeedRecord
{
	int          kind;                        // NBR_REC_*
	int          faces;                       // the shipped mesh's face count, 0 without a file
	unsigned int hBits;                       // strip width h the lists were built with (float bits)
	int          count[NBR_DIR_COUNT];        // seeds per side
	float*       seeds[NBR_DIR_COUNT];        // 4 floats (an hkVector4) per seed
};

// Per cell (index x * 64 + y): NULL (not built), NBR_REC_INFLIGHT (a thread is
// building it) or the published NbrSeedRecord*. Written only through
// InterlockedCompareExchangePointer / InterlockedExchangePointer.
static void* volatile g_nbrTable[ZONE_GRID_COUNT] = {};
static void* const    NBR_REC_INFLIGHT = (void*)(uintptr_t)1;

static inline void* NbrTableRead(int idx)
{
	return InterlockedCompareExchangePointer(&g_nbrTable[idx], NULL, NULL);
}

// NavMesh::stop seen, or the workers told to shut down.
static inline bool NbrStopSeen()
{
	return NavMeshStopRequested();
}

// The strip getSeedPointsFromAdjacentZone (0x3C96E0+0xDB..+0x1A7) searches in
// the neighbour's frame for the direction (dirX, dirY) from the target to the
// neighbour, with the original's arithmetic: h = (float)(halfSize.x * 0.2) in
// double; max = (h, 980, h); a positive direction puts max at 0.1f on that axis,
// a negative one puts min at (float)(max - 0.1) in double. y is always
// [0, 980]; the 4th lane is not tested (movmskps & 7).
static void NbrStripBounds(float h, int dirX, int dirY, float lo[3], float hi[3])
{
	lo[0] = 0.0f; lo[1] = 0.0f; lo[2] = 0.0f;
	hi[0] = h;    hi[1] = 980.0f; hi[2] = h;
	if (dirX > 0) hi[0] = 0.1f;
	if (dirX < 0) lo[0] = (float)((double)hi[0] - 0.1);
	if (dirY > 0) hi[2] = 0.1f;
	if (dirY < 0) lo[2] = (float)((double)hi[2] - 0.1);
}

static inline bool NbrInStrip(const float* v, const float lo[3], const float hi[3])
{
	return lo[0] <= v[0] && v[0] <= hi[0]
	    && lo[1] <= v[1] && v[1] <= hi[1]
	    && lo[2] <= v[2] && v[2] <= hi[2];
}

// The seed rule of getSeedPointsFromAdjacentZone for one side, over every face
// of a mesh: each face's user edges, then its own edges (the original's order,
// 0x3C9932-0x3C9AD3), and every edge with m_oppositeEdge == -1 (+8), both
// vertices inside the strip and a squared length above 0.1f gives one seed:
// (b - a) * 0.5 + a per lane, plus (dirX * h, 0, dirY * h, 0) (0x3C98E0,
// 0x3C9A52-0x3C9A83). The original walks only the faces the neighbour's query
// mediator returns for the strip's AABB; every face that holds a qualifying
// edge overlaps that AABB (the edge lies inside it), so the set of seeds is the
// same and only their order can differ, which pruning does not read. Returns
// the count; out (4 floats per seed) may be NULL for the counting pass. POD
// only: plain reads of a mesh this thread loaded, indices bounds-checked.
static int NbrCollectSide(const char* faces, int nFaces, const char* edges, int nEdges,
                          const float* verts, int nVerts, float h, int dirX, int dirY,
                          float* out)
{
	float lo[3], hi[3];
	NbrStripBounds(h, dirX, dirY, lo, hi);
	const float shift[4] = { (float)dirX * h, 0.0f, (float)dirY * h, 0.0f };

	int count = 0;
	for (int fi = 0; fi < nFaces; ++fi)
	{
		const char* face = faces + HKAI_FACE_SIZE * fi;
		int   startEdge = *(const int*)(face + 0);
		int   startUser = *(const int*)(face + 4);
		short numEdges  = *(const short*)(face + 8);
		short numUser   = *(const short*)(face + 10);
		for (int pass = 0; pass < 2; ++pass)
		{
			int first = (pass == 0) ? startUser : startEdge;
			int num   = (pass == 0) ? numUser   : numEdges;
			if (first < 0 || num <= 0)
				continue;
			for (int e = first; e < first + num && e < nEdges; ++e)
			{
				const char* edge = edges + HKAI_EDGE_SIZE * e;
				if (*(const int*)(edge + 8) != -1)
					continue;
				int a = *(const int*)(edge + 0);
				int b = *(const int*)(edge + 4);
				if (a < 0 || b < 0 || a >= nVerts || b >= nVerts)
					continue;
				const float* va = verts + 4 * a;
				const float* vb = verts + 4 * b;
				if (!NbrInStrip(va, lo, hi) || !NbrInStrip(vb, lo, hi))
					continue;
				float dx = va[0] - vb[0];
				float dy = va[1] - vb[1];
				float dz = va[2] - vb[2];
				float d2 = dz * dz + (dy * dy + dx * dx);
				if (!(d2 > 0.1f))
					continue;
				if (out)
				{
					float* s = out + 4 * count;
					for (int k = 0; k < 4; ++k)
					{
						float mid = (vb[k] - va[k]) * 0.5f + va[k];
						s[k] = shift[k] + mid;
					}
				}
				++count;
			}
		}
	}
	return count;
}

// A record for a cell with nothing to give (kind NOFILE or PLACE), or one with
// the four sides extracted from a loaded shipped mesh. malloc, never Havok: the
// record outlives the mesh and is freed outside any Havok context. NULL only
// when the allocation fails (the cell is then left unbuilt and reads late).
static NbrSeedRecord* NbrMakeRecord(int kind, int faces, float h)
{
	NbrSeedRecord* rec = (NbrSeedRecord*)malloc(sizeof(NbrSeedRecord));
	if (!rec) return NULL;
	memset(rec, 0, sizeof(*rec));
	rec->kind  = kind;
	rec->faces = faces;
	memcpy(&rec->hBits, &h, sizeof(h));
	return rec;
}

static NbrSeedRecord* NbrExtractRecord(uintptr_t mesh, float h)
{
	int nFaces = mesh ? hkArrayGetCount(mesh, NMOFF_FACES)    : 0;
	int nEdges = mesh ? hkArrayGetCount(mesh, NMOFF_EDGES)    : 0;
	int nVerts = mesh ? hkArrayGetCount(mesh, NMOFF_VERTICES) : 0;
	const char*  faces = mesh ? (const char*)hkArrayGetPtr(mesh, NMOFF_FACES)     : NULL;
	const char*  edges = mesh ? (const char*)hkArrayGetPtr(mesh, NMOFF_EDGES)     : NULL;
	const float* verts = mesh ? (const float*)hkArrayGetPtr(mesh, NMOFF_VERTICES) : NULL;
	if (!faces || !edges || !verts || nFaces <= 0 || nEdges <= 0 || nVerts <= 0)
		return NbrMakeRecord(NBR_REC_PLACE, nFaces > 0 ? nFaces : 0, h);
	if (nFaces < NBR_PLACEHOLDER_FACES)
		return NbrMakeRecord(NBR_REC_PLACE, nFaces, h);

	// Side s of this cell faces the tile T at cell + side(s); the direction
	// from T to this cell, which the original's strip and shift use, is -side(s).
	int counts[NBR_DIR_COUNT];
	int total = 0;
	for (int s = 0; s < NBR_DIR_COUNT; ++s)
	{
		counts[s] = NbrCollectSide(faces, nFaces, edges, nEdges, verts, nVerts, h,
		                           -kNbrSideDx[s], -kNbrSideDy[s], NULL);
		total += counts[s];
	}
	if (total == 0)
		return NbrMakeRecord(NBR_REC_PLACE, nFaces, h);

	size_t bytes = sizeof(NbrSeedRecord) + (size_t)total * 4 * sizeof(float) + 16;
	NbrSeedRecord* rec = (NbrSeedRecord*)malloc(bytes);
	if (!rec) return NULL;
	memset(rec, 0, sizeof(*rec));
	rec->kind  = NBR_REC_SHIP;
	rec->faces = nFaces;
	memcpy(&rec->hBits, &h, sizeof(h));
	// Seeds are copied out 16 bytes at a time with memcpy, so the 16-byte
	// alignment an hkVector4 has in the work buffer is not needed here.
	float* cursor = (float*)(rec + 1);
	for (int s = 0; s < NBR_DIR_COUNT; ++s)
	{
		rec->seeds[s] = cursor;
		rec->count[s] = NbrCollectSide(faces, nFaces, edges, nEdges, verts, nVerts, h,
		                               -kNbrSideDx[s], -kNbrSideDy[s], cursor);
		cursor += 4 * rec->count[s];
	}
	return rec;
}

// RAII over the navmesh build mutex (lockZone(..., wait=true) / unlockZone),
// the way stitchUnloadedZone brackets the same calls. getFilename, loadZone and
// deleteMesh carry C++ unwind state; if anything unwinds through the build, the
// mutex is still released (a build mutex left held would stall every section
// add, zone creation and tile save on the path thread for good).
struct NbrBuildMutexScope
{
	void* nmg;
	int   coords[2];
	bool  held;

	NbrBuildMutexScope(void* n, int x, int y) : nmg(n), held(false)
	{
		coords[0] = x;
		coords[1] = y;
		held = fn_nmgLockZone(nmg, coords, true);
	}
	~NbrBuildMutexScope()
	{
		if (held)
			fn_nmgUnlockZone(nmg, coords);
	}

private:
	NbrBuildMutexScope(const NbrBuildMutexScope&);
	NbrBuildMutexScope& operator=(const NbrBuildMutexScope&);
};

// The file name std::string getFilename constructs (the game's own allocation),
// released through the game's own string routine: no CRT string object of ours
// on a NavMesh thread.
struct NbrGameString
{
	unsigned __int64 storage[GAME_STRING_SIZE / 8];
	bool             constructed;

	NbrGameString() : constructed(false) { memset(storage, 0, sizeof(storage)); }
	~NbrGameString() { Release(); }
	void* Raw() { return storage; }
	void Release()
	{
		if (constructed)
			fn_gameStringRelease(storage);
		constructed = false;
	}

private:
	NbrGameString(const NbrGameString&);
	NbrGameString& operator=(const NbrGameString&);
};

// Loads cell (x, y)'s shipped tile and extracts its record, exactly as
// stitchUnloadedZone (0x3CB140) loads an unloaded neighbour on these threads:
//   1. the build mutex, blocking (lockZone(..., 1) 0x3C73B0)
//   2. the ZoneMap (getZoneMap 0xA07C10, done by the caller)
//   3. getFilename(zone, BASE = 1, write = false) 0x3A6230
//   4. loadZone(navmesh, zone, name) 0x3A7F10
//   5. the string released (the game's routine 0x69160), as stitchUnloadedZone
//      does right after loadZone
//   6. the four border strips extracted (NbrExtractRecord)
//   7. deleteMesh(navmesh, sector) 0x3AC8B0
//   8. the build mutex released (unlockZone 0x3BF560)
// Lock order: the caller holds nothing (not processJobCS, buildCollisionCS,
// nmCacheCS or the generator queue lock), so the build mutex is a leaf here.
// Inside it only the game's loader runs (Havok allocations, its own file I/O
// and Ogre log line, a read of the world's streaming collection); none of it
// takes a mod lock. Returns NULL when the stop was seen after the mutex was
// won (nothing loaded) or an allocation failed.
static NbrSeedRecord* NbrLoadShippedRecord(void* realNMG, void* navmesh, void* zone,
                                           int x, int y, float h)
{
	NbrBuildMutexScope buildMutex(realNMG, x, y);
	if (NbrStopSeen())
		return NULL;

	NbrGameString name;
	fn_navMeshGetFilename(name.Raw(), zone, NAVMESH_FILE_BASE, false);
	name.constructed = true;
	void* sector = fn_navMeshLoadZone(navmesh, zone, name.Raw());
	name.Release();

	if (!sector)
		return NbrMakeRecord(NBR_REC_NOFILE, 0, h);

	NbrSeedRecord* rec = NbrExtractRecord(*(uintptr_t*)((uintptr_t)sector + OFF_NAVINST_MESH), h);
	fn_navMeshDeleteSector(navmesh, sector);
	return rec;
}

// Builds and publishes cell idx's record. The caller has claimed the slot
// (NULL -> NBR_REC_INFLIGHT); it is left published, or back at NULL when the
// build was abandoned (stop, retire, allocation), so a later MISS can retry.
static void NbrBuildCell(void* realNMG, void* navmesh, void* zoneMgr, int x, int y)
{
	int idx = x * 64 + y;
	NbrSeedRecord* rec = NULL;
	void* zone = fn_zmGetZoneMap(zoneMgr, x, y);
	float h = 0.0f;
	if (zone)
	{
		float halfX = *(const float*)(KLIB_MEMBER(4, zone, ZoneMap_bounds_mHalfSize_x, OFF_ZONE_AABB_HALF));
		h = (float)((double)halfX * 0.2);
	}
	if (!zone || !(h > 1.0f && h < 100000.0f))
	{
		rec = NbrMakeRecord(NBR_REC_NOFILE, 0, h);
	}
	else
	{
		// The retire's handshake: Havok memory is touched only
		// while the retire has not returned, and the retire waits briefly for
		// a load that began before it.
		if (!WorkerCleanupBegin())
		{
			InterlockedExchangePointer(&g_nbrTable[idx], NULL);
			return;
		}
		LONGLONG t0 = QpcNow();
		rec = NbrLoadShippedRecord(realNMG, navmesh, zone, x, y, h);
		LONGLONG t1 = QpcNow();
		WorkerCleanupEnd();
		long us = QpcDeltaUs(t0, t1);
		InterlockedIncrement(&nmNbrLoadCount);
		InterlockedExchangeAdd64(&nmNbrLoadTotalUs, (LONGLONG)us);
		NoteMaxUs(&nmNbrLoadMaxUs, us);
	}

	if (rec)
		InterlockedIncrement(&nmNbrRecordCount);
	InterlockedExchangePointer(&g_nbrTable[idx], rec);   // NULL = retry later
}

namespace nm_workers_detail {
// The prefetch: before a type-0 MISS generates, on the MISS path only (after
// the L1/L2 lookups failed), make sure the records of the job's four
// neighbours exist. Runs on the worker (after CloneNMG released processJobCS)
// or the bg thread, right before missLock: nothing is held, and never
// processJobCS. A neighbour that is a live non-temp sector now is skipped (the
// game will read its own mesh); a record another thread is building is waited
// for, bounded. Skipped entirely once NavMesh::stop is seen; each load is also
// refused after the worker retire has returned (NbrBuildCell).
void NbrSeedPrefetch(uintptr_t realNMG, uintptr_t jobZone)
{
	if (!NmNbrSeedStandInActive() || InterlockedCompareExchange(&g_nbrSeedHookState, 0, 0) != 1)
		return;
	if (NbrStopSeen() || !realNMG || !jobZone)
		return;
	void* zoneMgr = g_cachedZoneMgr;   // written by the main thread each frame, never changes
	void* navmesh = *(void**)(KLIB_MEMBER(4, realNMG, NavMeshGenerator_navmesh, 0xF0));
	if (!zoneMgr || !navmesh)
		return;

	int gx = *(const int*)(KLIB_MEMBER(4, jobZone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	int gy = *(const int*)(KLIB_MEMBER(4, jobZone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));

	bool waitFor[NBR_DIR_COUNT] = { false, false, false, false };
	for (int d = 0; d < NBR_DIR_COUNT; ++d)
	{
		if (NbrStopSeen())
			return;
		int nx = gx + kNbrSideDx[d];
		int ny = gy + kNbrSideDy[d];
		if (nx < 0 || nx > ZONE_GRID_MAX || ny < 0 || ny > ZONE_GRID_MAX)
			continue;   // no neighbour cell: the hook counts nofile
		int idx = nx * 64 + ny;
		void* cur = NbrTableRead(idx);
		if (cur == NBR_REC_INFLIGHT)
		{
			waitFor[d] = true;
			continue;
		}
		if (cur)
			continue;   // published

		// Live and not temp right now: the game will take its own mesh.
		int coords[2] = { nx, ny };
		void* sector = fn_navMeshGetSector(navmesh, coords, false);
		if (sector && !*(volatile unsigned char*)((uintptr_t)sector + OFF_NAVINST_TEMP))
			continue;

		if (InterlockedCompareExchangePointer(&g_nbrTable[idx], NBR_REC_INFLIGHT, NULL) == NULL)
			NbrBuildCell((void*)realNMG, navmesh, zoneMgr, nx, ny);
		else if (NbrTableRead(idx) == NBR_REC_INFLIGHT)
			waitFor[d] = true;
	}

	LONGLONG start = QpcNow();
	for (;;)
	{
		bool pending = false;
		for (int d = 0; d < NBR_DIR_COUNT; ++d)
		{
			if (!waitFor[d])
				continue;
			int idx = (gx + kNbrSideDx[d]) * 64 + (gy + kNbrSideDy[d]);
			if (NbrTableRead(idx) == NBR_REC_INFLIGHT)
				pending = true;
			else
				waitFor[d] = false;
		}
		if (!pending || NbrStopSeen())
			break;
		if (QpcDeltaUs(start, QpcNow()) >= (long)NBR_INFLIGHT_WAIT_MS * 1000)
			break;
		Sleep(1);
	}
}
} // namespace nm_workers_detail

// The stand-in for one call of the hook (class none or temp, the original added
// nothing). Returns NBR_SI_*; *added gets the seeds appended. Runs inside
// processJobAlt under processJobCS: a table read, and at most one growth call
// per push through the game's own allocator (the calling thread's Havok heap),
// exactly as the original pushes.
static int NbrApplyStandIn(void* nmg, const void* zone, int zx, int zy, int dx, int dy, int* added)
{
	*added = 0;
	int nx = zx + dx;
	int ny = zy + dy;
	if (nx < 0 || nx > ZONE_GRID_MAX || ny < 0 || ny > ZONE_GRID_MAX)
		return NBR_SI_NOFILE;
	void* p = NbrTableRead(nx * 64 + ny);
	if (!p || p == NBR_REC_INFLIGHT)
		return NBR_SI_LATE;
	const NbrSeedRecord* rec = (const NbrSeedRecord*)p;
	if (rec->kind == NBR_REC_NOFILE)
		return NBR_SI_NOFILE;
	if (rec->kind != NBR_REC_SHIP)
		return NBR_SI_PLACE;

	// The record was built with the neighbour zone's strip width; the original
	// uses the target zone's. Zones share one size, so these match; a record
	// that does not is not used (siHBad=).
	float halfX = *(const float*)(KLIB_MEMBER(4, zone, ZoneMap_bounds_mHalfSize_x, OFF_ZONE_AABB_HALF));
	float h = (float)((double)halfX * 0.2);
	unsigned int hBits;
	memcpy(&hBits, &h, sizeof(h));
	if (hBits != rec->hBits)
		return NBR_SI_HBAD;

	// The neighbour's side facing this tile.
	int side = NbrDirIndex(-dx, -dy);
	if (side < 0 || rec->count[side] <= 0)
		return NBR_SI_PLACE;

	uintptr_t settings = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256));
	if (!settings)
		return NBR_SI_LATE;
	uintptr_t arr = settings + WB_OFF_REGION_SEED_POINTS;
	void* sAlloc = GameAddr(RVA_HK_CONTAINER_HEAP_ALLOC);
	const float* src = rec->seeds[side];
	for (int i = 0; i < rec->count[side]; ++i)
	{
		int* sizeSlot = (int*)(KLIB_MEMBER(4, arr, ByteArray_m_size, 8));
		int  capFlags = *(int*)(KLIB_MEMBER(4, arr, ByteArray_m_capacityAndFlags, 12));
		if (*sizeSlot == (capFlags & 0x3FFFFFFF))
			fn_sortedArrayGrow(sAlloc, (void*)arr, 16);
		char* data = *(char**)(KLIB_MEMBER(4, arr, ByteArray_m_data, 0));
		memcpy(data + 16 * (size_t)*sizeSlot, src + 4 * i, 16);
		*sizeSlot = *sizeSlot + 1;
	}
	*added = rec->count[side];
	return NBR_SI_SHIP;
}

namespace nm_workers_detail {
// The callees the stand-in needs beyond the KenshiLib registry (which already
// cross-checked the covered ones at startup): the two uncovered functions by
// their first 16 bytes, and the allocator object by its vtable. Any mismatch
// refuses the stand-in for the session. NavMesh bg thread, at the lazy install.
bool NbrCheckStandInCallees()
{
	if (!fn_navMeshGetFilename || !fn_navMeshLoadZone || !fn_navMeshDeleteSector
	    || !fn_nmgLockZone || !fn_nmgUnlockZone || !fn_zmGetZoneMap
	    || !fn_sortedArrayGrow || !fn_gameStringRelease)
		return false;
	bool ok = VerifyPrologue(RVA_SORTED_ARRAY_GROW, g_sortedArrayGrowBytes, "SortedArray__grow (called)")
	       && VerifyPrologue(RVA_GAME_STRING_RELEASE, g_gameStringReleaseBytes, "std::string release (called)");
	uintptr_t vtbl = *(uintptr_t*)GameAddr(RVA_HK_CONTAINER_HEAP_ALLOC);
	if (vtbl != (uintptr_t)GameAddr(RVA_HK_CONTAINER_HEAP_VTBL))
	{
		char line[160];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"neighbour seeds: hkContainerHeapAllocator::s_alloc vtable 0x%llx, expected 0x%llx",
			(unsigned long long)vtbl, (unsigned long long)(uintptr_t)GameAddr(RVA_HK_CONTAINER_HEAP_VTBL));
		LogMsgDeferrable(line);
		ok = false;
	}
	return ok;
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
// Frees every record. Only from the NavMesh::stop hook, after the original has
// run (the game has joined its bg thread and deleted the generator, so no
// generation and no bg prefetch remains) and only when no worker is alive,
// since a live worker could still read a record. The retire returns only with
// none live, so that test is a backstop. Otherwise the records are left to the
// process exit.
void NbrSeedFreeTable()
{
	for (int i = 0; i < ZONE_GRID_COUNT; ++i)
	{
		void* p = InterlockedExchangePointer(&g_nbrTable[i], NULL);
		if (p && p != NBR_REC_INFLIGHT)
			free(p);
	}
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
int hook_getSeedPointsAdj(void* nmg, const void* zone, const int* dir)
{
	int n = orig_getSeedPointsAdj(nmg, zone, dir);
	if (!nmg || !zone || !dir)
		return n;

	int dx = dir[0];
	int dy = dir[1];
	int zx = *(const int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	int zy = *(const int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));

	int cls;
	if (n > 0)
	{
		cls = NBR_CLASS_LIVE;
	}
	else
	{
		void* navmesh = *(void**)(KLIB_MEMBER(4, nmg, NavMeshGenerator_navmesh, 0xF0));
		int coords[2] = { zx + dx, zy + dy };
		void* sector = (navmesh && fn_navMeshGetSector) ? fn_navMeshGetSector(navmesh, coords, false) : NULL;
		if (!sector)
			cls = NBR_CLASS_NONE;
		else if (*(volatile unsigned char*)((uintptr_t)sector + OFF_NAVINST_TEMP))
			cls = NBR_CLASS_TEMP;
		else
			cls = NBR_CLASS_ZERO;
	}

	switch (cls)
	{
	case NBR_CLASS_LIVE: InterlockedIncrement(&nmNbrSeedLive); break;
	case NBR_CLASS_TEMP: InterlockedIncrement(&nmNbrSeedTemp); break;
	case NBR_CLASS_ZERO: InterlockedIncrement(&nmNbrSeedZero); break;
	default:             InterlockedIncrement(&nmNbrSeedNone); break;
	}

	int d = NbrDirIndex(dx, dy);
	if (t_nbrJob.armed && d >= 0)
	{
		t_nbrJob.cls[d]   = (unsigned char)cls;
		t_nbrJob.seeds[d] = n;

		// The stand-in applies only where the game added nothing and the neighbour is not a
		// live non-temp sector. The return value stays the original's: its
		// only caller discards it.
		if ((cls == NBR_CLASS_NONE || cls == NBR_CLASS_TEMP) && NmNbrSeedStandInActive())
		{
			int added = 0;
			int si = NbrApplyStandIn(nmg, zone, zx, zy, dx, dy, &added);
			t_nbrJob.standIn[d]      = (unsigned char)si;
			t_nbrJob.standInSeeds[d] = added;
			switch (si)
			{
			case NBR_SI_SHIP:
				InterlockedIncrement(&nmNbrStandInShip);
				InterlockedExchangeAdd64(&nmNbrStandInSeeds, (LONGLONG)added);
				break;
			case NBR_SI_PLACE:  InterlockedIncrement(&nmNbrStandInPlace);  break;
			case NBR_SI_NOFILE: InterlockedIncrement(&nmNbrStandInNoFile); break;
			case NBR_SI_HBAD:   InterlockedIncrement(&nmNbrStandInHBad);   break;
			default:
				InterlockedIncrement(&nmNbrStandInLate);
				t_nbrJob.late = 1;   // this job's mesh stays out of L2
				break;
			}
		}
	}
	return n;
}
} // namespace nm_workers_detail


namespace nm_workers_detail {
// Around ProcessNavMeshJob's processJobAlt call. Begin arms the thread's record
// for a type-0 job (the only type whose generation calls the hook); End
// disarms it and, in DEV, queues one line per generation:
//   NbrSeeds grid=(x,y) W=<class>[:n] E=... S=... N=...
// class live:<n> (the original's seeds), zero, temp, none; "-" for a direction
// the generation never asked about. Fixed buffer, _snprintf_s and
// LogMsgDeferrable: this runs on the NavMesh bg thread or a worker.
void NbrSeedJobBegin(int jobType, int gridX, int gridY)
{
	memset(&t_nbrJob, 0, sizeof(t_nbrJob));
	if (jobType != 0 || !NmNbrSeedHookWanted()
	    || InterlockedCompareExchange(&g_nbrSeedHookState, 0, 0) != 1)
		return;
	t_nbrJob.gridX = gridX;
	t_nbrJob.gridY = gridY;
	t_nbrJob.armed = 1;
}
} // namespace nm_workers_detail

namespace nm_workers_detail {
void NbrSeedJobEnd()
{
	if (!t_nbrJob.armed)
		return;
	t_nbrJob.armed = 0;
#ifdef ZONEOPT_DEBUG
	static const char* const kDirNames[NBR_DIR_COUNT] = { "W", "E", "S", "N" };
	char line[DEFERRED_LOG_CHARS];
	size_t pos = 0;
	int w = _snprintf_s(line, sizeof(line), _TRUNCATE, "NbrSeeds grid=(%d,%d)",
	                    t_nbrJob.gridX, t_nbrJob.gridY);
	pos = (w > 0) ? (size_t)w : 0;
	bool any = false;
	for (int d = 0; d < NBR_DIR_COUNT && pos < sizeof(line); ++d)
	{
		const char* cls = "-";
		switch (t_nbrJob.cls[d])
		{
		case NBR_CLASS_LIVE: cls = "live"; break;
		case NBR_CLASS_ZERO: cls = "zero"; break;
		case NBR_CLASS_TEMP: cls = "temp"; break;
		case NBR_CLASS_NONE: cls = "none"; break;
		default: break;
		}
		if (t_nbrJob.cls[d] != NBR_CLASS_UNSEEN)
			any = true;
		if (t_nbrJob.cls[d] == NBR_CLASS_LIVE)
			w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, " %s=%s:%d",
			                kDirNames[d], cls, t_nbrJob.seeds[d]);
		else
			w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, " %s=%s",
			                kDirNames[d], cls);
		if (w < 0) break;
		pos += (size_t)w;
		// What the stand-in did there, e.g. W=none->ship:18.
		if (t_nbrJob.standIn[d] != NBR_SI_NONE && pos < sizeof(line))
		{
			switch (t_nbrJob.standIn[d])
			{
			case NBR_SI_SHIP:
				w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "->ship:%d",
				                t_nbrJob.standInSeeds[d]);
				break;
			case NBR_SI_PLACE:  w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "->place");  break;
			case NBR_SI_NOFILE: w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "->nofile"); break;
			case NBR_SI_HBAD:   w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "->hbad");   break;
			default:            w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "->late");   break;
			}
			if (w < 0) break;
			pos += (size_t)w;
		}
	}
	if (any)
		LogMsgDeferrable(line);
#endif
}
} // namespace nm_workers_detail
