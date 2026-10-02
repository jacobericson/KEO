// coarse_graph_live.h - The live overlay: a registering section's coarse graph, copied from its
// navmesh and graph instances into a store buffer, and the comparison that finds the registered
// sections a later stitch changed. The walk and the comparison are pure over the instances' memory;
// the path thread runs them inside NavMesh::update's exclusive world lock, where the instances and
// their arrays stay put (a stitch on a generator thread may still rewrite a mesh's streaming sets),
// and they allocate nothing and take no lock.
#ifndef KEO_PLANNER_COARSE_GRAPH_LIVE_H
#define KEO_PLANNER_COARSE_GRAPH_LIVE_H

#include <stddef.h>
#include "fixes/search/cluster_cross_cost_policy.h"    // OFF_GI_*, OFF_CCC_*
#include "fixes/streaming/mesh_face_guard_policy.h"    // OFF_NMI_*, OFF_HKARRAY_SIZE
#include "planner/coarse_graph.h"                      // CgBlock, the capacities

namespace planner {

const size_t LIVE_GI_ORIGINAL_NODES     = 16;
const size_t LIVE_GI_NUM_ORIGINAL_NODES = 24;
const size_t LIVE_GI_ORIGINAL_EDGES     = 32;
const size_t LIVE_NMI_FRAME_COL0        = 0x70;
const size_t LIVE_NMI_FRAME_COL1        = 0x80;
const size_t LIVE_NMI_FRAME_COL2        = 0x90;
const size_t LIVE_NMI_FRAME_TRANSLATION = 0xA0;
const size_t LIVE_NM_STREAMING_SETS     = 0x40;   // hkaiNavMesh::m_streamingSets (hkArray)
const size_t LIVE_NM_FACE_DATA          = 0x50;   // hkaiNavMesh::m_faceData (hkArray of int)
const size_t LIVE_NM_FACE_DATA_STRIDING = 0x70;   // hkaiNavMesh::m_faceDataStriding
const size_t LIVE_SET_STRIDE            = 56;
const size_t LIVE_SET_THIS_UID          = 0;
const size_t LIVE_SET_OPP_UID           = 4;
const size_t LIVE_SET_CONNECTIONS       = 8;      // hkArray of 16-byte connections
const size_t LIVE_FACE_STRIDE           = 16;
const size_t LIVE_FACE_CLUSTER          = 12;     // int16
const size_t LIVE_EDGE_STRIDE           = 20;
const size_t LIVE_VERTEX_STRIDE         = 16;

enum CgLiveResult
{
	CGL_OK = 0,
	CGL_BAD_SLOT,       // the graph instance's slot is not below the collection's count
	CGL_NO_MESH,        // InstanceInfo[slot]+0 is NULL, or its original mesh is
	CGL_OVER_NODES,     // more nodes than the buffer holds
	CGL_OVER_ARCS,
	CGL_OVER_BORDERS
};
struct CgLiveCounts { int slot; long faces; long bordersSkipped; long arcsSkipped; long waterNodes; long noFaceData;
                      long zeroStride; };

// Fills buf (a live buffer from CgLiveAcquire) from the registering graph instance and its mesh
// instance, and reports the graph instance's collection slot in counts->slot. shift is
// *(NavMesh+0x1D8), read once by the caller. On any result but CGL_OK the buffer is left for
// CgLiveRelease.
CgLiveResult CgLiveCopy(const void* graphInst, const void* coll, const float shift[3],
                        CgBlock* buf, CgLiveCounts* counts);

// Pure reads shared with the main thread's locator and loaded-set snapshot (called there under a
// try-shared world lock).
int         CgCollectionCount(const void* coll);
const void* CgCollectionMeshInstance(const void* coll, int slot);   // NULL out of range
int         CgMeshInstanceUid(const void* meshInst);
int         CgInstanceFaceCluster(const void* meshInst, unsigned face);   // -1 when out of range

// The stitch comparison (path thread). A record per collection slot holds the section uid, the
// store generation and the signature of its own streaming sets at its last posted copy.
const int CG_LIVE_REFRESH_MAX = 128;   // opposite sections one registration compares
struct CgLiveSig { int valid; int uid; unsigned gen; unsigned sig; };
// FNV-1a over mesh's sets whose this-side uid is uid, in set order: the opposite uid's 4 bytes,
// the connection count's 4 bytes, then each connection's 16 bytes. A NULL array or a negative count
// reads as empty.
unsigned    CgLiveSetSignature(const void* mesh, int uid);
// The section at slot: its uid and its own sets' signature; false without a mesh instance or mesh.
bool        CgLiveSlotSignature(const void* coll, int slot, int* uidOut, unsigned* sigOut);
const void* CgCollectionGraphInstance(const void* coll, int slot);   // InstanceInfo[slot]+16; NULL out of range
// The first slot whose mesh instance carries uid and that holds a graph instance; -1 when none.
int         CgCollectionSlotOfUid(const void* coll, int uid);
// Each distinct opposite uid meshInst's own sets name, registered at a slot other than ownSlot, is
// compared with sigs[slot] and counted in *checked; it is listed (its slot and current signature)
// when the record is not valid, names another uid or generation, or holds another signature. A slot
// at or past sigCount has no record. Opposites past max count in *overflow, neither listed nor
// compared. Returns the slots listed.
int         CgLiveStaleNeighbours(const void* coll, const void* meshInst, int ownSlot, const CgLiveSig* sigs,
                                  int sigCount, unsigned gen, int* slotsOut, unsigned* sigsOut, int max,
                                  int* checked, int* overflow);

// Game side (coarse_graph_live_site.cpp). Path thread, from the graph-instance connect's post-call
// on the live collection: one acquire, one copy, one post into the slot's record; every refusal
// counted. Then each registered section the registering mesh's sets name whose sets changed since
// its last copy (a stitch from a later navmesh) is copied again. Every copy and the comparison run
// under a fault guard, and a copy whose signature moved while it ran is discarded. Returns at once
// when the store is not ready.
void CgLiveOnConnect(void* graphInst, void* coll);
// Main thread, from the planner's frame step: the PlannerLive: line on its timer.
void CgLiveReport(double now);

} // namespace planner

#endif
