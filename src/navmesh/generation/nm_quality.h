// nm_quality.h — NavMesh generation settings the mod installs + probes (Layer 3)
// Depends on: nm_cache_core.h (for probe buffer externs)

#ifndef KENSHI_ZONE_OPT_NM_QUALITY_H
#define KENSHI_ZONE_OPT_NM_QUALITY_H

#include "navmesh/cache/nm_cache_core.h"
#include <string.h>   // memcpy, used by NmMaterialSlope below
#include <stddef.h>   // offsetof, for the layout checks below


// =========================================================================
// Generation settings
// =========================================================================
//
// Every navmesh generates with the game's own settings. The real work buffer
// (NMG+256) is left exactly as NavMeshGenerator__setup_void 0x3C48D0 builds it,
// and fresh work buffers (ConstructFreshSettings, nm_fresh_wb.cpp) copy their
// scalar settings from it: nothing overwrites any of them.
//
// This header holds only the values the mod itself installs on a fresh work
// buffer, because L2SettingsHash (nm_disk_cache.cpp) hashes them: editing one
// invalidates every cached mesh generated with the old value.
//
// Offsets are byte offsets into hkaiNavMeshGenerationSettings (NMG+256).
// All values are in Havok units (= Kenshi world units / 10).

// The four per-material walkable slopes NavMeshGenerator__initWorkBuffer
// (0x3C48D0) installs for materials 1-4 in the wb+520 override array, and the
// base slope at wb+52 that 0xDD95D0 falls back to when no override matches.
// ConstructFreshSettings installs the first four on every fresh work buffer,
// and L2SettingsHash hashes all five, so the cache key and the
// generated mesh cannot describe different slopes.
//
// Stored as raw IEEE-754 bit patterns, not degrees: these are hand-entered
// constants in the game, so they are radians and they are not all clean
// conversions. 60 degrees is a true pi/3 but "90 degrees" is the literal 1.57f,
// and the base is 0.6981308 where 40 degrees would be 0.6981317. Computing them
// from degrees would silently produce different meshes, so the bits are copied
// from the decompile of 0x3C48D0 and verified against the real work buffer at
// runtime in DEV builds.
const unsigned int NM_MATERIAL_SLOPE_BITS[5] = {
	0x3F860A92u,  // material 1  1.0471976  (60 deg, = 1065749138)
	0x3F860A92u,  // material 2  1.0471976  (60 deg, = 1065749138)
	0x3FC8F5C3u,  // material 3  1.57       (~90 deg, = 1070134723)
	0x3F860A92u,  // material 4  1.0471976  (60 deg, = 1065749138)
	0x3F32B8C3u   // base wb+52  0.6981308  (~40 deg, = 1060288707)
};
const int NM_MATERIAL_SLOPE_COUNT = 5;
const int NM_MATERIAL_OVERRIDE_COUNT = 4;   // the first four: materials 1-4

inline float NmMaterialSlope(int i)
{
	float f;
	unsigned int bits = NM_MATERIAL_SLOPE_BITS[i];
	memcpy(&f, &bits, sizeof(f));
	return f;
}


