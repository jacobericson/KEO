// Host tests for the per-face bounds decision. Links
// src/fixes/streaming/mesh_face_guard_policy.cpp unmodified; no game, no Windows header.

#include <cstdio>
#include "fixes/streaming/mesh_face_guard_policy.h"

#include "check.h"

// The numbers the recorded fault carried: 23 original faces with none owned,
// 113 original edges against 276 the instance owns, and a face record whose
// first field had been overwritten by the low half of a pointer.
static const int kFaces       = 23;
static const int kOwnedFaces  = 0;
static const int kEdges       = 113;
static const int kOwnedEdges  = 276;
static const int kWildStart   = 1445915680;   // 0x562EEC20
static const int kVerts       = 70;
static const int kOwnedVerts  = 116;

static void IndexSpace()
{
	Check(MeshFaceIndexSpace(23, 0) == 23, "space: originals only");
	Check(MeshFaceIndexSpace(113, 276) == 389, "space: both halves");
	Check(MeshFaceIndexSpace(0, 0) == 0, "space: empty instance");
	// A count read out of a half-torn object can be negative; treating it as
	// zero keeps the result an upper bound instead of a negative one.
	Check(MeshFaceIndexSpace(-5, 276) == 276, "space: negative original count");
	Check(MeshFaceIndexSpace(113, -5) == 113, "space: negative owned count");
	// 2^31-1 + 2^31-1 must not wrap.
	Check(MeshFaceIndexSpace(2147483647, 2147483647) == 4294967294LL, "space: no overflow");
}

static void FaceIndex()
{
	Check(ClassifyMeshFaceIndex(0, kFaces, kOwnedFaces) == MESH_FACE_RUN_ORIGINAL,
	      "face: index 0 of 23 runs");
	Check(ClassifyMeshFaceIndex(22, kFaces, kOwnedFaces) == MESH_FACE_RUN_ORIGINAL,
	      "face: last original runs");
	Check(ClassifyMeshFaceIndex(23, kFaces, kOwnedFaces) == MESH_FACE_INDEX_OUT_OF_RANGE,
	      "face: one past the end fires");
	Check(ClassifyMeshFaceIndex(23, kFaces, 4) == MESH_FACE_RUN_ORIGINAL,
	      "face: first owned face runs");
	Check(ClassifyMeshFaceIndex(27, kFaces, 4) == MESH_FACE_INDEX_OUT_OF_RANGE,
	      "face: one past the owned faces fires");
	Check(ClassifyMeshFaceIndex(-1, kFaces, kOwnedFaces) == MESH_FACE_INDEX_OUT_OF_RANGE,
	      "face: negative fires");
	Check(ClassifyMeshFaceIndex(0, 0, 0) == MESH_FACE_INDEX_OUT_OF_RANGE,
	      "face: empty instance fires on any index");
}

