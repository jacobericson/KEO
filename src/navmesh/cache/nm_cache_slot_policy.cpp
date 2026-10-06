#include "navmesh/cache/nm_cache_slot_policy.h"
#include <stddef.h>

bool KeysMatch(const NavMeshCacheKey& a, const NavMeshCacheKey& b)
{
	return a.gridX == b.gridX
	    && a.gridY == b.gridY
	    && a.sectionTileId == b.sectionTileId
	    && a.jobType == b.jobType
	    && a.aabbHash == b.aabbHash
	    && a.buildingHash == b.buildingHash;
}

bool CacheSlotMatches(const NavMeshCacheEntry& slot, const NavMeshCacheKey& key)
{
	return slot.valid
	    && slot.cachedFaces != NULL
	    && slot.faceCount > 0
	    && KeysMatch(slot.key, key);
}

bool CacheSlotMatchesAt(const NavMeshCacheEntry* ring, int idx, const NavMeshCacheKey& key)
{
	return idx >= 0 && idx < NM_CACHE_SIZE && CacheSlotMatches(ring[idx], key);
}

int CacheSlotServed(const NavMeshCacheEntry* ring, int fill, const NavMeshCacheKey& key)
{
	for (int i = 0; i < fill && i < NM_CACHE_SIZE; ++i)
	{
		if (ring[i].valid && KeysMatch(ring[i].key, key) && ring[i].faceCount > 0)
			return i;
	}
	return -1;
}

int CacheSlotsToReplace(const NavMeshCacheEntry* ring, int fill, const NavMeshCacheKey& key,
                        int* out, int cap)
{
	int n = 0;
	for (int i = 0; i < fill && i < NM_CACHE_SIZE && n < cap; ++i)
	{
		if (ring[i].valid && KeysMatch(ring[i].key, key))
			out[n++] = i;
	}
	return n;
}