// =========================================================================
// Region pruning and extra-vertex parity
// =========================================================================
//
// A fresh work buffer is ctor-built (0xDD99D0) and ConstructFreshSettings
// copies the real one's scalar regions into it. Two blocks were never copied,
// so every mod MISS generated with Havok's defaults instead of Kenshi's:
//
//   RegionPruningSettings, wb+0x90 (reflection: hkaiNavMeshGenerationSettings
//   member table 0x2113FC0, entry 0x21141A0 "regionPruningSettings" at 0x90;
//   nested table 0x17B41A0). NavMeshGenerator__setup_void 0x3C48D0 writes
//   minRegionArea 1e8, minDistanceToSeedPoints 0.4 and
//   borderPreservationTolerance 0 on the real WB (and revertSettings 0x3C2300
//   rewrites the first two after every job); the ctor's wb144 init 0xDD9200
//   leaves 5 / 1 / 0.1. With Kenshi's values realGenerate's pruneRegions keeps
//   only regions near a seed point; with Havok's it keeps every region of 5 HU^2
//   or more, which is where the sealed pockets that trap characters in town
//   gates came from.
//
//   ExtraVertexSettings, wb+0x1A8 = SimplificationSettings (wb+0x150) + 0x58
//   (reflection: SimplificationSettings table 0x17B3560, "extraVertexSettings"
//   at 0x58; nested table 0x2113AA0). setup_void and revertSettings run
//   HavokNavMesh__wb424cleanup 0xDDBE10 on it, which zeroes vertexFraction and
//   areaFraction (the qword at +0x04) and both addVertices bools (the word at
//   +0x18); the ctor's ExtraVertexSettings_init 0xDE0E80 leaves 0.025 /
//   0.000125 / true / true. generateTaskBT 0x3CBE60 copies the WB's block into
//   every non-town job's override entry (line 877).
//
// ConstructFreshSettings always copies both blocks' actual bytes from the real
// WB, so a fresh WB generates exactly as the real one does (job types 2/3/4 and
// the "MISS no-swap" fallback run on the real WB itself), whatever the real WB
// holds, another mod's values included. The tables below are what the game
// writes, and the L2 settings hash folds them. When the real values differ
// from the tables (CheckGenerationSettingsKey at the first dispatch, and
// NmCheckGenerationSettingsKey on every fresh WB), the meshes no longer match
// their cache key: the fresh WBs are counted (pruneBad= / xvBad=) and L2 is
// turned off for the rest of the session (no read, no write; L1 stays on),
// with one PROD line saying so. INI navmeshVanillaPruning=false turns both
// copies off, under the settings hash without the pruning marker.
//
// Field values are raw IEEE-754 bits, as for the slope table above: 1e8 is
// 0x4CBEBC20 (1287568416 in the decompile), 0.4 is 0x3ECCCCCD (1053609165).

const int WB_OFF_REGION_PRUNING     = 0x90;    // 144: RegionPruningSettings
const int WB_OFF_REGION_SEED_POINTS = 0xA0;    // 160: its m_regionSeedPoints hkArray (never copied)
const int WB_OFF_SIMPLIFICATION     = 0x150;   // 336: SimplificationSettings (160 bytes)
const int WB_OFF_EXTRA_VERTEX       = 0x1A8;   // 424: SimplificationSettings::m_extraVertexSettings
const int WB_OFF_USER_VERTICES      = 0x1D0;   // 464: its m_userVertices hkArray (never copied)

// The scalar head of RegionPruningSettings, +0x90..+0x9F. The two hkArrays
// behind it (+0xA0 seed points, +0xB0 region connections) are per-job state
// and must never be copied.
struct NmRegionPruningScalars {
	unsigned int  minRegionAreaBits;                // +0x00 float
	unsigned int  minDistanceToSeedPointsBits;      // +0x04 float
	unsigned int  borderPreservationToleranceBits;  // +0x08 float
	unsigned char preserveVerticalBorderRegions;    // +0x0C bool
	unsigned char pruneBeforeTriangulation;         // +0x0D bool
	unsigned char pad0E[2];                         // +0x0E never written by the game
};
static_assert(sizeof(NmRegionPruningScalars) == 16, "RegionPruningSettings scalar head size");
static_assert(offsetof(NmRegionPruningScalars, minDistanceToSeedPointsBits) == 0x04, "minDistanceToSeedPoints offset");
static_assert(offsetof(NmRegionPruningScalars, borderPreservationToleranceBits) == 0x08, "borderPreservationTolerance offset");
static_assert(offsetof(NmRegionPruningScalars, preserveVerticalBorderRegions) == 0x0C, "preserveVerticalBorderRegions offset");
static_assert(offsetof(NmRegionPruningScalars, pruneBeforeTriangulation) == 0x0D, "pruneBeforeTriangulation offset");
static_assert((size_t)WB_OFF_REGION_PRUNING + sizeof(NmRegionPruningScalars) == (size_t)WB_OFF_REGION_SEED_POINTS,
              "the pruning copy must stop at the seed-point hkArray");

