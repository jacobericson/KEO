#include "pathfind/npc_search_probe_policy.h"
#include <string.h>
#include "check.h"

// Two section slots: slot 0 holds an instance with two original faces (face 1 mapped to the
// instanced copy 0) and one owned face; slot 1 is empty.
struct __declspec(align(16)) FakeWorld
{
	unsigned char coll[0x38];
	unsigned char infos[2 * NSP_INFO_STRIDE];
	unsigned char inst[0x1C0];
	unsigned char origFaces[2 * NSP_FACE_BYTES];
	unsigned char instFaces[1 * NSP_FACE_BYTES];
	unsigned char ownedFaces[1 * NSP_FACE_BYTES];
	int faceMap[2];
	int origData[2];
	int instData[1];
	int ownedData[1];
};

static void PutPtr(unsigned char* base, size_t off, const void* p) { memcpy(base + off, &p, sizeof(p)); }
static void PutInt(unsigned char* base, size_t off, int v) { memcpy(base + off, &v, sizeof(v)); }
static void PutCluster(unsigned char* face, short c) { memcpy(face + NSP_FACE_CLUSTER, &c, sizeof(c)); }

static void Build(FakeWorld* w)
{
	memset(w, 0, sizeof(*w));
	PutPtr(w->coll, NSP_COLL_INSTANCES, w->infos);
	PutInt(w->coll, NSP_COLL_COUNT, 2);
	PutPtr(w->infos, 0, w->inst);
	PutPtr(w->inst, NSP_INST_ORIG_FACES, w->origFaces);
	PutInt(w->inst, NSP_INST_NUM_ORIG, 2);
	PutPtr(w->inst, NSP_INST_ORIG_DATA, w->origData);
	PutInt(w->inst, NSP_INST_DATA_STRIDE, 1);
	w->faceMap[0] = -1;
	w->faceMap[1] = 0;
	PutPtr(w->inst, NSP_INST_FACE_MAP, w->faceMap);
	PutInt(w->inst, NSP_INST_FACE_MAP + 8, 2);
	PutPtr(w->inst, NSP_INST_INST_FACES, w->instFaces);
	PutInt(w->inst, NSP_INST_INST_FACES + 8, 1);
	PutPtr(w->inst, NSP_INST_OWNED_FACES, w->ownedFaces);
	PutInt(w->inst, NSP_INST_OWNED_FACES + 8, 1);
	PutPtr(w->inst, NSP_INST_INST_DATA, w->instData);
	PutPtr(w->inst, NSP_INST_OWNED_DATA, w->ownedData);
	PutCluster(w->origFaces, 11);
	PutCluster(w->origFaces + NSP_FACE_BYTES, 99);
	PutCluster(w->instFaces, 12);
	PutCluster(w->ownedFaces, -1);
	w->origData[0] = 4;
	w->origData[1] = 9;
	w->instData[0] = 5;
	w->ownedData[0] = 3;
}

static void CheckFaces()
{
	FakeWorld w;
	Build(&w);
	int cluster = 0, data = 0;
	Check(NpcProbeReadFace(w.coll, 0u, &cluster, &data) && cluster == 11 && data == 4, "faces: an original face reads its cluster and data word");
	Check(NpcProbeReadFace(w.coll, 1u, &cluster, &data) && cluster == 12 && data == 5, "faces: a mapped face reads the instanced copy");
	Check(NpcProbeReadFace(w.coll, 2u, &cluster, &data) && cluster == -1 && data == 3, "faces: an owned face reads past the original count");
	Check(!NpcProbeReadFace(w.coll, 3u, &cluster, &data) && cluster == -1 && data == -1, "faces: an index past the owned faces is refused");
	Check(!NpcProbeReadFace(w.coll, 1u << 22, &cluster, &data), "faces: an empty section slot is refused");
	Check(!NpcProbeReadFace(w.coll, 5u << 22, &cluster, &data), "faces: a section past the collection is refused");
	Check(!NpcProbeReadFace(w.coll, NSP_NO_KEY, &cluster, &data), "faces: no key reads nothing");
	PutInt(w.inst, NSP_INST_DATA_STRIDE, 0);
	Check(NpcProbeReadFace(w.coll, 0u, &cluster, &data) && cluster == 11 && data == -1, "faces: no face data reads -1");
	Build(&w);
	PutInt(w.inst, NSP_INST_FACE_MAP + 8, 0);
	Check(NpcProbeReadFace(w.coll, 0u, &cluster, &data) && cluster == 12 && data == 5,
	      "faces: without a face map the face index indexes the instanced faces, as the engine does");
}

int main()
{
	Check(NpcProbeClusterKey((5u << 22) | 123u, 37) == ((5u << 22) | 37u), "cluster: preserves the section");
	Check(NpcProbeClusterKey(1u, -1) == NSP_NO_KEY, "cluster: missing cluster has no key");
	CheckFaces();
	return CheckExit("npc_search_probe_units");
}
