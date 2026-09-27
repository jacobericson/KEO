// nm_fresh_wb.cpp - fresh work-buffer construction and teardown on generating threads.
// Construction reads the real WB under processJobCS; teardown keeps each caller's
// capture/free order, including outside that lock under a worker's retire handshake.
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
// WorkBuffer (hkaiNavMeshGenerationSettings) construction
// --------------------------------------------------------------------
//
// Build a fresh 544-byte settings object via the real constructor (0xDD99D0),
// then copy ONLY scalar config regions from the original. The sub-structs'
// hkArrays (+160/+176 in RegionPruningSettings at +144, +464 userVertices in
// ExtraVertexSettings at +424) stay at ctor defaults — shallow-copying them
// shares internally-allocated pointers with realWB, so processJobAlt's
// subsequent hkArray growth would free buffers realWB still owns. Their scalar
// heads are copied (+144..+159 and +424..+463).
// +192 (WallClimbingSettings) and +304..+316 (fixupOverlappingTriangles and its
// settings) are not copied: the game leaves both at the ctor values, which the
// DEV "NavMesh freshWB diff:" audit confirms each session.
//
// The hkArrays (carvers +240, painters +256, materialMap +288,
// overrideSettings +520) are handled specially:
//   - carvers/painters: left at the ctor state (empty, DONT_DEALLOCATE).
//     SortedArray__grow (0xBA3AA0) takes the flag path on the first push
//     (bufAlloc + copy, the same path the real WB's arrays take) and writes
//     the new capacity without the flag; an array the job never grew stays
//     at capacity 0 with DONT_DEALLOCATE still set, so the ctor state keeps
//     the fresh WB on the vanilla path.
//   - overrideSettings: guard slot + fixed capacity, DONT_DEALLOCATE (below).
//   - materialMap: per-clone deep-copy of the pointer array into a clone-owned
//     buffer. Eliminates the last DONT_DEALLOCATE shared buffer from realWB.
//
// Constructor writes +8 = 0xFFFF0001: m_referenceCount (low word at +8) = 1,
// m_memSizeAndFlags (word at +10) = 0xFFFF = class-default size. Refcounting
// is enabled (not immortal); nothing releases the WB, FreeFreshSettings frees
// it directly.
static const int OVR_ENTRY_SIZE  = 240;                             // OverrideSettings entry (SortedArray__grow elemSize)
static const int OVR_GUARD_CAP   = 8;                               // capacity; one append per job
static const int OVR_GUARD_BYTES = OVR_ENTRY_SIZE * (OVR_GUARD_CAP + 1);  // + the guard slot at entry[-1]

// DEV probe state for the material overrides (reported as wbOv= / wbOvSlope=).
volatile long g_wbOverrideInstalled = 0;   // entries appended by the last construct
volatile long g_wbOverrideAfterPop  = -1;  // count read back after processJobAlt
volatile long g_wbOverrideSlopeBad  = 0;   // table slope != the real WB's slope
volatile long g_wbOverrideSkipped   = 0;   // teardown left entries alone (count > capacity)

// OverrideSettings::dtor is at 0xDD92F0, the address hook_edgeProcess patches.
// Call the unhooked body through the trampoline so our teardown neither trips
// the clone-guard nor moves the edge=a/u counters, which count the game's own
// calls (two per MISS) and are a diagnostic in their own right.
static void CallOverrideSettingsDtor(void* entry)
{
	if (orig_edgeProcess)
		orig_edgeProcess(entry);
	else
		((edgeProcess_t)GameAddr(RVA_EDGE_PROCESS))(entry);
}

