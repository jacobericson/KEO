// npc_fail_memo_policy.cpp - see npc_fail_memo_policy.h.
#include "pathfind/npc_fail_memo_policy.h"
#include <string.h>

static const unsigned char* PtrAt(const void* base, size_t off)
{
	return *(const unsigned char* const*)((const unsigned char*)base + off);
}

static int IntAt(const void* base, size_t off)
{
	return *(const int*)((const unsigned char*)base + off);
}

static unsigned Mix(unsigned h, unsigned long long v)
{
	for (int b = 0; b < 8; ++b)
	{
		h ^= (unsigned)((v >> (8 * b)) & 0xFFu);
		h *= 16777619u;
	}
	return h;
}

bool NfmCovers(int mode, AstarCallerClass cls, bool playerTag, bool waved)
{
	return mode != NFM_OFF && cls == ASTAR_CALLER_CHARACTER_NPC && !playerTag && !waved;
}

unsigned NfmClusterKey(unsigned faceKey, int cluster)
{
	if (faceKey == NFM_NO_KEY || cluster < 0)
		return NFM_NO_KEY;
	return ((unsigned)cluster & 0x3FFFFFu) | (faceKey & 0xFFC00000u);
}

NfmKey NfmMakeKey(unsigned goalFace, unsigned startFace, int startCluster, unsigned diameterBits, bool costModifier)
{
	NfmKey k;
	k.goal = goalFace;
	k.diameter = diameterBits;
	k.flags = costModifier ? NFM_COST_MODIFIER : 0u;
	unsigned cluster = NfmClusterKey(startFace, startCluster);
	if (cluster == NFM_NO_KEY)
	{
		k.start = startFace;
		k.flags |= NFM_START_FACE;
	}
	else
	{
		k.start = cluster;
	}
	return k;
}

bool NfmKeyEqual(const NfmKey& a, const NfmKey& b)
{
	return a.goal == b.goal && a.start == b.start && a.diameter == b.diameter && a.flags == b.flags;
}

unsigned NfmHash(const NfmKey& k)
{
	unsigned h = 2166136261u;
	h = Mix(h, k.goal);
	h = Mix(h, k.start);
	h = Mix(h, k.diameter);
	return Mix(h, k.flags);
}

NfmDrop NfmStale(const NfmEntry& e, const NfmEpoch& now, long long nowTicks, long long ttlTicks)
{
	if (e.epoch.reset != now.reset)
		return NFM_DROP_RESET;
	if (e.epoch.sections != now.sections)
		return NFM_DROP_SECTIONS;
	if (e.epoch.door != now.door)
		return NFM_DROP_DOOR;
	if (nowTicks < e.insertTicks || nowTicks - e.insertTicks >= ttlTicks)
		return NFM_DROP_TTL;
	return NFM_LIVE;
}

static int Slot(unsigned home, int probe)
{
	return (int)((home + (unsigned)probe) & (unsigned)(NFM_TABLE_SIZE - 1));
}

NfmLookup NfmFind(NfmTable* t, const NfmKey& k, unsigned startFace, const NfmEpoch& now,
                  long long nowTicks, long long ttlTicks)
{
	NfmLookup r;
	r.index = -1;
	r.exactFace = 0;
	memset(r.dropped, 0, sizeof(r.dropped));
	unsigned home = NfmHash(k);
	for (int p = 0; p < NFM_PROBE; ++p)
	{
		int i = Slot(home, p);
		NfmEntry& e = t->e[i];
		if (!e.used)
			continue;
		NfmDrop d = NfmStale(e, now, nowTicks, ttlTicks);
		if (d != NFM_LIVE)
		{
			e.used = 0;
			--t->entries;
			++r.dropped[d];
			continue;
		}
		if (r.index < 0 && NfmKeyEqual(e.key, k))
		{
			r.index = i;
			r.exactFace = e.startFace == startFace ? 1 : 0;
		}
	}
	return r;
}

bool NfmInsert(NfmTable* t, const NfmKey& k, unsigned startFace, const NfmEpoch& epoch,
               long long nowTicks, long long serviceTicks)
{
	unsigned home = NfmHash(k);
	int target = -1, freeSlot = -1, oldest = -1;
	for (int p = 0; p < NFM_PROBE; ++p)
	{
		int i = Slot(home, p);
		const NfmEntry& e = t->e[i];
		if (e.used && NfmKeyEqual(e.key, k))
		{
			target = i;
			break;
		}
		if (!e.used)
		{
			if (freeSlot < 0)
				freeSlot = i;
		}
		else if (oldest < 0 || e.insertTicks < t->e[oldest].insertTicks)
		{
			oldest = i;
		}
	}
	bool tookFree = false;
	if (target < 0)
	{
		tookFree = freeSlot >= 0;
		target = tookFree ? freeSlot : oldest;
	}
	NfmEntry& e = t->e[target];
	e.key = k;
	e.startFace = startFace;
	e.epoch = epoch;
	e.insertTicks = nowTicks;
	e.serviceTicks = serviceTicks;
	if (!e.used)
	{
		e.used = 1;
		++t->entries;
	}
	return tookFree;
}

