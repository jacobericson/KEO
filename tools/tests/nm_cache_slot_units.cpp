// The L1 slot predicate, driven through the shipped code: a model of the
// ring's publication (one write index, advanced modulo NM_CACHE_SIZE, over
// whatever the slot held) and the slot states an eviction, an invalid slot
// and a partial entry leave.

#include <cstdio>
#include <cstring>
#include "navmesh/cache/nm_cache_slot_policy.h"

#include "check.h"

static unsigned char s_faces[16];

static NavMeshCacheKey Key(int gx, int gy)
{
	NavMeshCacheKey k;
	memset(&k, 0, sizeof(k));
	k.gridX = gx;
	k.gridY = gy;
	k.sectionTileId = 7;
	k.jobType = 0;
	k.aabbHash = 0x1234u;
	k.buildingHash = 0x5678u;
	return k;
}

static NavMeshCacheEntry Entry(const NavMeshCacheKey& k)
{
	NavMeshCacheEntry e;
	memset(&e, 0, sizeof(e));
	e.key = k;
	e.cachedFaces = s_faces;
	e.faceCount = 1;
	e.valid = true;
	return e;
}

struct Ring
{
	NavMeshCacheEntry e[NM_CACHE_SIZE];
	int writeIdx;
};

static Ring s_ring;

// As the ring publishes: overwrite the slot at the write index, advance it.
static int Publish(Ring* r, const NavMeshCacheKey& k)
{
	int idx = r->writeIdx;
	r->e[idx] = Entry(k);
	r->writeIdx = (idx + 1) % NM_CACHE_SIZE;
	return idx;
}

// A promotion's saved index, then the rest of the ring published before the
// index is used.
static void RingModel()
{
	memset(&s_ring, 0, sizeof(s_ring));
	const NavMeshCacheKey promoted = Key(10, 20);
	int idx = Publish(&s_ring, promoted);
	for (int n = 0; n < NM_CACHE_SIZE - 1; ++n)
		Publish(&s_ring, Key(100 + n, 0));
	Check(CacheSlotMatches(s_ring.e[idx], promoted),
	      "ring: 255 publications after the promotion, the slot still holds its key");
	Publish(&s_ring, Key(999, 0));
	Check(!CacheSlotMatches(s_ring.e[idx], promoted),
	      "ring: the 256th publication replaces the promoted slot, and the predicate sees it");
}

static void SlotStates()
{
	const NavMeshCacheKey k = Key(3, 4);
	Check(CacheSlotMatches(Entry(k), k), "slot: a valid entry with faces and the same key matches");

	NavMeshCacheEntry evicted = Entry(k);
	evicted.valid = false;
	evicted.cachedFaces = NULL;
	Check(!CacheSlotMatches(evicted, k), "slot: an evicted slot that kept its key does not match");

	NavMeshCacheEntry invalid = Entry(k);
	invalid.valid = false;
	Check(!CacheSlotMatches(invalid, k), "slot: an invalid slot with its arrays and key does not match");

	NavMeshCacheEntry noFaces = Entry(k);
	noFaces.cachedFaces = NULL;
	Check(!CacheSlotMatches(noFaces, k), "slot: a valid slot with no face array does not match");

	NavMeshCacheEntry zero = Entry(k);
	zero.faceCount = 0;
	Check(!CacheSlotMatches(zero, k), "slot: a zero-face slot does not match");

	NavMeshCacheEntry negative = Entry(k);
	negative.faceCount = -1;
	Check(!CacheSlotMatches(negative, k), "slot: a negative face count does not match");

	memset(&s_ring, 0, sizeof(s_ring));
	const int at = Publish(&s_ring, k);
	Check(CacheSlotMatchesAt(s_ring.e, at, k), "slot: a saved index whose slot still holds the key matches");
	Check(!CacheSlotMatchesAt(s_ring.e, -1, k), "slot: a negative saved index does not match");
	Check(!CacheSlotMatchesAt(s_ring.e, NM_CACHE_SIZE, k), "slot: a saved index past the ring does not match");
}

static void KeyFields()
{
	const NavMeshCacheKey k = Key(3, 4);
	const NavMeshCacheEntry e = Entry(k);
	NavMeshCacheKey d;
	d = k; d.gridX++;          Check(!CacheSlotMatches(e, d), "key: gridX differs");
	d = k; d.gridY++;          Check(!CacheSlotMatches(e, d), "key: gridY differs");
	d = k; d.sectionTileId++;  Check(!CacheSlotMatches(e, d), "key: sectionTileId differs");
	d = k; d.jobType = 1;      Check(!CacheSlotMatches(e, d), "key: jobType differs");
	d = k; d.aabbHash ^= 1u;   Check(!CacheSlotMatches(e, d), "key: aabbHash differs");
	d = k; d.buildingHash ^= 1u; Check(!CacheSlotMatches(e, d), "key: buildingHash differs");
	Check(KeysMatch(k, k), "key: a key matches itself");
}

int main()
{
	RingModel();
	SlotStates();
	KeyFields();
	return CheckExit("nm_cache_slot_units");
}
