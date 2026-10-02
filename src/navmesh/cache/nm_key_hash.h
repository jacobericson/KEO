#ifndef KEO_NM_KEY_HASH_H
#define KEO_NM_KEY_HASH_H

#include <stddef.h>
#include "base/hash.h"

// The navmesh cache's key hashes over the bytes the caller points at: the L2
// header's settings hash, a zone's AABB hash (the L2 file name) and one
// building's hash (summed into the zone's). No lock, no call out, no global,
// so a host suite runs the code the cache runs; a caller that points into
// game memory calls these inside its own fault guard. The values name every
// cached mesh, so they never change.

inline unsigned int L2SettingsHashOf(const void* slopeBits, size_t slopeLen,
                                     const void* prune, size_t pruneLen,
                                     const void* xvert, size_t xvertLen,
                                     bool pruning, bool nbrSeed)
{
	// The game's own generation settings (nm_quality.h): no mod value reaches a
	// work buffer apart from the five slope constants the mod installs on fresh
	// work buffers. FNV-1a over those, then over the fixed marker
	// "vanilla". Byte for byte the formula a deleted quality-tuning build's untuned
	// arm shipped (ZONEOPT_NMQUALITY_OFF, settings hash 0xD3FE8BD2), so the meshes
	// that arm wrote stay valid under it.
	unsigned int h = FNV1A32_OFFSET;
	h = Fnv1a32Bytes(h, slopeBits, slopeLen);
	h = Fnv1a32Bytes(h, "vanilla", sizeof("vanilla") - 1);   // no terminator

	// With vanilla pruning active (nm_quality.h), fresh work buffers
	// carry the game's region-pruning block and extra-vertex block instead of
	// Havok's defaults, so the meshes differ and the key must too. Each block
	// folds a versioned marker and the table the copied
	// block is expected to match (pads are zero in the tables, so hashing whole
	// structs is deterministic). If the real WB holds anything else, L2 is off
	// for that session (L2Bypassed). Pruning off (navmeshVanillaPruning=false)
	// leaves the hash above untouched: 0xD3FE8BD2, the same generation as before.
	if (pruning)
	{
		h = Fnv1a32Bytes(h, "prune1", sizeof("prune1") - 1);
		h = Fnv1a32Bytes(h, prune, pruneLen);
		h = Fnv1a32Bytes(h, "xvert1", sizeof("xvert1") - 1);
		h = Fnv1a32Bytes(h, xvert, xvertLen);
	}

	// With the neighbour-seed stand-in active (nm_nbr_seeds.cpp), a missing or
	// temp neighbour contributes its shipped tile's border seeds, so pruning
	// keeps seam regions it would otherwise drop and the meshes differ. The L2
	// header records no neighbour state, so the affected files cannot be picked
	// out: every file written before is refused once (reason h). Folded last, so
	// the pruning values above are unchanged with the stand-in off
	// (navmeshNeighbourSeeds=false, or the hook / a callee refused). With it on:
	// 0x8F42260B over the pruning hash 0x5F777F1D.
	if (nbrSeed)
		h = Fnv1a32Bytes(h, "nbrseed1", sizeof("nbrseed1") - 1);   // no terminator
	return h;
}

inline unsigned int NmAabbHash(const float* aabb6)
{
	return Fnv1a32Bytes(FNV1A32_OFFSET, aabb6, 24);
}

// One building: its position (12 bytes), its rotation quaternion (16), then
// its stringID's bytes when sid is not NULL.
inline unsigned int NmBuildingHashOf(const void* pos12, const void* rot16,
                                     const void* sid, size_t sidLen)
{
	unsigned int h = Fnv1a32Bytes(FNV1A32_OFFSET, pos12, 12);
	h = Fnv1a32Bytes(h, rot16, 16);
	if (sid)
		h = Fnv1a32Bytes(h, sid, sidLen);
	return h;
}

#endif