// Duplicates the real WB's four material-override entries onto a fresh WB.
// `dst` and `src` are work buffers; the fresh array is the guard-slot buffer
// installed just above, empty and with room for OVR_GUARD_CAP entries.
static void AppendMaterialOverrides(char* dst, const char* src)
{
	char* srcArr = *(char**)(KLIB_MEMBER(4, src + 520, ByteArray_m_data, 0));
	int   srcCnt = *(int*)(KLIB_MEMBER(4, src + 520, ByteArray_m_size, 8));
	char* dstArr = *(char**)(KLIB_MEMBER(4, dst + 520, ByteArray_m_data, 0));

	// Report 0 unless the whole run is installed below, so wbOv= describes this
	// construct rather than the last successful one.
	InterlockedExchange(&g_wbOverrideInstalled, 0);

	if (!srcArr || !dstArr || srcCnt < NM_MATERIAL_OVERRIDE_COUNT)
		return;
	if (!fn_simplSettingsCopy)
		return;

	// Validate the whole run before copying any of it, so a layout that has
	// moved leaves the fresh WB with an empty array (the guard slot alone)
	// instead of a half-built one.
	//   +0 must be NULL: OverrideSettings__initFromSettings writes NULL there
	//     and initWorkBuffer never replaces it, which is also what makes these
	//     entries stop finalizeDeep's pop loop.
	//   +8 must be the material id 1..4, in order.
	int i;
	for (i = 0; i < NM_MATERIAL_OVERRIDE_COUNT; ++i)
	{
		const char* se = srcArr + (size_t)OVR_ENTRY_SIZE * i;
		if (*(void* const*)(se + OVR_OFF_VOLUME) != NULL) return;
		if (*(const int*)(se + OVR_OFF_MATERIAL) != i + 1) return;
	}

	for (i = 0; i < NM_MATERIAL_OVERRIDE_COUNT; ++i)
	{
		const char* se = srcArr + (size_t)OVR_ENTRY_SIZE * i;
		char*       de = dstArr + (size_t)OVR_ENTRY_SIZE * i;

		memset(de, 0, OVR_ENTRY_SIZE);
		*(void**)(de + OVR_OFF_VOLUME) = NULL;              // volume-less, nothing to addref
		*(int*)(de + OVR_OFF_MATERIAL) = i + 1;
		*(char*)(de + OVR_OFF_FLAG)    = *(const char*)(se + OVR_OFF_FLAG);
		*(float*)(de + OVR_OFF_SLOPE)  = NmMaterialSlope(i);
		memcpy(de + OVR_OFF_EMP, se + OVR_OFF_EMP, 56);
		fn_simplSettingsCopy(de + OVR_OFF_SIMPL, (void*)(se + OVR_OFF_SIMPL));

		// The table is authoritative, but it is transcribed from the decompile,
		// so check it against what the game actually installed.
		if (*(const float*)(se + OVR_OFF_SLOPE) != NmMaterialSlope(i))
			InterlockedIncrement(&g_wbOverrideSlopeBad);
	}

	*(int*)(KLIB_MEMBER(4, dst + 520, ByteArray_m_size, 8)) = NM_MATERIAL_OVERRIDE_COUNT;
	InterlockedExchange(&g_wbOverrideInstalled, NM_MATERIAL_OVERRIDE_COUNT);
}

// --------------------------------------------------------------------
// Fresh-WB proof token and audit
// --------------------------------------------------------------------

// The prune= stats token (nm_cache_core.h): the region-pruning scalars of the
// session's first fresh WB, read back after every copy. Any thread, no locks
// or allocation; the claim makes it once even if two constructs overlap.
static void NoteFreshWbPrune(const char* f)
{
	if (InterlockedCompareExchange(&g_wbPruneSeen, 1, 0) != 0)
		return;
	const NmRegionPruningScalars* rp = (const NmRegionPruningScalars*)(f + WB_OFF_REGION_PRUNING);
	InterlockedExchange(&g_wbPruneAreaBits,   (long)rp->minRegionAreaBits);
	InterlockedExchange(&g_wbPruneSeedBits,   (long)rp->minDistanceToSeedPointsBits);
	InterlockedExchange(&g_wbPruneBorderBits, (long)rp->borderPreservationToleranceBits);
	InterlockedExchange(&g_wbPruneFlags,
	                    (long)rp->preserveVerticalBorderRegions | ((long)rp->pruneBeforeTriangulation << 8));
	InterlockedExchange(&g_wbPruneSeen, 2);
}