// ExtraVertexSettings without its trailing m_userVertices hkArray, +0x1A8..+0x1CF.
struct NmExtraVertexScalars {
	unsigned char vertexSelectionMethod;            // +0x00 enum (u8)
	unsigned char pad01[3];
	unsigned int  vertexFractionBits;               // +0x04 float
	unsigned int  areaFractionBits;                 // +0x08 float
	unsigned int  minPartitionAreaBits;             // +0x0C float
	int           numSmoothingIterations;           // +0x10
	unsigned int  iterationDampingBits;             // +0x14 float
	unsigned char addVerticesOnBoundaryEdges;       // +0x18 bool
	unsigned char addVerticesOnPartitionBorders;    // +0x19 bool
	unsigned char pad1A[2];
	unsigned int  boundaryEdgeSplitLengthBits;      // +0x1C float
	unsigned int  partitionBordersSplitLengthBits;  // +0x20 float
	unsigned int  userVertexOnBoundaryToleranceBits; // +0x24 float
};
static_assert(sizeof(NmExtraVertexScalars) == 40, "ExtraVertexSettings scalar head size");
static_assert(offsetof(NmExtraVertexScalars, vertexFractionBits) == 0x04, "vertexFraction offset");
static_assert(offsetof(NmExtraVertexScalars, numSmoothingIterations) == 0x10, "numSmoothingIterations offset");
static_assert(offsetof(NmExtraVertexScalars, addVerticesOnBoundaryEdges) == 0x18, "addVerticesOnBoundaryEdges offset");
static_assert(offsetof(NmExtraVertexScalars, boundaryEdgeSplitLengthBits) == 0x1C, "boundaryEdgeSplitLength offset");
static_assert(offsetof(NmExtraVertexScalars, userVertexOnBoundaryToleranceBits) == 0x24, "userVertexOnBoundaryTolerance offset");
static_assert(WB_OFF_EXTRA_VERTEX == WB_OFF_SIMPLIFICATION + 0x58, "ExtraVertexSettings sits at SimplificationSettings+0x58");
static_assert((size_t)WB_OFF_EXTRA_VERTEX + sizeof(NmExtraVertexScalars) == (size_t)WB_OFF_USER_VERTICES,
              "the extra-vertex copy must stop at the userVertices hkArray");

// What setup_void 0x3C48D0 leaves in the real WB's pruning block.
const NmRegionPruningScalars NM_PRUNE_VANILLA = {
	0x4CBEBC20u,   // minRegionArea 1e8: never passes, so area alone keeps nothing
	0x3ECCCCCDu,   // minDistanceToSeedPoints 0.4
	0x00000000u,   // borderPreservationTolerance 0: border preservation off
	0,             // preserveVerticalBorderRegions false (ctor)
	1,             // pruneBeforeTriangulation true (ctor)
	{ 0, 0 }
};

// ExtraVertexSettings_init 0xDE0E80's defaults with wb424cleanup's four zeroes.
const NmExtraVertexScalars NM_EXTRA_VERTEX_VANILLA = {
	0, { 0, 0, 0 },
	0x00000000u,   // vertexFraction 0      (ctor 0.025, cleared by wb424cleanup)
	0x00000000u,   // areaFraction 0        (ctor 0.000125, cleared by wb424cleanup)
	0x447A0000u,   // minPartitionArea 1000 (ctor)
	20,            // numSmoothingIterations (ctor)
	0x3D4CCCCDu,   // iterationDamping 0.05 (ctor)
	0,             // addVerticesOnBoundaryEdges false  (ctor true, cleared)
	0,             // addVerticesOnPartitionBorders false (ctor true, cleared)
	{ 0, 0 },
	0x42480000u,   // boundaryEdgeSplitLength 50 (ctor)
	0x42480000u,   // partitionBordersSplitLength 50 (ctor)
	0x3A83126Fu    // userVertexOnBoundaryTolerance 0.001 (ctor)
};

