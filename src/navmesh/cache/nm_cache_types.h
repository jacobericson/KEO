// nm_cache_types.h — Shared types and constants for navmesh cache modules
// Depends on: config.h

#ifndef KEO_NM_CACHE_TYPES_H
#define KEO_NM_CACHE_TYPES_H

#include "base/config.h"
#include "game/klib_members.h"


// hkaiNavMesh array element sizes (from Havok SDK hkaiNavMesh.h)
const int HKAI_FACE_SIZE     = 16;
const int HKAI_EDGE_SIZE     = 20;
const int HKAI_VERTEX_SIZE   = 16;
const int HKAI_FACEDATA_UNIT = 4;
const int HKAI_EDGEDATA_UNIT = 4;

// hkArray offsets within hkaiNavMesh (176 bytes)
const int NMOFF_FACES    = 16;
const int NMOFF_EDGES    = 32;
const int NMOFF_VERTICES = 48;
const int NMOFF_FACEDATA = 80;
const int NMOFF_EDGEDATA = 96;

// hkArray field accessors
inline void* hkArrayGetPtr(uintptr_t nm, int off)   { return *(void**)(KLIB_MEMBER(1, nm + off, ByteArray_m_data, 0)); }
inline int   hkArrayGetCount(uintptr_t nm, int off)  { return *(int*)(KLIB_MEMBER(1, nm + off, ByteArray_m_size, 8)); }
inline void  hkArraySet(uintptr_t nm, int off, void* ptr, int count, int capFlags)
{
	*(void**)(KLIB_MEMBER(1, nm + off, ByteArray_m_data, 0))     = ptr;
	*(int*)(KLIB_MEMBER(1, nm + off, ByteArray_m_size, 8))   = count;
	*(int*)(KLIB_MEMBER(1, nm + off, ByteArray_m_capacityAndFlags, 12))  = capFlags;
}


#include "navmesh/cache/nm_cache_key.h"


const int NMG_STRUCT_SIZE = 352;
static_assert(NMG_STRUCT_SIZE == KLIB_SIZE_NavMeshGenerator, "clone memcpy and allocation width");
const int WB_OBJECT_SIZE = 0x220;  // hkaiNavMeshGenerationSettings: 544 bytes (confirmed from settings dtor 0xDDABE0: default free size = 544)

// hkArray flag: data is not owned by this array, do not free/realloc (hkArray.h)
const int HKARRAY_DONT_DEALLOCATE = (int)0x80000000;

const int WB_MAX_ARRAYS = 64;
const int WB_MAX_SDK_ARRAYS = 8;
const int L2_MISS_LOG_MAX = 16;

struct WBArrayProbe { int offset; uintptr_t ptr; int count; int capFlags; };

struct L2MissEntry {
	int gridX, gridY, tileId, jobType;
	unsigned int aabbHash, buildingHash;
	int thingsCount;
};


// L2 disk cache rejection reasons. A rejection means the file existed but was
// not trustworthy; a plain "no such file" is a miss, not a rejection.
// Reported as l2Rej=<total>(m/v/h/l/c/b/i/a/p) on the NM cache stats line.
enum L2RejectReason {
	L2REJ_MAGIC = 0,    // m: not one of our files
	L2REJ_VERSION,      // v: written by a different format version
	L2REJ_SETTINGS,     // h: generation settings hash differs
	L2REJ_LENGTH,       // l: payload length disagrees with the counts or the file size
	L2REJ_CRC,          // c: checksum mismatch over the header tail and the payload
	L2REJ_BOUNDS,       // b: a count or striding outside sane maxima
	L2REJ_IO,           // i: short read
	L2REJ_ALLOC,        // a: allocation failed, entry discarded whole
	L2REJ_PATHLONG,     // p: a path did not fit its buffer; that file is skipped
	L2REJ_REASON_COUNT
};

// Sane upper bounds for the payload counts. Faces/edges/vertices keep the
// limits StoreCacheEntry has always applied; the data arrays and the stridings
// are bounded here too.
const int L2_MAX_FACES    = 50000;
const int L2_MAX_EDGES    = 200000;
const int L2_MAX_VERTICES = 100000;
const int L2_MAX_FACEDATA = 1000000;
const int L2_MAX_EDGEDATA = 2000000;
const int L2_MAX_STRIDING = 64;


#endif // KEO_NM_CACHE_TYPES_H