#ifdef ZONEOPT_DEBUG
// The one-per-session DEV audit: every scalar field of the first fresh WB
// against the real WB it was built from, logged as
//   NavMesh freshWB diff: <n> range(s) [<start>..<end> ...] ov=<fresh>/<real> ...
// A range is a run of adjacent differing fields (byte offsets, inclusive);
// "ov<i>+<a>..<b>" is inside override entry i, "ovCount=<fresh>/<real>" a count
// mismatch. Expected, with navmeshVanillaPruning on: 0 ranges. With it off
// the known gaps show: [144..155] (region pruning) and [428..435 448..449]
// (extra vertices).
//
// The field lists are Havok's reflection tables in the binary: the settings
// member table 0x2113FC0, EdgeMatchingParameters 0x17B5530,
// RegionPruningSettings 0x17B41A0, WallClimbingSettings 0x17B42F0,
// SimplificationSettings 0x17B3560 and ExtraVertexSettings 0x2113AA0, with the
// ctor 0xDD99D0 for OverlappingTrianglesSettings (+0x134: two floats, a bool).
// Padding is left out (the ctor never writes it, so both sides hold heap
// bytes). Skipped by design, because they differ by construction:
//   - the header +0..+15 (vtable, refcount);
//   - pointers: +272 painterOverlapCallback, the hkStringPtrs at +488 / +512;
//   - every hkArray header: +160 / +176 (pruning seeds and connections, per
//     job), +240 carvers, +256 painters, +288 materialMap (our own copy),
//     +464 userVertices, +520 overrides (the guard slot and the
//     DONT_DEALLOCATE buffers); the four material overrides are compared by
//     value instead, entry by entry;
//   - +208..+239 boundsAabb: per-job input, written by generateTaskBT
//     (0x3CBE60 lines 505 / 663) before every use and never reset, so the real
//     WB holds whatever job it ran last.
// The fields are collected into this POD on the thread that builds the WB and
// formatted into a fixed buffer for LogMsgDeferrable: no CRT strings, streams
// or LogMsg off the main thread.
struct WbAuditField { short off; short len; short unit; };

static const WbAuditField kWbAuditTop[] = {
	{  16,  4, 4 },   // characterHeight
	{  32, 16, 4 },   // up (hkVector4)
	{  48,  4, 4 },   // quantizationGridSize
	{  52,  4, 4 },   // maxWalkableSlope
	{  56,  1, 1 },   // triangleWinding
	{  60, 12, 4 },   // degenerateAreaThreshold, degenerateWidthThreshold, convexThreshold
	{  72,  4, 4 },   // maxNumEdgesPerFace
	{  76, 52, 4 },   // edgeMatchingParams: 13 floats
	{ 128,  1, 1 },   // edgeMatchingParams.useSafeEdgeTraversibilityHorizontalEpsilon
	{ 132,  8, 4 },   // edgeMatchingMetric, edgeConnectionIterations
	// +140..+143 is padding, not a field: edgeConnectionIterations (0x88) is a
	// 4-byte int and the reflection table puts regionPruningSettings at 0x90
	// (the IDB type lumps both into _unk08C[0x14]); the ctor never writes it.
	// It is copied anyway by the +132..+143 memcpy, so it cannot differ.
	{ 144, 12, 4 },   // regionPruning: minRegionArea, minDistanceToSeedPoints, borderPreservationTolerance
	{ 156,  2, 1 },   // regionPruning: preserveVerticalBorderRegions, pruneBeforeTriangulation
	{ 192,  2, 1 },   // wallClimbing: enableWallClimbing, excludeWalkableFaces
	{ 280,  4, 4 },   // defaultConstructionProperties
	{ 304,  1, 1 },   // fixupOverlappingTriangles
	{ 308,  8, 4 },   // overlappingTrianglesSettings: two floats
	{ 316,  1, 1 },   // overlappingTrianglesSettings: bool
	{ 320,  1, 1 },   // weldInputVertices
	{ 324,  8, 4 },   // weldThreshold, minCharacterWidth
	{ 332,  2, 1 },   // characterWidthUsage, enableSimplification
	// +336..+495 SimplificationSettings: kWbAuditSimpl at base 336
	{ 496,  8, 4 },   // carvedMaterialDeprecated, carvedCuttingMaterialDeprecated
	{ 504,  2, 1 }    // checkEdgeGeometryConsistency, saveInputSnapshot
};

// SimplificationSettings (160 bytes), relative: the WB's at +336, and each
// override entry's at +80.
static const WbAuditField kWbAuditSimpl[] = {
	{   0, 40, 4 },   // maxBorderSimplifyArea .. maxPartitionSize (10 fields)
	{  40,  1, 1 },   // useHeightPartitioning
	{  44,  4, 4 },   // maxPartitionHeightError
	{  48,  1, 1 },   // useConservativeHeightPartitioning
	{  52, 32, 4 },   // hertelMehlhornHeightError .. maxBoundaryVertexVerticalError (8 fields)
	{  84,  1, 1 },   // mergeLongestEdgesFirst
	{  88,  1, 1 },   // extraVertexSettings.vertexSelectionMethod
	{  92, 20, 4 },   // .vertexFraction, .areaFraction, .minPartitionArea, .numSmoothingIterations, .iterationDamping
	{ 112,  2, 1 },   // .addVerticesOnBoundaryEdges, .addVerticesOnPartitionBorders
	{ 116, 12, 4 },   // .boundaryEdgeSplitLength, .partitionBordersSplitLength, .userVertexOnBoundaryTolerance
	{ 144,  1, 1 }    // saveInputSnapshot
};