// True when fresh work buffers get the two blocks: not turned off in the INI.
// Read-only after LoadConfig, any thread.
inline bool NmVanillaPruningActive()
{
	return navmesh::g_navmeshCfg.navmeshVanillaPruningEnabled;
}

// Field-by-field against the tables (padding ignored).
inline bool NmPruneMatchesVanilla(const NmRegionPruningScalars* s)
{
	return s->minRegionAreaBits == NM_PRUNE_VANILLA.minRegionAreaBits
	    && s->minDistanceToSeedPointsBits == NM_PRUNE_VANILLA.minDistanceToSeedPointsBits
	    && s->borderPreservationToleranceBits == NM_PRUNE_VANILLA.borderPreservationToleranceBits
	    && s->preserveVerticalBorderRegions == NM_PRUNE_VANILLA.preserveVerticalBorderRegions
	    && s->pruneBeforeTriangulation == NM_PRUNE_VANILLA.pruneBeforeTriangulation;
}

inline bool NmExtraVertexMatchesVanilla(const NmExtraVertexScalars* s)
{
	const NmExtraVertexScalars& v = NM_EXTRA_VERTEX_VANILLA;
	return s->vertexSelectionMethod == v.vertexSelectionMethod
	    && s->vertexFractionBits == v.vertexFractionBits
	    && s->areaFractionBits == v.areaFractionBits
	    && s->minPartitionAreaBits == v.minPartitionAreaBits
	    && s->numSmoothingIterations == v.numSmoothingIterations
	    && s->iterationDampingBits == v.iterationDampingBits
	    && s->addVerticesOnBoundaryEdges == v.addVerticesOnBoundaryEdges
	    && s->addVerticesOnPartitionBorders == v.addVerticesOnPartitionBorders
	    && s->boundaryEdgeSplitLengthBits == v.boundaryEdgeSplitLengthBits
	    && s->partitionBordersSplitLengthBits == v.partitionBordersSplitLengthBits
	    && s->userVertexOnBoundaryToleranceBits == v.userVertexOnBoundaryToleranceBits;
}

// Compares a work buffer's blocks with the tables the L2 settings hash
// describes: the pruning block and the extra-vertex block, and only while
// NmVanillaPruningActive (otherwise nothing is copied
// and the hash describes the ctor's own values). Returns true when they match;
// *pruneOk / *xvOk say which block differs. On a difference it sets g_l2Bypass
// (nm_cache_core.h) and, the first time, queues the PROD line
//   NavMesh L2 bypassed: generation settings differ from the cache key (prune=... xv=...)
// through LogMsgDeferrable. Any thread: Interlocked, a fixed buffer and
// _snprintf_s only, no CRT strings, no LogMsg off the main thread.
bool NmCheckGenerationSettingsKey(const char* wb, bool* pruneOk, bool* xvOk);


// All functions take explicit nmg (NavMeshGenerator*) — no globals.
void ProbeNavMeshSettings(uintptr_t nmg);
// Reads the real work buffer's edge-matching, simplification and character
// width fields (the game's own values) for the one-time
// "NavMesh generation settings:" line.
void VerifyNavMeshSettings(uintptr_t nmg);
void ProbeWorkBufferSize(uintptr_t nmg);
// The one check of the real work buffer's blocks against the settings hash
// (NmCheckGenerationSettingsKey), at the first dispatch where realNMG+256 is
// readable, before any job is served. Latched like the probes above (0 -> 1
// claimed -> 2 done; re-armed while the work buffer is still NULL).
void CheckGenerationSettingsKey(uintptr_t nmg);

#endif // KENSHI_ZONE_OPT_NM_QUALITY_H
