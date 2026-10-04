#ifndef KEO_TOOLS_TESTS_COARSE_GRAPH_LIVE_WORLD_H
#define KEO_TOOLS_TESTS_COARSE_GRAPH_LIVE_WORLD_H

// The fabricated world of the live overlay's host suites: byte arrays the instances point into, the
// builders that lay them out at the real offsets, a live-shaped buffer and the copy call. Each suite
// includes it once and adds its own rows.

#include <cstring>
#include <vector>
#include "planner/coarse_graph_live.h"

namespace coarse_graph_live_world_detail {

using namespace planner;

typedef std::vector<unsigned char> Bytes;

// One fabricated world. Every array is a byte vector the instances point into.
struct World
{
	Bytes coll, infos, gi, nmi, mesh;
	Bytes gnodes, gedges, gpos;
	Bytes faces, edges, verts, owned, sets;
	std::vector<Bytes> conns;
	float shift[3];
};

// A live-shaped buffer with room for the given counts, outside the store's pool.
struct Buffer
{
	CgBlock               b;
	std::vector<CgNode>   nodes;
	std::vector<CgArc>    arcs;
	std::vector<CgBorder> borders;
	std::vector<int>      nodeBorders;
};

static const int   UID       = 0x1305;   // exterior x = 5, y = 19
static const int   OPP_HIGH  = 0x1405;
static const int   OPP_LOW   = 0x1204;
static const int   OTHER_UID = 0x9999;
static const int   SLOT      = 2;

inline void PutInt(Bytes& b, size_t off, int v)                { memcpy(&b[off], &v, 4); }
inline void PutShort(Bytes& b, size_t off, short v)            { memcpy(&b[off], &v, 2); }
inline void PutFloat(Bytes& b, size_t off, float v)            { memcpy(&b[off], &v, 4); }
inline void PutPtr(Bytes& b, size_t off, const void* p)        { memcpy(&b[off], &p, sizeof(p)); }
inline void PutArray(Bytes& b, size_t off, Bytes& data, int n) { PutPtr(b, off, data.empty() ? NULL : &data[0]); PutInt(b, off + 8, n); }

inline void PutVec(Bytes& b, size_t off, float x, float y, float z)
{
	PutFloat(b, off, x);
	PutFloat(b, off + 4, y);
	PutFloat(b, off + 8, z);
}

inline void SetFace(Bytes& faces, int f, int start, short num, short cluster)
{
	PutInt(faces, (size_t)f * 16, start);
	PutShort(faces, (size_t)f * 16 + 8, num);
	PutShort(faces, (size_t)f * 16 + 12, cluster);
}

inline void SetEdge(Bytes& edges, int e, int a, int b)
{
	PutInt(edges, (size_t)e * 20, a);
	PutInt(edges, (size_t)e * 20 + 4, b);
}

inline void SetGraphEdge(Bytes& edges, int e, unsigned short half, unsigned target)
{
	memcpy(&edges[(size_t)e * 8], &half, 2);
	memcpy(&edges[(size_t)e * 8 + 4], &target, 4);
}

inline void SetConnection(Bytes& conn, int j, int face, int edge, int oppFace, int oppEdge)
{
	PutInt(conn, (size_t)j * 16, face);
	PutInt(conn, (size_t)j * 16 + 4, edge);
	PutInt(conn, (size_t)j * 16 + 8, oppFace);
	PutInt(conn, (size_t)j * 16 + 12, oppEdge);
}

// Points every instance field at its array; run after any array is resized.
inline void Link(World& w, int nodes, int graphEdges, int faceCount, int edgeCount)
{
	PutArray(w.coll, OFF_CCC_INSTANCE_DATA, w.infos, 3);
	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO, &w.nmi[0]);
	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH, &w.gi[0]);
	PutPtr(w.gi, LIVE_GI_ORIGINAL_NODES, w.gnodes.empty() ? NULL : &w.gnodes[0]);
	PutInt(w.gi, LIVE_GI_NUM_ORIGINAL_NODES, nodes);
	PutPtr(w.gi, LIVE_GI_ORIGINAL_EDGES, w.gedges.empty() ? NULL : &w.gedges[0]);
	PutInt(w.gi, OFF_GI_BASE_EDGE_COUNT, graphEdges);
	PutPtr(w.gi, OFF_GI_POSITIONS, w.gpos.empty() ? NULL : &w.gpos[0]);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_FACES, &w.faces[0]);
	PutInt(w.nmi, OFF_NMI_NUM_ORIGINAL_FACES, faceCount);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_EDGES, &w.edges[0]);
	PutInt(w.nmi, OFF_NMI_NUM_ORIGINAL_EDGES, edgeCount);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_VERTICES, &w.verts[0]);
	PutInt(w.nmi, OFF_NMI_NUM_ORIGINAL_VERTICES, 4);
	PutArray(w.nmi, OFF_NMI_OWNED_VERTICES, w.owned, 1);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_MESH, &w.mesh[0]);
	PutArray(w.mesh, LIVE_NM_STREAMING_SETS, w.sets, (int)w.conns.size());
	for (size_t s = 0; s < w.conns.size(); ++s)
		PutArray(w.sets, s * LIVE_SET_STRIDE + LIVE_SET_CONNECTIONS, w.conns[s], (int)(w.conns[s].size() / 16));
}