// An OverrideSettings entry (240 bytes) apart from its SimplificationSettings.
static const WbAuditField kWbAuditOverride[] = {
	{  8,  4, 4 },    // m_material
	{ 12,  1, 1 },    // m_characterWidthUsage
	{ 16,  4, 4 },    // m_maxWalkableSlope
	{ 20, 52, 4 },    // m_edgeMatchingParams: 13 floats
	{ 72,  1, 1 }     // m_edgeMatchingParams bool
};

static const int WB_AUDIT_MAX_RANGES = 20;
static const short WB_AUDIT_OV_TOP   = -1;   // a range in the WB itself
static const short WB_AUDIT_OV_COUNT = 99;   // the override count differs

struct WbAuditCollector {
	int   fields;                 // fields compared
	int   total;                  // ranges found
	int   stored;                 // ranges kept for the line
	short lastOv, lastEnd;        // the range being extended (valid while total > 0)
	bool  lastStored;             // ... and whether it is ov/start/end[stored - 1]
	short ov[WB_AUDIT_MAX_RANGES];
	short start[WB_AUDIT_MAX_RANGES];
	short end[WB_AUDIT_MAX_RANGES];
};

static void WbAuditNote(WbAuditCollector* c, short ov, short s, short e)
{
	if (c->total > 0 && c->lastOv == ov && c->lastEnd + 1 == s)
	{
		c->lastEnd = e;
		if (c->lastStored)
			c->end[c->stored - 1] = e;
		return;
	}
	c->total++;
	c->lastOv = ov;
	c->lastEnd = e;
	c->lastStored = (c->stored < WB_AUDIT_MAX_RANGES);
	if (c->lastStored)
	{
		c->ov[c->stored] = ov;
		c->start[c->stored] = s;
		c->end[c->stored] = e;
		c->stored++;
	}
}

static void WbAuditCompare(WbAuditCollector* c, const char* f, const char* o, int base,
                           const WbAuditField* tbl, int n, short ov)
{
	for (int i = 0; i < n; ++i)
	{
		for (int u = 0; u < tbl[i].len; u += tbl[i].unit)
		{
			int off = base + tbl[i].off + u;
			c->fields++;
			if (memcmp(f + off, o + off, (size_t)tbl[i].unit) != 0)
				WbAuditNote(c, ov, (short)off, (short)(off + tbl[i].unit - 1));
		}
	}
}

static volatile long g_wbAuditDone = 0;

