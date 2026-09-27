#ifndef KENSHI_ZONE_OPT_FIXES_MESH_FACE_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_MESH_FACE_GUARD_H

// Detour on the per-face AABB step a navmesh instance's clearance reset runs.
//
// The step fetches a face record, reads the index of its first edge and its
// edge count out of that record, and then walks that many consecutive edges
// through the instance's edge accessor. Neither end of the run is compared
// against the instance's edge space, so a face record whose first four bytes
// no longer hold an index sends the walk at an address the accessor computes
// from a nine-digit offset, and the load through it is an access violation.
// That is measured rather than hypothetical: the same site, the same faulting
// instruction, in three recorded sessions, each with a valid key, a valid slot
// and face index 0.
//
// m_originalEdges is itself one of those inherited arrays, so a run whose
// bounds land inside the instance can still fetch an edge record out of a
// freed block; that record's own vertex index is walked into the vertex
// arrays with no check either.
//
// The detour tests the face index, the edge run, and then every vertex index
// the run names, before the walk. When all three land inside the instance the
// original runs unchanged. When one does not it answers with the bounds the
// engine itself produces for a face carrying no edges, which is the one
// result the caller already handles.
//
// Runs on the navmesh and content-stream threads: no lock, no allocation, and
// nothing logged except through LogMsgDeferrable.
void InstallMeshFaceGuard(int* installed, int*);

#endif // KENSHI_ZONE_OPT_FIXES_MESH_FACE_GUARD_H