static void EdgeRun()
{
	Check(ClassifyMeshFaceEdges(0, 3, kEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "edges: a run at the start is in range");
	Check(ClassifyMeshFaceEdges(386, 3, kEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "edges: a run ending exactly at the end is in range");
	Check(ClassifyMeshFaceEdges(387, 3, kEdges, kOwnedEdges) == MESH_FACE_EDGE_OUT_OF_RANGE,
	      "edges: a run ending one past the end fires");
	Check(ClassifyMeshFaceEdges(112, 2, kEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "edges: a run crossing from original into owned is in range");
	// The recorded fault, exactly.
	Check(ClassifyMeshFaceEdges(kWildStart, 3, kEdges, kOwnedEdges) == MESH_FACE_EDGE_OUT_OF_RANGE,
	      "edges: the recorded wild start fires");
	Check(ClassifyMeshFaceEdges(-1, 3, kEdges, kOwnedEdges) == MESH_FACE_EDGE_OUT_OF_RANGE,
	      "edges: a negative start fires");
	// A face with no edges contributes nothing, so its start field is not an
	// index and the engine's own loop never runs. It must not fire, or every
	// such face would be counted as a fault.
	Check(ClassifyMeshFaceEdges(kWildStart, 0, kEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "edges: a face with no edges is left alone");
	Check(ClassifyMeshFaceEdges(0, -1, kEdges, kOwnedEdges) == MESH_FACE_RUN_ORIGINAL,
	      "edges: a negative edge count is left alone");
	// 2147483647 + 3 must fire on the bound, not wrap to a negative and pass.
	Check(ClassifyMeshFaceEdges(2147483647, 3, kEdges, kOwnedEdges) == MESH_FACE_EDGE_OUT_OF_RANGE,
	      "edges: the sum does not wrap");
}

static void VertexIndex()
{
	Check(ClassifyMeshFaceVertex(0, kVerts, kOwnedVerts) == MESH_FACE_RUN_ORIGINAL,
	      "vertex: index 0 of 70 runs");
	Check(ClassifyMeshFaceVertex(69, kVerts, kOwnedVerts) == MESH_FACE_RUN_ORIGINAL,
	      "vertex: last original runs");
	Check(ClassifyMeshFaceVertex(70, kVerts, 4) == MESH_FACE_RUN_ORIGINAL,
	      "vertex: first owned vertex runs");
	Check(ClassifyMeshFaceVertex(185, kVerts, 116) == MESH_FACE_RUN_ORIGINAL,
	      "vertex: last owned vertex of the recorded instance runs");
	Check(ClassifyMeshFaceVertex(186, kVerts, kOwnedVerts) == MESH_FACE_VERTEX_OUT_OF_RANGE,
	      "vertex: one past the end fires");
	Check(ClassifyMeshFaceVertex(-1, kVerts, kOwnedVerts) == MESH_FACE_VERTEX_OUT_OF_RANGE,
	      "vertex: negative fires");
	// The face and edge run can both be genuinely in range while the edge
	// record itself is garbage (m_originalEdges is a freed array too), so a
	// wild vertex index like the recorded fault's edge start must fire here
	// exactly as it would have as an edge-run index.
	Check(ClassifyMeshFaceVertex(kWildStart, kVerts, kOwnedVerts) == MESH_FACE_VERTEX_OUT_OF_RANGE,
	      "vertex: a wild index out of a garbage edge record fires");
	Check(ClassifyMeshFaceVertex(0, 0, 0) == MESH_FACE_VERTEX_OUT_OF_RANGE,
	      "vertex: empty instance fires on any index");
}

// No healthy face may fire: the three tests together over a whole instance
// whose faces name consecutive edge runs, each edge naming a vertex within
// the same walk of the vertex space -- what a generated section is.
static void HealthyInstanceNeverFires()
{
	int fired = 0;
	int edge = 0;
	int vertex = 0;
	for (int f = 0; f < kFaces; ++f)
	{
		if (MeshFaceArmSubstitutes(ClassifyMeshFaceIndex(f, kFaces, kOwnedFaces)))
			++fired;
		const int numEdges = 3;
		if (MeshFaceArmSubstitutes(ClassifyMeshFaceEdges(edge, numEdges, kEdges, kOwnedEdges)))
			++fired;
		for (int e = 0; e < numEdges; ++e)
		{
			if (MeshFaceArmSubstitutes(ClassifyMeshFaceVertex(vertex, kVerts, kOwnedVerts)))
				++fired;
			vertex = (vertex + 1) % (kVerts + kOwnedVerts);
		}
		edge += numEdges;
	}
	Check(fired == 0, "healthy: no face of a consistent instance fires");
}

static void Arms()
{
	Check(!MeshFaceArmSubstitutes(MESH_FACE_RUN_ORIGINAL), "arm: in range runs the original");
	Check(MeshFaceArmSubstitutes(MESH_FACE_INDEX_OUT_OF_RANGE), "arm: face index substitutes");
	Check(MeshFaceArmSubstitutes(MESH_FACE_EDGE_OUT_OF_RANGE), "arm: edge run substitutes");
	Check(MeshFaceArmSubstitutes(MESH_FACE_VERTEX_OUT_OF_RANGE), "arm: vertex index substitutes");
	Check(MeshFaceArmSubstitutes(MESH_FACE_NULL_RECORD), "arm: a missing record substitutes");
}

static void Offsets()
{
	// The layout the decision is read against, pinned so a future edit that
	// moves one of them fails here rather than in the game.
	Check(OFF_NMI_NUM_ORIGINAL_EDGES == 0x28, "offset: original edge count");
	Check(OFF_NMI_OWNED_EDGES + OFF_HKARRAY_SIZE == 0x128, "offset: owned edge count");
	Check(OFF_NMI_NUM_ORIGINAL_FACES == 0x18, "offset: original face count");
	Check(OFF_NMI_OWNED_FACES + OFF_HKARRAY_SIZE == 0x118, "offset: owned face count");
	Check(OFF_NMI_NUM_ORIGINAL_VERTICES == 0x38, "offset: original vertex count");
	Check(OFF_NMI_OWNED_VERTICES + OFF_HKARRAY_SIZE == 0x138, "offset: owned vertex count");
	Check(OFF_NMI_ORIGINAL_MESH == 0x60, "offset: the instanced mesh");
	Check(OFF_NMI_SECTION_UID == 0x1A0, "offset: section uid");
	Check(OFF_FACE_NUM_EDGES == 8, "offset: face edge count");
	Check(OFF_EDGE_VERTEX_INDEX == 0, "offset: edge vertex index");
	Check(HKARRAY_CAP_MASK == 0x3FFFFFFF, "offset: capacity mask");
}

int main()
{
	IndexSpace();
	FaceIndex();
	EdgeRun();
	VertexIndex();
	HealthyInstanceNeverFires();
	Arms();
	Offsets();
	return CheckExit("mesh_face_guard_units");
}
