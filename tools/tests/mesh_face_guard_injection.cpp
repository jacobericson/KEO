// Drives the shipped decision with the face record the crash dump carried, and
// shows three things in one run:
//
//   1. the engine's own read sequence through that record faults, at an
//      address computed the way the recorded instruction computes it;
//   2. the guard's decision over the identical memory does not fault and
//      classifies the record as an out-of-range edge run;
//   3. the bounds it substitutes are the bounds the step's own loop produces
//      for a face carrying no edges, for an arbitrary seed and expansion.
//
// Links src/fixes/streaming/mesh_face_guard_policy.cpp unmodified. Kept out of
// build_tests.bat because it raises an access violation on purpose.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include "fixes/streaming/mesh_face_guard_policy.h"

#include "check.h"

// The instance the dump carried: 23 faces and 113 edges borrowed from its
// mesh, 276 edges of its own, and face 0's first four bytes replaced by the
// low half of a free-list pointer.
static const int kNumOriginalFaces = 23;
static const int kNumOriginalEdges = 113;
static const int kOwnedEdges       = 276;
static const int kWildStartEdge    = 1445915680;   // 0x562EEC20
static const int kNumEdges         = 3;
static const int kEdgeStride       = 20;
static const int kNumOriginalVerts = 70;
static const int kOwnedVerts       = 116;
static const int kWildVertex       = 1445915680;   // the same free-list value, read as a vertex index

struct FaceRecord
{
	int   startEdge;
	int   oppositeKey;
	short numEdges;
	short pad0;
	int   pad1;
};

// The address the recorded instruction dereferences: the owned-edge array
// indexed by the wild start, which is what the accessor computes whenever the
// index is at or past the borrowed half.
static const unsigned char* OwnedEdgeAddress(const unsigned char* ownedEdges, int index)
{
	return ownedEdges + (__int64)kEdgeStride * ((__int64)index - kNumOriginalEdges);
}

// The engine's own two steps after the face fetch, written out so the fault is
// this harness's own and its address can be reported.
static bool VanillaReadFaults(const FaceRecord* face, const unsigned char* ownedEdges,
                              unsigned __int64* accessOut)
{
	*accessOut = 0;
	__try
	{
		const unsigned char* edge = OwnedEdgeAddress(ownedEdges, face->startEdge);
		*accessOut = (unsigned __int64)edge;
		volatile int vertex = *(const int*)edge;
		if (vertex == 1) printf("");   // keep the load
		return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return true;
	}
}

// The step's accumulate loop, written from its shape rather than from the
// guard's substitution, so the two are compared rather than assumed equal.
static void ModelStep(const float* seed, const float* expand, int numEdges,
                      const float* edgePoints, float* out)
{
	float lo[4], hi[4];
	for (int i = 0; i < 4; ++i) { lo[i] = seed[i]; hi[i] = -seed[i]; }
	for (int e = 0; e < numEdges; ++e)
	{
		for (int i = 0; i < 4; ++i)
		{
			const float v = edgePoints[e * 4 + i];
			if (v < lo[i]) lo[i] = v;
			if (v > hi[i]) hi[i] = v;
		}
	}
	for (int i = 0; i < 4; ++i)
	{
		out[i]     = lo[i] - expand[i];
		out[4 + i] = hi[i] + expand[i];
	}
}

int main()
{
	// One page of owned edges, so the wild index lands far outside it rather
	// than merely past its end.
	const size_t ownedBytes = (size_t)kOwnedEdges * kEdgeStride;
	unsigned char* ownedEdges = (unsigned char*)VirtualAlloc(
		NULL, ownedBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!ownedEdges) { printf("FAIL could not reserve the owned-edge array\n"); return 1; }
	memset(ownedEdges, 0, ownedBytes);

	FaceRecord face;
	face.startEdge   = kWildStartEdge;
	face.oppositeKey = -1;
	face.numEdges    = (short)kNumEdges;
	face.pad0        = 0;
	face.pad1        = 0;

	unsigned __int64 access = 0;
	const bool faulted = VanillaReadFaults(&face, ownedEdges, &access);
	printf("vanilla read: %s, address 0x%016llX (owned edges at 0x%016llX, %u bytes)\n",
	       faulted ? "faulted" : "did NOT fault", access,
	       (unsigned __int64)ownedEdges, (unsigned int)ownedBytes);
	Check(faulted, "the engine's own read through the recorded record faults");
	Check(access > (unsigned __int64)ownedEdges + ownedBytes,
	      "the address it reaches is outside the owned-edge array");

	const MeshFaceArm arm = ClassifyMeshFaceEdges(face.startEdge, face.numEdges,
	                                              kNumOriginalEdges, kOwnedEdges);
	Check(arm == MESH_FACE_EDGE_OUT_OF_RANGE, "the guard calls it an out-of-range edge run");
	Check(MeshFaceArmSubstitutes(arm), "and answers it itself");

	// The face index the record sat at was 0, which is in range: the guard must
	// not blame the index for a fault that came out of the record's contents.
	Check(ClassifyMeshFaceIndex(0, kNumOriginalFaces, 0) == MESH_FACE_RUN_ORIGINAL,
	      "the face index itself is judged in range");

	// A neighbouring face of the same instance, with a sane run, still goes to
	// the original: the guard must cost the rest of the instance nothing.
	Check(ClassifyMeshFaceEdges(3, 3, kNumOriginalEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "a healthy face of the same instance runs the original");

	// A second instance where the face and its edge run are both genuinely in
	// range -- m_originalEdges itself is a freed array, so a valid-looking run
	// can still fetch a record whose vertex index is the free-list value --
	// and the guard must classify it as the vertex arm, not as edgeRun.
	Check(ClassifyMeshFaceIndex(0, kNumOriginalFaces, 0) == MESH_FACE_RUN_ORIGINAL,
	      "vertex case: the face index is judged in range");
	Check(ClassifyMeshFaceEdges(0, kNumEdges, kNumOriginalEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "vertex case: the edge run is judged in range");
	Check(ClassifyMeshFaceVertex(kWildVertex, kNumOriginalVerts, kOwnedVerts) == MESH_FACE_VERTEX_OUT_OF_RANGE,
	      "vertex case: the garbage vertex index fires as vertIdx");
	Check(ClassifyMeshFaceVertex(0, kNumOriginalVerts, kOwnedVerts) == MESH_FACE_RUN_ORIGINAL,
	      "vertex case: a healthy neighbouring edge's vertex still runs the original");

	// The substituted bounds against the step's own loop with no edges.
	const float seed[4]   = { FLT_MAX, FLT_MAX, FLT_MAX, 0.0f };
	const float expand[4] = { 0.25f, 0.5f, 0.75f, 0.0f };
	float modelled[8], substituted[8];
	ModelStep(seed, expand, 0, NULL, modelled);
	MeshFaceEmptyBounds(seed, expand, substituted);
	bool same = true;
	for (int i = 0; i < 8; ++i)
		if (modelled[i] != substituted[i]) same = false;
	Check(same, "the substituted bounds equal the step's own result for a face with no edges");

	// And again for a seed of zero, which is what the image carries before the
	// engine writes the real one: the two must still agree.
	const float zeroSeed[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	ModelStep(zeroSeed, expand, 0, NULL, modelled);
	MeshFaceEmptyBounds(zeroSeed, expand, substituted);
	same = true;
	for (int i = 0; i < 8; ++i)
		if (modelled[i] != substituted[i]) same = false;
	Check(same, "they agree for a zero seed too");

	VirtualFree(ownedEdges, 0, MEM_RELEASE);

	return CheckExit("mesh_face_guard_injection");
}