// The graph instance: three nodes; node 0 has an arc to 1 (half 2.0), a self edge and an edge to
// another section; node 1 an arc to 2 (half 1.0); node 2 none. Columns (0,0,-1), (0,1,0), (1,0,0)
// rotate 90 degrees about y, so (x, y, z) -> (z, y, -x), then the translation (100, 5, 200).
inline void BuildGraph(World& w, int nodes)
{
	w.gi.assign(272, 0);
	w.gnodes.assign((size_t)nodes * 8, 0);
	w.gpos.assign((size_t)nodes * 16, 0);
	w.gedges.assign(4 * 8, 0);
	PutInt(w.gi, OFF_GI_SECTION, SLOT);
	PutVec(w.gi, OFF_GI_ROW0, 0.0f, 0.0f, -1.0f);
	PutVec(w.gi, OFF_GI_ROW0 + 16, 0.0f, 1.0f, 0.0f);
	PutVec(w.gi, OFF_GI_ROW0 + 32, 1.0f, 0.0f, 0.0f);
	PutVec(w.gi, OFF_GI_TRANSLATION, 100.0f, 5.0f, 200.0f);
	if (nodes >= 3)
	{
		PutInt(w.gnodes, 0, 0);  PutInt(w.gnodes, 4, 3);
		PutInt(w.gnodes, 8, 3);  PutInt(w.gnodes, 12, 1);
		PutInt(w.gnodes, 16, 4); PutInt(w.gnodes, 20, 0);
		PutVec(w.gpos, 0, 1.0f, 2.0f, 3.0f);
		PutVec(w.gpos, 16, 0.0f, 0.0f, 0.0f);
		PutVec(w.gpos, 32, -2.0f, 1.0f, 4.0f);
	}
	SetGraphEdge(w.gedges, 0, 0x4000, 1);
	SetGraphEdge(w.gedges, 1, 0x4000, 0);
	SetGraphEdge(w.gedges, 2, 0x4000, 1u | (5u << 22));
	SetGraphEdge(w.gedges, 3, 0x3F80, 2);
}