bool NfmErase(NfmTable* t, const NfmKey& k)
{
	unsigned home = NfmHash(k);
	for (int p = 0; p < NFM_PROBE; ++p)
	{
		NfmEntry& e = t->e[Slot(home, p)];
		if (e.used && NfmKeyEqual(e.key, k))
		{
			e.used = 0;
			--t->entries;
			return true;
		}
	}
	return false;
}

void NfmClear(NfmTable* t)
{
	memset(t, 0, sizeof(*t));
}

bool NfmShouldInsert(int status, int cause, int iterations)
{
	return status == 3 && cause == 3 && iterations >= NFM_MIN_INSERT_ITER;
}

bool NfmWrong(int status)
{
	return status == 1;
}

long long NfmTtlTicks(long long ticksPerSecond, bool doorSeen)
{
	return ticksPerSecond * (doorSeen ? NFM_TTL_SECONDS : NFM_TTL_NO_DOOR_SECONDS);
}

// The section's instance, or NULL when the key names no populated slot.
static const unsigned char* InstanceOf(const void* collection, unsigned faceKey)
{
	if (!collection || faceKey == NFM_NO_KEY)
		return NULL;
	int count = IntAt(collection, NFM_COLL_COUNT);
	const unsigned char* infos = PtrAt(collection, NFM_COLL_INSTANCES);
	unsigned section = faceKey >> 22;
	if (!infos || count <= 0 || count > NFM_MAX_SECTIONS || (int)section >= count)
		return NULL;
	return PtrAt(infos + NFM_INFO_STRIDE * section, 0);
}

bool NfmReadFace(const void* collection, unsigned faceKey, int* cluster, int* data)
{
	*cluster = -1;
	*data = -1;
	const unsigned char* inst = InstanceOf(collection, faceKey);
	if (!inst)
		return false;
	int index = (int)(faceKey & 0x3FFFFFu);
	int numOriginal = IntAt(inst, NFM_INST_NUM_ORIG);
	size_t facesAt, dataAt;
	int row;
	if (index < numOriginal)
	{
		int mapped = index;
		int mapSize = IntAt(inst, NFM_INST_FACE_MAP + 8);
		if (mapSize != 0)
		{
			const unsigned char* map = PtrAt(inst, NFM_INST_FACE_MAP);
			if (!map || index >= mapSize)
				return false;
			mapped = ((const int*)map)[index];
		}
		if (mapped == -1)
		{
			facesAt = NFM_INST_ORIG_FACES;
			dataAt = NFM_INST_ORIG_DATA;
			row = index;
		}
		else
		{
			if (mapped < 0 || mapped >= IntAt(inst, NFM_INST_INST_FACES + 8))
				return false;
			facesAt = NFM_INST_INST_FACES;
			dataAt = NFM_INST_INST_DATA;
			row = mapped;
		}
	}
	else
	{
		row = index - numOriginal;
		if (row >= IntAt(inst, NFM_INST_OWNED_FACES + 8))
			return false;
		facesAt = NFM_INST_OWNED_FACES;
		dataAt = NFM_INST_OWNED_DATA;
	}
	const unsigned char* faces = PtrAt(inst, facesAt);
	if (!faces)
		return false;
	*cluster = *(const short*)(faces + NFM_FACE_BYTES * (size_t)row + NFM_FACE_CLUSTER);
	int stride = IntAt(inst, NFM_INST_DATA_STRIDE);
	const int* faceData = (const int*)PtrAt(inst, dataAt);
	if (stride > 0 && faceData)
		*data = faceData[(size_t)row * (size_t)stride];
	return true;
}

unsigned NfmSectionsFingerprint(const void* collection, bool* ok)
{
	*ok = false;
	if (!collection)
		return 0;
	int count = IntAt(collection, NFM_COLL_COUNT);
	const unsigned char* infos = PtrAt(collection, NFM_COLL_INSTANCES);
	if (count < 0 || count > NFM_MAX_SECTIONS || (count > 0 && !infos))
		return 0;
	unsigned h = Mix(2166136261u, (unsigned long long)count);
	for (int i = 0; i < count; ++i)
	{
		const unsigned char* inst = PtrAt(infos + NFM_INFO_STRIDE * (size_t)i, 0);
		h = Mix(h, (unsigned long long)(size_t)inst);
		if (!inst)
			continue;
		h = Mix(h, (unsigned long long)(size_t)PtrAt(inst, NFM_INST_ORIG_FACES));
		h = Mix(h, (unsigned)IntAt(inst, NFM_INST_NUM_ORIG));
		h = Mix(h, (unsigned)IntAt(inst, NFM_INST_RUNTIME_ID));
	}
	*ok = true;
	return h;
}