static void AuditFreshWorkBuffer(const char* f, const char* o)
{
	if (InterlockedCompareExchange(&g_wbAuditDone, 1, 0) != 0)
		return;

	WbAuditCollector c;
	memset(&c, 0, sizeof(c));

	const int nTop   = (int)(sizeof(kWbAuditTop) / sizeof(kWbAuditTop[0]));
	const int nSimpl = (int)(sizeof(kWbAuditSimpl) / sizeof(kWbAuditSimpl[0]));
	const int nOv    = (int)(sizeof(kWbAuditOverride) / sizeof(kWbAuditOverride[0]));

	// Offset order, so adjacent differing fields merge: the top list up to
	// +335, then the SimplificationSettings block, then the rest.
	int split = 0;
	while (split < nTop && kWbAuditTop[split].off < WB_OFF_SIMPLIFICATION)
		++split;
	WbAuditCompare(&c, f, o, 0, kWbAuditTop, split, WB_AUDIT_OV_TOP);
	WbAuditCompare(&c, f, o, WB_OFF_SIMPLIFICATION, kWbAuditSimpl, nSimpl, WB_AUDIT_OV_TOP);
	WbAuditCompare(&c, f, o, 0, kWbAuditTop + split, nTop - split, WB_AUDIT_OV_TOP);

	// The material overrides, by value.
	const char* fArr = *(const char* const*)(KLIB_MEMBER(4, f + 520, ByteArray_m_data, 0));
	const char* oArr = *(const char* const*)(KLIB_MEMBER(4, o + 520, ByteArray_m_data, 0));
	int fCnt = *(const int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_size, 8));
	int oCnt = *(const int*)(KLIB_MEMBER(4, o + 520, ByteArray_m_size, 8));
	if (fCnt != oCnt)
		WbAuditNote(&c, WB_AUDIT_OV_COUNT, (short)fCnt, (short)oCnt);
	int nEntries = fCnt < oCnt ? fCnt : oCnt;
	if (nEntries > NM_MATERIAL_OVERRIDE_COUNT) nEntries = NM_MATERIAL_OVERRIDE_COUNT;
	if (!fArr || !oArr) nEntries = 0;
	for (int i = 0; i < nEntries; ++i)
	{
		const char* fe = fArr + (size_t)OVR_ENTRY_SIZE * i;
		const char* oe = oArr + (size_t)OVR_ENTRY_SIZE * i;
		WbAuditCompare(&c, fe, oe, 0, kWbAuditOverride, nOv, (short)i);
		WbAuditCompare(&c, fe, oe, OVR_OFF_SIMPL, kWbAuditSimpl, nSimpl, (short)i);
	}

	char line[DEFERRED_LOG_CHARS - 64];   // room for LogMsgDeferrable's tag
	size_t pos = 0;
	int w = _snprintf_s(line, sizeof(line), _TRUNCATE, "NavMesh freshWB diff: %d range(s) [", c.total);
	if (w > 0) pos = (size_t)w;
	for (int r = 0; r < c.stored && pos < sizeof(line); ++r)
	{
		const char* sep = r ? " " : "";
		if (c.ov[r] == WB_AUDIT_OV_TOP)
			w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "%s%d..%d", sep, c.start[r], c.end[r]);
		else if (c.ov[r] == WB_AUDIT_OV_COUNT)
			w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "%sovCount=%d/%d", sep, c.start[r], c.end[r]);
		else
			w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, "%sov%d+%d..%d", sep, c.ov[r], c.start[r], c.end[r]);
		if (w < 0) break;
		pos += (size_t)w;
	}
	if (pos < sizeof(line))
	{
		w = _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE,
		                "%s] ov=%d/%d fields=%d prune=%s step=%d skip=hdr,ptr,arrays,aabb208..239",
		                (c.total > c.stored) ? " ..." : "", fCnt, oCnt, c.fields,
		                NmVanillaPruningActive() ? "on" : "off", 2);
		(void)w;
	}
	LogMsgDeferrable(line);
}
#endif // ZONEOPT_DEBUG