// The mesh instance, translated by (1000, 0, 2000): vertices (0,0,0), (2,0,0), (2,0,2), (0,0,2)
// and one owned vertex (4,1,4) at index 4. Face 0 (cluster 0) runs edges 0-2 over vertices 0, 1,
// 2; face 1 (cluster 1) edges 3-5 over 2, 3 and the owned 4; face 2 names cluster 7 (no node);
// face 3's run passes the edge array. A fifth face record past the original count names cluster 2.
inline void BuildMesh(World& w)
{
	w.nmi.assign(0x1B0, 0);
	w.mesh.assign(0xB0, 0);
	w.faces.assign(5 * 16, 0);
	w.edges.assign(6 * 20, 0);
	w.verts.assign(4 * 16, 0);
	w.owned.assign(16, 0);
	PutInt(w.nmi, OFF_NMI_SECTION_UID, UID);
	PutVec(w.nmi, LIVE_NMI_FRAME_COL0, 1.0f, 0.0f, 0.0f);
	PutVec(w.nmi, LIVE_NMI_FRAME_COL1, 0.0f, 1.0f, 0.0f);
	PutVec(w.nmi, LIVE_NMI_FRAME_COL2, 0.0f, 0.0f, 1.0f);
	PutVec(w.nmi, LIVE_NMI_FRAME_TRANSLATION, 1000.0f, 0.0f, 2000.0f);
	PutVec(w.verts, 0, 0.0f, 0.0f, 0.0f);
	PutVec(w.verts, 16, 2.0f, 0.0f, 0.0f);
	PutVec(w.verts, 32, 2.0f, 0.0f, 2.0f);
	PutVec(w.verts, 48, 0.0f, 0.0f, 2.0f);
	PutVec(w.owned, 0, 4.0f, 1.0f, 4.0f);
	SetEdge(w.edges, 0, 0, 1);
	SetEdge(w.edges, 1, 1, 2);
	SetEdge(w.edges, 2, 2, 0);
	SetEdge(w.edges, 3, 2, 3);
	SetEdge(w.edges, 4, 3, 4);
	SetEdge(w.edges, 5, 4, 2);
	SetFace(w.faces, 0, 0, 3, 0);
	SetFace(w.faces, 1, 3, 3, 1);
	SetFace(w.faces, 2, 0, 3, 7);
	SetFace(w.faces, 3, 5, 3, 2);
	SetFace(w.faces, 4, 0, 3, 2);
}

// Set 0 (this uid, to OPP_HIGH): face 1 over edge 3, face 0 over edge 0, a face equal to the
// original count, and an edge past the edge array. Set 1 names another uid. Set 2 (this uid, to
// OPP_LOW): face 0 over edge 1.
inline void BuildSets(World& w)
{
	w.conns.assign(3, Bytes());
	w.conns[0].assign(4 * 16, 0);
	SetConnection(w.conns[0], 0, 1, 3, 7, 9);
	SetConnection(w.conns[0], 1, 0, 0, 2, 1);
	SetConnection(w.conns[0], 2, 4, 0, 3, 3);
	SetConnection(w.conns[0], 3, 0, 6, 4, 4);
	w.conns[1].assign(16, 0);
	SetConnection(w.conns[1], 0, 0, 0, 5, 5);
	w.conns[2].assign(16, 0);
	SetConnection(w.conns[2], 0, 0, 1, 6, 6);
	w.sets.assign(3 * LIVE_SET_STRIDE, 0);
	PutInt(w.sets, 0 * LIVE_SET_STRIDE + LIVE_SET_THIS_UID, UID);
	PutInt(w.sets, 0 * LIVE_SET_STRIDE + LIVE_SET_OPP_UID, OPP_HIGH);
	PutInt(w.sets, 1 * LIVE_SET_STRIDE + LIVE_SET_THIS_UID, OTHER_UID);
	PutInt(w.sets, 1 * LIVE_SET_STRIDE + LIVE_SET_OPP_UID, 0x7777);
	PutInt(w.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_THIS_UID, UID);
	PutInt(w.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_OPP_UID, OPP_LOW);
}

inline void Build(World& w, int nodes)
{
	w.coll.assign(48, 0);
	w.infos.assign(3 * SIZE_CCC_INSTANCE_INFO, 0);
	BuildGraph(w, nodes);
	BuildMesh(w);
	BuildSets(w);
	w.shift[0] = 30.0f;
	w.shift[1] = 0.0f;
	w.shift[2] = -40.0f;
	Link(w, nodes, 4, 4, 6);
}

inline void MakeBuffer(Buffer& x, int nodes, int arcs, int borders)
{
	memset(&x.b, 0, sizeof(x.b));
	x.nodes.assign((size_t)nodes, CgNode());
	x.arcs.assign((size_t)arcs, CgArc());
	x.borders.assign((size_t)borders, CgBorder());
	x.nodeBorders.assign((size_t)borders, 0);
	x.b.liveBuffer = -1;
	x.b.collSlot = -1;
	x.b.nodes = &x.nodes[0];
	x.b.arcs = &x.arcs[0];
	x.b.borders = &x.borders[0];
	x.b.nodeBorders = &x.nodeBorders[0];
}

inline CgLiveResult Copy(World& w, Buffer& x, CgLiveCounts* c)
{
	return CgLiveCopy(&w.gi[0], &w.coll[0], w.shift, &x.b, c);
}

} // namespace coarse_graph_live_world_detail
using namespace coarse_graph_live_world_detail;

#endif
