#include <cstdio>
#include <cstring>
#include "navmesh/jobs/buildlock_audit.h"

#include "check.h"

static const BuildLockLayout L = { 0x08, 0x3C, 0x40, 0x48, 0x38, 0x04 };

struct FakeMesh { unsigned char bytes[0x50]; unsigned char sets[4][0x38]; };
struct FakeInst { unsigned char bytes[0x48]; };

static void Setup(FakeInst* inst, FakeMesh* mesh, unsigned uid)
{
	memset(inst, 0, sizeof(*inst)); memset(mesh, 0, sizeof(*mesh));
	*(void**)(inst->bytes + L.instMesh) = mesh;
	*(unsigned*)(inst->bytes + L.instUid) = uid;
	*(void**)(mesh->bytes + L.meshSets) = mesh->sets;
}
static void AddSet(FakeMesh* m, unsigned own, unsigned other)
{
	int n = *(int*)(m->bytes + L.meshSetCount);
	*(unsigned*)(m->sets[n] + 0) = own;
	*(unsigned*)(m->sets[n] + L.setOtherUid) = other;
	*(int*)(m->bytes + L.meshSetCount) = n + 1;
}

int main()
{
	FakeInst j, i; FakeMesh jm, im;
	Setup(&j, &jm, 7); Setup(&i, &im, 9);
	const void* list[3] = { &i, NULL, &j };
	Check(StreamingPairMismatches(&j, list, 3, L) == 0, "no sets on either side");
	AddSet(&jm, 7, 9);
	Check(StreamingPairMismatches(&j, list, 3, L) == 1, "one-sided pair");
	AddSet(&im, 9, 7);
	Check(StreamingPairMismatches(&j, list, 3, L) == 0, "symmetric pair");
	*(void**)(i.bytes + L.instMesh) = NULL;
	Check(StreamingPairMismatches(&j, list, 3, L) == 0, "NULL mesh skipped");
	return CheckExit("buildlock_units");
}
