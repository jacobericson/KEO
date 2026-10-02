#ifndef KEO_FIXES_MESH_FACE_GUARD_POLICY_H
#define KEO_FIXES_MESH_FACE_GUARD_POLICY_H

#include <stddef.h>

// Layout and decision for the per-face bounds test the navmesh instance's own
// face-AABB step never makes. Pure arithmetic over values already read: no
// Windows header, no game pointer, so a host test drives every arm.

// hkaiNavMeshInstance. The object copies the original mesh's data pointers and
// element counts into its own head and keeps the pieces it owns in hkArrays
// further down; the counts below are the two halves of each index space.
const size_t OFF_NMI_ORIGINAL_FACES     = 0x10;
const size_t OFF_NMI_NUM_ORIGINAL_FACES = 0x18;
const size_t OFF_NMI_ORIGINAL_EDGES     = 0x20;
const size_t OFF_NMI_NUM_ORIGINAL_EDGES = 0x28;
const size_t OFF_NMI_ORIGINAL_VERTICES     = 0x30;
const size_t OFF_NMI_NUM_ORIGINAL_VERTICES = 0x38;
const size_t OFF_NMI_ORIGINAL_MESH      = 0x60;  // the one reference the dtor releases
const size_t OFF_NMI_EDGE_MAP           = 0xD0;
const size_t OFF_NMI_FACE_MAP           = 0xE0;
const size_t OFF_NMI_INSTANCED_FACES    = 0xF0;
const size_t OFF_NMI_INSTANCED_EDGES    = 0x100;
const size_t OFF_NMI_OWNED_FACES        = 0x110;
const size_t OFF_NMI_OWNED_EDGES        = 0x120;
const size_t OFF_NMI_OWNED_VERTICES     = 0x130;
const size_t OFF_NMI_SECTION_UID        = 0x1A0;
const size_t OFF_NMI_RUNTIME_ID         = 0x1A4;

// hkArray: pointer, element count, then capacity with two flag bits above it.
const size_t       OFF_HKARRAY_SIZE = 8;
const size_t       OFF_HKARRAY_CAP  = 12;
const unsigned int HKARRAY_CAP_MASK = 0x3FFFFFFF;

// A face record: the index of its first edge, an opposite-face key, then the
// edge count as a 16-bit field.
const size_t OFF_FACE_START_EDGE = 0;
const size_t OFF_FACE_NUM_EDGES  = 8;

// An edge record (hkaiNavMesh::Edge, 20 bytes): its first field is the index
// of the vertex the step reads; the step never touches the second (end)
// vertex, an opposite-edge key, an opposite-face key or the flags/cost tail.
const size_t OFF_EDGE_VERTEX_INDEX = 0;

// hkReferencedObject: the reference count is the low half of the dword at +8,
// and the half above it is a size-and-flags word that reads 0xFFFF for an
// object the allocator owns.
const size_t OFF_REFOBJ_COUNT = 8;
const size_t OFF_REFOBJ_FLAGS = 10;

enum MeshFaceArm
{
	MESH_FACE_RUN_ORIGINAL = 0,   // in range; the engine's own walk runs untouched
	MESH_FACE_INDEX_OUT_OF_RANGE, // the face index misses both halves of the face space
	MESH_FACE_EDGE_OUT_OF_RANGE,  // the face's edge run misses both halves of the edge space
	MESH_FACE_VERTEX_OUT_OF_RANGE,// an edge in the run names a vertex outside both halves
	MESH_FACE_NULL_RECORD         // the face accessor itself returned no record
};

// The arms the guard answers itself, in place of the original.
bool MeshFaceArmSubstitutes(MeshFaceArm arm);

// The total index space of each kind, which is what the engine's own accessors
// bound against: the original mesh's elements first, then the ones this
// instance created while cutting. Returned as a 64-bit value so a caller can
// add an element count to an index without overflowing.
__int64 MeshFaceIndexSpace(int numOriginal, int ownedCount);

// Whether the face index names a face of this instance. Nothing in the engine
// compares it against anything before turning it into an address.
MeshFaceArm ClassifyMeshFaceIndex(int faceIndex, int numOriginalFaces, int ownedFaceCount);

// Whether every edge the face names is an edge of this instance. The engine
// walks [startEdge, startEdge + numEdges) and bounds neither end, so a face
// record whose first four bytes have been overwritten sends the walk at an
// address hundreds of megabytes away.
MeshFaceArm ClassifyMeshFaceEdges(int startEdge, int numEdges,
                                  int numOriginalEdges, int ownedEdgeCount);

// Whether one edge's vertex index (its record's first field) names a vertex
// of this instance. m_originalEdges is itself one of the six arrays a freed
// backing store clobbers, so an edge record fetched from an in-range slot can
// still carry a vertex index that was never a count.
MeshFaceArm ClassifyMeshFaceVertex(int vertexIndex, int numOriginalVertices, int ownedVertexCount);

// The bounds the step writes for a face whose edge loop never runs: its two
// accumulators still hold what it seeded them with -- a value and that value's
// sign flip -- and the caller's expansion is applied to each. seed and expand
// are four floats; out receives eight, the minimum then the maximum. The seed
// is passed in rather than named here because the engine writes it at start-up
// and the image carries zeros in its place.
void MeshFaceEmptyBounds(const float* seed, const float* expand, float* out);

#endif // KEO_FIXES_MESH_FACE_GUARD_POLICY_H
