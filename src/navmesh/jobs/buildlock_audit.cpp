#include "navmesh/jobs/buildlock_audit.h"

static const unsigned char* At(const void* p, int off) { return (const unsigned char*)p + off; }

static bool ListsUid(const void* mesh, unsigned uid, const BuildLockLayout& L)
{
	const unsigned char* data = *(const unsigned char* const*)At(mesh, L.meshSets);
	int n = *(const int*)At(mesh, L.meshSetCount);
	for (int k = 0; data && k < n; ++k)
		if (*(const unsigned*)(data + k * L.setStride + L.setOtherUid) == uid)
			return true;
	return false;
}

int StreamingPairMismatches(const void* inst, const void* const* list, int count, const BuildLockLayout& L)
{
	const void* jMesh = *(const void* const*)At(inst, L.instMesh);
	if (!jMesh || !list)
		return 0;
	unsigned jUid = *(const unsigned*)At(inst, L.instUid);
	int bad = 0;
	for (int k = 0; k < count; ++k)
	{
		const void* other = list[k];
		if (!other || other == inst)
			continue;
		const void* iMesh = *(const void* const*)At(other, L.instMesh);
		if (!iMesh)
			continue;
		unsigned iUid = *(const unsigned*)At(other, L.instUid);
		if (ListsUid(jMesh, iUid, L) != ListsUid(iMesh, jUid, L))
			++bad;
	}
	return bad;
}
