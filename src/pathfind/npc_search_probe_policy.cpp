#include "pathfind/npc_search_probe_policy.h"

static const unsigned char* PtrAt(const void* base, size_t off)
{
	return *(const unsigned char* const*)((const unsigned char*)base + off);
}

static int IntAt(const void* base, size_t off)
{
	return *(const int*)((const unsigned char*)base + off);
}

unsigned NpcProbeClusterKey(unsigned faceKey, int cluster)
{
	if (faceKey == NSP_NO_KEY || cluster < 0)
		return NSP_NO_KEY;
	return ((unsigned)cluster & 0x3FFFFFu) | (faceKey & 0xFFC00000u);
}

static const unsigned char* InstanceOf(const void* collection, unsigned faceKey)
{
	if (!collection || faceKey == NSP_NO_KEY)
		return NULL;
	int count = IntAt(collection, NSP_COLL_COUNT);
	const unsigned char* infos = PtrAt(collection, NSP_COLL_INSTANCES);
	unsigned section = faceKey >> 22;
	if (!infos || count <= 0 || count > NSP_MAX_SECTIONS || (int)section >= count)
		return NULL;
	return PtrAt(infos + NSP_INFO_STRIDE * section, 0);
}

bool NpcProbeReadFace(const void* collection, unsigned faceKey, int* cluster, int* data)
{
	*cluster = -1;
	*data = -1;
	const unsigned char* inst = InstanceOf(collection, faceKey);
	if (!inst)
		return false;
	int index = (int)(faceKey & 0x3FFFFFu);
	int numOriginal = IntAt(inst, NSP_INST_NUM_ORIG);
	size_t facesAt, dataAt;
	int row;
	if (index < numOriginal)
	{
		int mapped = index;
		int mapSize = IntAt(inst, NSP_INST_FACE_MAP + 8);
		if (mapSize != 0)
		{
			const unsigned char* map = PtrAt(inst, NSP_INST_FACE_MAP);
			if (!map || index >= mapSize)
				return false;
			mapped = ((const int*)map)[index];
		}
		if (mapped == -1)
		{
			facesAt = NSP_INST_ORIG_FACES;
			dataAt = NSP_INST_ORIG_DATA;
			row = index;
		}
		else
		{
			if (mapped < 0 || mapped >= IntAt(inst, NSP_INST_INST_FACES + 8))
				return false;
			facesAt = NSP_INST_INST_FACES;
			dataAt = NSP_INST_INST_DATA;
			row = mapped;
		}
	}
	else
	{
		row = index - numOriginal;
		if (row >= IntAt(inst, NSP_INST_OWNED_FACES + 8))
			return false;
		facesAt = NSP_INST_OWNED_FACES;
		dataAt = NSP_INST_OWNED_DATA;
	}
	const unsigned char* faces = PtrAt(inst, facesAt);
	if (!faces)
		return false;
	*cluster = *(const short*)(faces + NSP_FACE_BYTES * (size_t)row + NSP_FACE_CLUSTER);
	int stride = IntAt(inst, NSP_INST_DATA_STRIDE);
	const int* faceData = (const int*)PtrAt(inst, dataAt);
	if (stride > 0 && faceData)
		*data = faceData[(size_t)row * (size_t)stride];
	return true;
}