namespace nm_workers_detail {
void* ConstructFreshSettings(uintptr_t origWB)
{
	// Every line below reads the original through `o`. Without this the failure
	// mode is a memcpy from address 16, so refuse and let the caller fall back
	// to the real work buffer ("MISS no-swap") the way an allocation failure
	// already does.
	if (!origWB)
	{
		InterlockedIncrement(&nmCloneConstructFailCount);
		return NULL;
	}

	void* mem = HavokTlsAlloc(WB_OBJECT_SIZE);
	if (!mem) return NULL;

	void* fresh = fn_settingsCtor(mem);
	if (!fresh) { HavokTlsFree(mem, WB_OBJECT_SIZE); return NULL; }
	char* f = (char*)fresh;
	char* o = (char*)origWB;

	// +16..+131: generation config + edgeMatchingParameters
	memcpy(f + 16, o + 16, 60);
	memcpy(f + 76, o + 76, 56);

	// +132..+143: scalar ints before +144 sub-struct
	memcpy(f + 132, o + 132, 12);

	// +144..+159: the RegionPruningSettings scalars (nm_quality.h). The ctor
	// left Havok's 5 / 1 / 0.1; the real WB carries Kenshi's 1e8 / 0.4 / 0, with
	// which realGenerate's pruneRegions drops every region no seed point
	// reaches: the sealed pockets that trapped characters in town gates.
	// +160 (seed points) and +176 (region connections) are hkArrays and stay at
	// the ctor state; each job fills the fresh WB's own seed array. Always the
	// real WB's actual bytes, so fresh and real WBs generate alike; a
	// difference from the settings hash's table is handled after the copies.
	if (NmVanillaPruningActive())
		memcpy(f + WB_OFF_REGION_PRUNING, o + WB_OFF_REGION_PRUNING, sizeof(NmRegionPruningScalars));

	// +240/+256 carvers + painters: stay at the ctor state (empty,
	// DONT_DEALLOCATE). SortedArray__grow handles the flag on the first push.

	// +272..+287: scalar config between painters and materialMap
	memcpy(f + 272, o + 272, 16);

	// +288 materialMap: per-clone deep-copy. Raw pointer memcpy (not
	// hkRefPtr::operator=) — scene objects behind the pointers stay shared
	// read-only. Snapshot happens under processJobCS so any writer is serialized.
	{
		int mmCount  = *(int*)(KLIB_MEMBER(4, o + 288, ByteArray_m_size, 8));
		int mmCapLow = *(int*)(KLIB_MEMBER(4, o + 288, ByteArray_m_capacityAndFlags, 12)) & 0x3FFFFFFF;
		int mmCap    = mmCapLow > 0 ? mmCapLow : mmCount;
		if (mmCount > 0 && mmCap > 0)
		{
			void* mmData = HavokTlsAlloc((size_t)mmCap * 8);
			if (mmData)
			{
				memcpy(mmData, *(void**)(KLIB_MEMBER(4, o + 288, ByteArray_m_data, 0)), (size_t)mmCount * 8);
				*(void**)(KLIB_MEMBER(4, f + 288, ByteArray_m_data, 0)) = mmData;
				*(int*)(KLIB_MEMBER(4, f + 288, ByteArray_m_size, 8))   = mmCount;
				*(int*)(KLIB_MEMBER(4, f + 288, ByteArray_m_capacityAndFlags, 12))   = mmCap | HKARRAY_DONT_DEALLOCATE;
			}
		}
	}

	// +320..+335: byte flags + qword after the +308 sub-struct
	memcpy(f + 320, o + 320, 16);

	// +336..+423: SimplificationSettings scalars (SimplificationSettings::copy
	// at 0x3DA000 shallow-copies this region).
	memcpy(f + 336, o + 336, 88);

	// +424..+463: the ExtraVertexSettings scalars (nm_quality.h), inside
	// SimplificationSettings. The real WB has HavokNavMesh__wb424cleanup's
	// zeroes (vertexFraction, areaFraction, both addVertices bools); the ctor
	// left 0.025 / 0.000125 / true / true, and generateTaskBT copies this block
	// into every non-town job's override entry. +464 (userVertices) is an
	// hkArray and stays at the ctor state. The real WB's actual bytes, as above.
	if (NmVanillaPruningActive())
		memcpy(f + WB_OFF_EXTRA_VERTEX, o + WB_OFF_EXTRA_VERTEX, sizeof(NmExtraVertexScalars));

	// The copied blocks are the real WB's, so the fresh WB is
	// right whatever they hold. When they are not what the L2 settings hash
	// describes, this mesh would not match its cache key: count it
	// (pruneBad= / xvBad=) and let NmCheckGenerationSettingsKey turn L2 off
	// for the session (normally already done at the first dispatch).
	if (NmVanillaPruningActive())
	{
		bool pruneOk = true, xvOk = true;
		if (!NmCheckGenerationSettingsKey(f, &pruneOk, &xvOk))
		{
			if (!pruneOk) InterlockedIncrement(&g_wbPruneBad);
			if (!xvOk)    InterlockedIncrement(&g_wbExtraVertexBad);
		}
	}

	// +480..+487: SimplSettings byte + padding
	memcpy(f + 480, o + 480, 8);

	// +496..+511: config between SimplSettings and top-level hkStringPtr
	memcpy(f + 496, o + 496, 16);

	// +520 overrideSettings. processJobAlt appends exactly ONE entry per job
	// (0x3CD3EF) and finalizeDeep (0x3C2300+0x18C) then pops entries from the
	// end until one satisfies (entry+8 != -1 && entry+0 == NULL) — with NO
	// count > 0 check. The real WB owns four base entries pushed by the NMG
	// ctor helper (0x3C48D0) that stop that loop; a ctor-fresh WB has none, so
	// after popping the appended entry the loop reads entry[-1] BEFORE the
	// array and only stops if the neighbouring heap bytes happen to look like
	// a base entry, which can walk into an unmapped page.
	// Fix: a zeroed guard slot in front of a capacity the job can never
	// outgrow, DONT_DEALLOCATE so the game neither frees nor reallocs it.
	// FreeFreshSettings releases it.
	{
		char* ovr = (char*)HavokTlsAlloc(OVR_GUARD_BYTES);
		if (!ovr)
		{
			// A fresh WB without a guard is unsafe: fall back to the real WB
			// ("MISS no-swap"). Release what has been built so far through the
			// same teardown a complete fresh WB gets, so the two cannot drift:
			//   - the object the ctor built: FreeFreshSettings runs the
			//     settings dtor body on it (the +144 sub-struct, +336, +512,
			//     the carver and painter arrays), which the old failure path
			//     skipped by freeing the raw block;
			//   - the +288 material-map copy, when one was made: still ours
			//     (DONT_DEALLOCATE, non-zero capacity), so the body leaves it
			//     and FreeFreshSettings frees it from the pointer it captured
			//     first;
			//   - the 544-byte block.
			// The +520 array is still at the ctor state here (NULL, count 0,
			// DONT_DEALLOCATE with capacity 0), so FreeFreshSettings neither
			// runs an override dtor nor treats it as our guard buffer. Nothing
			// after this point has run yet: no material overrides, so there is
			// nothing else to undo.
			FreeFreshSettings(fresh);
			return NULL;
		}
		memset(ovr, 0, OVR_GUARD_BYTES);
		*(void**)(KLIB_MEMBER(4, f + 520, ByteArray_m_data, 0)) = ovr + OVR_ENTRY_SIZE;   // entry[-1] = zeroed guard
		*(int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_size, 8))   = 0;
		*(int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_capacityAndFlags, 12))   = OVR_GUARD_CAP | HKARRAY_DONT_DEALLOCATE;
	}

	// The four per-material walkable-slope overrides the real WB carries.
	// NavMeshGenerator__initWorkBuffer (0x3C48D0) pushes them once at NMG
	// construction: a staging entry from OverrideSettings__initFromSettings
	// (0xDD9280), then four copies with the material id at +8 and the slope at
	// +16 varied. Without them every fresh-WB MISS generated materials 1-4 at
	// the base slope from +52 instead of 60/60/90/60, because 0xDD95D0 falls
	// back to +52 when no override matches.
	//
	// The entries are duplicated from the real WB rather than rebuilt from the
	// staging sequence: that reproduces the fields this code does not model
	// (the flag byte, the 56-byte EdgeMatchingParameters block the game patches
	// before pushing, and the SimplificationSettings sub-object) without
	// re-deriving them. Only the material id and the slope are written from
	// NM_MATERIAL_SLOPE_BITS, so the table stays the single authority that the
	// L2 settings hash also reads.
	AppendMaterialOverrides(f, o);

	// No generation setting is written here: these fields keep
	// the real WB's own values, copied above (+76..+143, +320..+335,
	// +336..+423), so wbQ= reads the game's 0.90.
	InterlockedExchange(&g_wbQualityLast, *(long*)(f + 328));

	// Once per session: the prune= stats token, and in DEV the full comparison
	// against the real WB (the "NavMesh freshWB diff:" line). Both read only.
	NoteFreshWbPrune(f);
