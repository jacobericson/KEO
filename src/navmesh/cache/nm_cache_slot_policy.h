#ifndef KEO_NM_CACHE_SLOT_POLICY_H
#define KEO_NM_CACHE_SLOT_POLICY_H

#include "navmesh/cache/nm_cache_key.h"

// Slot identity in the L1 ring. An index taken while nmCacheCS was held names
// the entry only while the lock stays held; once it is released the ring may
// publish over the slot. Pure: no game header.

bool KeysMatch(const NavMeshCacheKey& a, const NavMeshCacheKey& b);

// True when the slot holds a servable mesh for `key`: valid, faces present
// (the array and a positive count) and the same key.
bool CacheSlotMatches(const NavMeshCacheEntry& slot, const NavMeshCacheKey& key);

// The same for a saved index into a ring of NM_CACHE_SIZE entries: an index
// outside the ring matches nothing, and the ring is not read.
bool CacheSlotMatchesAt(const NavMeshCacheEntry* ring, int idx, const NavMeshCacheKey& key);

// The slot a lookup serves for key in a ring holding fill entries: the lowest
// index that is valid, has the key and a positive face count. -1 when none.
int CacheSlotServed(const NavMeshCacheEntry* ring, int fill, const NavMeshCacheKey& key);

// The slots a forced store clears before it publishes: every valid slot with
// key, lowest first, zero-face ones included, at most cap into out. Returns
// how many.
int CacheSlotsToReplace(const NavMeshCacheEntry* ring, int fill, const NavMeshCacheKey& key,
                        int* out, int cap);

#endif
