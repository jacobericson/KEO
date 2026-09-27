#include "fixes/streaming/mesh_face_guard_policy.h"

bool MeshFaceArmSubstitutes(MeshFaceArm arm)
{
	return arm == MESH_FACE_INDEX_OUT_OF_RANGE || arm == MESH_FACE_EDGE_OUT_OF_RANGE
	    || arm == MESH_FACE_VERTEX_OUT_OF_RANGE || arm == MESH_FACE_NULL_RECORD;
}

// A negative count is not a count. Treating it as zero keeps the space from
// shrinking below the half that is still readable, so the test stays an upper
// bound and never rejects an index the engine would have resolved.
__int64 MeshFaceIndexSpace(int numOriginal, int ownedCount)
{
	__int64 a = numOriginal > 0 ? (__int64)numOriginal : 0;
	__int64 b = ownedCount  > 0 ? (__int64)ownedCount  : 0;
	return a + b;
}

MeshFaceArm ClassifyMeshFaceIndex(int faceIndex, int numOriginalFaces, int ownedFaceCount)
{
	if (faceIndex < 0)
		return MESH_FACE_INDEX_OUT_OF_RANGE;
	if ((__int64)faceIndex >= MeshFaceIndexSpace(numOriginalFaces, ownedFaceCount))
		return MESH_FACE_INDEX_OUT_OF_RANGE;
	return MESH_FACE_RUN_ORIGINAL;
}

MeshFaceArm ClassifyMeshFaceEdges(int startEdge, int numEdges,
                                  int numOriginalEdges, int ownedEdgeCount)
{
	// A face with no edges contributes nothing and the engine's loop never
	// runs, so its start index is not an index at all and is left alone.
	if (numEdges <= 0)
		return MESH_FACE_RUN_ORIGINAL;
	if (startEdge < 0)
		return MESH_FACE_EDGE_OUT_OF_RANGE;
	if ((__int64)startEdge + numEdges > MeshFaceIndexSpace(numOriginalEdges, ownedEdgeCount))
		return MESH_FACE_EDGE_OUT_OF_RANGE;
	return MESH_FACE_RUN_ORIGINAL;
}

MeshFaceArm ClassifyMeshFaceVertex(int vertexIndex, int numOriginalVertices, int ownedVertexCount)
{
	if (vertexIndex < 0)
		return MESH_FACE_VERTEX_OUT_OF_RANGE;
	if ((__int64)vertexIndex >= MeshFaceIndexSpace(numOriginalVertices, ownedVertexCount))
		return MESH_FACE_VERTEX_OUT_OF_RANGE;
	return MESH_FACE_RUN_ORIGINAL;
}

void MeshFaceEmptyBounds(const float* seed, const float* expand, float* out)
{
	for (int i = 0; i < 4; ++i)
	{
		out[i]     = seed[i] - expand[i];
		out[4 + i] = -seed[i] + expand[i];
	}
}