#ifdef ZONEOPT_DEBUG
	AuditFreshWorkBuffer(f, o);
#endif

	return fresh;
}
} // namespace nm_workers_detail

// Release a WB from ConstructFreshSettings. This runs the game's own settings
// dtor body on it, which is what releases the carvers, the painters,
// +160/+176, +336 and +512. The two DONT_DEALLOCATE buffers are ours to free: the +520 guard
// while +532 still carries the flag with our capacity, and the +288 material-map
// copy while +300 does — SortedArray__grow (0xBA3AA0) replaces the buffer and
// rewrites the capacity if a job ever outgrew it (never observed).
namespace nm_workers_detail {
void FreeFreshSettings(void* wb)
{
	if (!wb) return;
	char* f = (char*)wb;

	// Destroy whatever entries are still in the override array, back to front,
	// exactly as the settings dtor body (0xDD9BC0) does:
	//     v2 = count - 1; v3 = data + 240*v2;
	//     do { OverrideSettings__dtor(v3); v3 -= 240; --v2; } while (v2 >= 0);
	// Each entry owns a SimplificationSettings whose hkStringPtr may have
	// allocated during the copy, so a bare free would leak it. The count is
	// then zeroed, so the dtor body called below finds count 0, takes
	// `count - 1 < 0` and destroys nothing: no double-destroy. Keeping the loop
	// here rather than letting the body do it is deliberate — the body reaches
	// OverrideSettings::dtor at its real address, which hook_edgeProcess
	// patches, so four extra calls per MISS would land in the edge=a/u counters,
	// which should read exactly 2 per MISS.
	{
		char* entries = *(char**)(KLIB_MEMBER(4, f + 520, ByteArray_m_data, 0));
		int count = *(int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_size, 8));
		if (entries && count > 0 && count <= OVR_GUARD_CAP)
		{
			for (int i = count - 1; i >= 0; --i)
				CallOverrideSettingsDtor(entries + (size_t)OVR_ENTRY_SIZE * i);
			*(int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_size, 8)) = 0;
		}
		else if (entries && count > OVR_GUARD_CAP)
		{
			// SortedArray__grow replaced the buffer, so it is no longer ours and
			// the entry count is not one this code put there. Leaving it alone
			// leaks those entries, which is the safe direction, but it should
			// never happen: the capacity is 8 and a job appends one.
			InterlockedIncrement(&g_wbOverrideSkipped);
		}
	}

	// Capture our two DONT_DEALLOCATE buffers BEFORE anything else: the dtor
	// body NULLs both fields (+520/+532 and +288/+300), so reading them
	// afterwards would lose the pointers.
	//   +520 guard: ours while +532 still carries DONT_DEALLOCATE with our
	//     capacity. SortedArray__grow would have replaced the buffer and
	//     rewritten the capacity if the game ever outgrew it.
	//   +288 material map: ours while +300 carries DONT_DEALLOCATE with a
	//     non-zero capacity.
	char* ovr = *(char**)(KLIB_MEMBER(4, f + 520, ByteArray_m_data, 0));
	int capFlags = *(int*)(KLIB_MEMBER(4, f + 520, ByteArray_m_capacityAndFlags, 12));
	bool ovrIsOurs = (ovr != NULL) && (capFlags & HKARRAY_DONT_DEALLOCATE)
	              && ((capFlags & 0x3FFFFFFF) == OVR_GUARD_CAP);

	void* mm = *(void**)(KLIB_MEMBER(4, f + 288, ByteArray_m_data, 0));
	int mmCapFlags = *(int*)(KLIB_MEMBER(4, f + 288, ByteArray_m_capacityAndFlags, 12));
	bool mmIsOurs = (mm != NULL) && (mmCapFlags & HKARRAY_DONT_DEALLOCATE)
	             && ((mmCapFlags & 0x3FFFFFFF) > 0);

	// The dtor BODY, never the deleting dtor 0xDDABE0: this
	// object came from HavokTlsAlloc and is freed below, not through the class
	// allocator.
	//
	// It releases the +240 carvers and +256 painters element by element, frees
	// their buffers, destroys +336 and +512, and runs the +144 sub-struct
	// destructor which frees +160 and +176 — every one of those was orphaned
	// before, because finalizeDeep empties the arrays but keeps the buffers.
	// It skips our two buffers, because it frees +288 and +520 only when their
	// capacity word is non-negative, i.e. DONT_DEALLOCATE clear.
	//
	// Safe on a fresh buffer: the regions ConstructFreshSettings copies from
	// the real one are scalars the dtor never reads, and the sub-structs whose
	// shallow copy would crash are still at ctor defaults. Runs on the
	// thread that ran processJobAlt, after it returned, so the buffers go back
	// to the allocator they came from.
	if (fn_settingsDtorBody)
		fn_settingsDtorBody(wb);

	size_t freedBytes = 0;

	if (ovrIsOurs)
	{
		HavokTlsFree(ovr - OVR_ENTRY_SIZE, OVR_GUARD_BYTES);
		freedBytes += OVR_GUARD_BYTES;
	}

	if (mmIsOurs)
	{
		size_t mmBytes = (size_t)(mmCapFlags & 0x3FFFFFFF) * 8;
		HavokTlsFree(mm, mmBytes);
		freedBytes += mmBytes;
	}

	HavokTlsFree(wb, WB_OBJECT_SIZE);
	freedBytes += WB_OBJECT_SIZE;

#ifdef ZONEOPT_DEBUG
	InterlockedExchangeAdd64(&nmWbFreedBytes, (LONGLONG)freedBytes);
#else
	(void)freedBytes;
#endif
}
} // namespace nm_workers_detail
