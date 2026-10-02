// nm_disk_cache.h — L2 persistent disk cache for navmesh results (Layer 3)
// Depends on: nm_cache_core.h (for types + shared state)

#ifndef KEO_NM_DISK_CACHE_H
#define KEO_NM_DISK_CACHE_H

#include "navmesh/cache/nm_cache_core.h"

// On-disk format version. Bump on ANY change to the header layout, the payload
// layout, or a generation input the settings hash cannot see. Version 3 puts
// the four material overrides on every fresh work buffer, so meshes generated
// on the worker path carry the real work buffer's slopes (60/60/90/60) rather
// than the default 40-degree slope.
const int L2_CACHE_VERSION = 3;

// File magic, 'K' 'Z' 'O' '2' in file order (little-endian value).
const unsigned int L2_CACHE_MAGIC = 0x324F5A4Bu;

// Serialized L2 payload, built from the L1 deep copy while the cache lock is
// held and written to disk afterwards (the game sees the mesh first).
struct L2WriteBlob {
	NavMeshCacheKey key;
	void*           data;   // one allocation: header followed by payload
	size_t          size;
};

// Builds "<cache dir>\<grid>_<grid>_<tile>_<type>_<aabb>_<bldg>_<modset>.bin".
// No CRT string objects: safe on the NavMesh bg thread and the workers.
void GetDiskCachePath(const NavMeshCacheKey& key, char* out, size_t outSize);

// FNV-1a over the generation settings the mod applies plus the four material
// override slopes. Part of every header; a mismatch rejects the file.
unsigned int L2SettingsHash();

// CRC-32 (reflected, polynomial 0xEDB88320). The checksummed region runs from
// the header field after the CRC slot to the end of the payload, and on the
// read side those live in two buffers, so the running form is the primary one.
inline unsigned int L2Crc32Begin() { return 0xFFFFFFFFu; }
unsigned int        L2Crc32Update(unsigned int crc, const void* data, size_t len);
inline unsigned int L2Crc32End(unsigned int crc) { return ~crc; }
unsigned int        L2Crc32(const void* data, size_t len);

// Snapshot `e` (an L1 entry) into `out`. Returns false and leaves out->data
// NULL if the entry is inconsistent, has zero faces (counted as l2ZeroSkip=),
// or the allocation fails, and always while L2 is bypassed
// for the session (L2Bypassed, nm_cache_core.h). The only producer of L2 bytes.
// The entry, payload-range and allocation refusals are counted as write
// failures (l2WrFail=), because each one loses a file the same way.
bool BuildDiskCacheBlob(const NavMeshCacheKey& key, const NavMeshCacheEntry& e, L2WriteBlob* out);

// Writes the blob atomically (temp file + MoveFileEx) and frees it. Safe to
// call with a blob whose data is NULL. Returns true when the file landed.
// Every way it can fail is counted by cause (l2WrFail=), and the first failure
// of the session names itself in the log.
bool WriteDiskCacheBlob(L2WriteBlob* blob);

// Frees a blob without writing it.
void FreeDiskCacheBlob(L2WriteBlob* blob);

// Main-thread startup sweep: deletes stale ".tmp" leftovers and ".bin" files
// whose name is not the current key format, then measures the directory and
// trims it to the cap. Leaves the total tracked, so the first write of the
// session does not rescan. Called from InitNavMeshCacheCS.
void L2StartupScan();

// Strict reader. On success `out` owns freshly allocated arrays and out.valid
// is true; on any failure nothing is allocated and out.valid is false. While
// L2 is bypassed for the session every call is a plain miss (no file opened).
bool ReadDiskCache(const NavMeshCacheKey& key, NavMeshCacheEntry& out);

#endif // KEO_NM_DISK_CACHE_H
