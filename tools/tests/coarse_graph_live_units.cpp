// The live overlay's walk over fabricated instances at the real offsets: a three-slot collection,
// a graph instance in slot 2 rotated 90 degrees about y and translated, a mesh instance translated,
// its original mesh with three streaming sets, and a non-zero world shift. Every expected world
// value below is worked by hand from the fabricated numbers. Each over-capacity row allocates its
// section and its buffer one element past the capacity, so a dropped bound prints the row's FAIL
// line instead of writing past an allocation. One row runs a copy through the store's own pool,
// record and promotion.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "planner/coarse_graph_live.h"

#include "check.h"

using namespace planner;

namespace coarse_graph_live_units_detail {

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

} // namespace coarse_graph_live_units_detail
using namespace coarse_graph_live_units_detail;

static const int   UID       = 0x1305;   // exterior x = 5, y = 19
static const int   OPP_HIGH  = 0x1405;
static const int   OPP_LOW   = 0x1204;
static const int   OTHER_UID = 0x9999;
static const int   SLOT      = 2;

static void PutInt(Bytes& b, size_t off, int v)                { memcpy(&b[off], &v, 4); }
static void PutShort(Bytes& b, size_t off, short v)            { memcpy(&b[off], &v, 2); }
static void PutFloat(Bytes& b, size_t off, float v)            { memcpy(&b[off], &v, 4); }
static void PutPtr(Bytes& b, size_t off, const void* p)        { memcpy(&b[off], &p, sizeof(p)); }
static void PutArray(Bytes& b, size_t off, Bytes& data, int n) { PutPtr(b, off, data.empty() ? NULL : &data[0]); PutInt(b, off + 8, n); }

static void PutVec(Bytes& b, size_t off, float x, float y, float z)
{
	PutFloat(b, off, x);
	PutFloat(b, off + 4, y);
	PutFloat(b, off + 8, z);
}

static void SetFace(Bytes& faces, int f, int start, short num, short cluster)
{
	PutInt(faces, (size_t)f * 16, start);
	PutShort(faces, (size_t)f * 16 + 8, num);
	PutShort(faces, (size_t)f * 16 + 12, cluster);
}

static void SetEdge(Bytes& edges, int e, int a, int b)
{
	PutInt(edges, (size_t)e * 20, a);
	PutInt(edges, (size_t)e * 20 + 4, b);
}

static void SetGraphEdge(Bytes& edges, int e, unsigned short half, unsigned target)
{
	memcpy(&edges[(size_t)e * 8], &half, 2);
	memcpy(&edges[(size_t)e * 8 + 4], &target, 4);
}

static void SetConnection(Bytes& conn, int j, int face, int edge, int oppFace, int oppEdge)
{
	PutInt(conn, (size_t)j * 16, face);
	PutInt(conn, (size_t)j * 16 + 4, edge);
	PutInt(conn, (size_t)j * 16 + 8, oppFace);
	PutInt(conn, (size_t)j * 16 + 12, oppEdge);
}

// Points every instance field at its array; run after any array is resized.
static void Link(World& w, int nodes, int graphEdges, int faceCount, int edgeCount)
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
static void BuildGraph(World& w, int nodes)
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
static void BuildMesh(World& w)
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
static void BuildSets(World& w)
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

static void Build(World& w, int nodes)
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

static void MakeBuffer(Buffer& x, int nodes, int arcs, int borders)
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

static bool Near(float a, float b)
{
	return std::fabs(a - b) < 0.01f;
}

static bool NearV(const float* v, float x, float y, float z)
{
	return Near(v[0], x) && Near(v[1], y) && Near(v[2], z);
}

static CgLiveResult Copy(World& w, Buffer& x, CgLiveCounts* c)
{
	return CgLiveCopy(&w.gi[0], &w.coll[0], w.shift, &x.b, c);
}

// The base world, copied once; the rows below read its result.
static void CheckBaseWorld()
{
	World w;
	Build(w, 3);
	Buffer x;
	MakeBuffer(x, 8, 8, 8);
	CgLiveCounts c;
	CgLiveResult r = Copy(w, x, &c);
	Check(r == CGL_OK && c.slot == SLOT && x.b.uid == UID && x.b.source == CG_LIVE && x.b.nodeCount == 3,
	      "live: the base world copies, its slot and uid reported");

	// Node 0 at (1, 2, 3): (3, 2, -1) + (100, 5, 200) = (103, 7, 199); minus the shift (30, 0, -40)
	// is (73, 7, 239); times 10.
	Check(NearV(x.nodes[0].centre, 730.0f, 70.0f, 2390.0f),
	      "live: a node's centre is the graph instance's frame, unshifted, in world units");

	// The border over edge 3 (vertices 2 and 3): Havok midpoint (1001, 0, 2002), minus the shift,
	// times 10. It sorts last: (OPP_LOW, 0), (OPP_HIGH, 0), (OPP_HIGH, 1).
	Check(x.b.borderCount == 3 && NearV(x.borders[2].portal, 9710.0f, 0.0f, 20420.0f)
	      && NearV(x.borders[2].a, 9720.0f, 0.0f, 20420.0f) && NearV(x.borders[2].b, 9700.0f, 0.0f, 20420.0f),
	      "live: a portal is stored in world units");

	Check(x.b.arcCount == 2 && x.nodes[0].firstArc == 0 && x.nodes[0].arcCount == 1 && x.arcs[0].to == 1
	      && Near(x.arcs[0].cost, 20.0f) && x.nodes[1].arcCount == 1 && x.arcs[1].to == 2
	      && Near(x.arcs[1].cost, 10.0f) && x.nodes[2].arcCount == 0 && c.arcsSkipped == 2,
	      "live: an intra arc's cost is ten times its half");

	// Node 0's face: vertices 0, 1, 2 at (1000..1002, 0, 2000..2002), shifted, times 10. Node 2 has
	// no face: its box is its centre. Faces 2 (no node) and 3 (bad run) count nowhere.
	Check(NearV(x.nodes[0].boxMin, 9700.0f, 0.0f, 20400.0f) && NearV(x.nodes[0].boxMax, 9720.0f, 0.0f, 20420.0f)
	      && x.nodes[0].faces == 1 && x.nodes[1].faces == 1 && x.nodes[2].faces == 0 && c.faces == 2
	      && NearV(x.nodes[2].boxMin, x.nodes[2].centre[0], x.nodes[2].centre[1], x.nodes[2].centre[2])
	      && NearV(x.nodes[2].boxMax, x.nodes[2].centre[0], x.nodes[2].centre[1], x.nodes[2].centre[2]),
	      "live: a footprint bounds its faces' vertices");

	// Node 1's face reaches the owned vertex (4, 1, 4): (1004, 1, 2004), shifted, times 10.
	Check(NearV(x.nodes[1].boxMin, 9700.0f, 0.0f, 20420.0f) && NearV(x.nodes[1].boxMax, 9740.0f, 10.0f, 20440.0f),
	      "live: a vertex past the original count reads the owned array");

	bool other = false;
	for (int i = 0; i < x.b.borderCount; ++i)
		other = other || x.borders[i].oppUid == 0x7777;
	Check(!other && x.b.borderCount == 3, "live: a set naming another uid is skipped");

	bool face4 = false;
	for (int i = 0; i < x.b.borderCount; ++i)
		face4 = face4 || x.borders[i].face == 4;
	Check(!face4 && x.b.borderCount == 3 && c.bordersSkipped == 2,
	      "live: a border whose face is out of range is skipped and counted");

	Check(x.borders[0].oppUid == OPP_LOW && x.borders[0].face == 0 && x.borders[0].oppFace == 6
	      && x.borders[1].oppUid == OPP_HIGH && x.borders[1].face == 0 && x.borders[1].oppFace == 2
	      && x.borders[2].oppUid == OPP_HIGH && x.borders[2].face == 1 && x.borders[2].oppFace == 7,
	      "live: borders are sorted by opposite uid and face");

	Check(x.borders[0].from == 0 && x.borders[1].from == 0 && x.borders[2].from == 1
	      && x.nodes[0].firstBorder == 0 && x.nodes[0].borderCount == 2
	      && x.nodes[1].firstBorder == 2 && x.nodes[1].borderCount == 1 && x.nodes[2].borderCount == 0
	      && x.nodeBorders[0] == 0 && x.nodeBorders[1] == 1 && x.nodeBorders[2] == 2 && x.b.arcsTrunc == 0,
	      "live: each node lists its borders in border order");
}

// Face data on the original mesh, striding 1: faces 0, 1 and 3 are water (word 3), face 2 dry. Face 2
// is moved to node 1 and the owned vertex to (6, 1, 6), so node 1 holds face 1 (vertices (2,2),
// (0,2), (6,6) in x/z: area 4) and face 2 (vertices (0,0), (2,0), (2,2): area 2). Node 0's one face
// is water: 255. Node 1: 4 of 6 by area, 4 / 6 * 255 + 0.5 = 170.5 -> 170 (by count it would be
// 128). Node 2 has no face: 0.
static void CheckWater()
{
	World w;
	Build(w, 3);
	SetFace(w.faces, 2, 0, 3, 1);
	PutVec(w.owned, 0, 6.0f, 1.0f, 6.0f);
	Bytes faceData(4 * 4, 0);
	PutInt(faceData, 0, 3);
	PutInt(faceData, 4, 3);
	PutInt(faceData, 8, 0);
	PutInt(faceData, 12, 3);
	PutArray(w.mesh, LIVE_NM_FACE_DATA, faceData, 4);
	PutInt(w.mesh, LIVE_NM_FACE_DATA_STRIDING, 1);
	Buffer x;
	MakeBuffer(x, 8, 8, 8);
	CgLiveCounts c;
	CgLiveResult r = Copy(w, x, &c);
	Check(r == CGL_OK && x.nodes[0].water == 255 && x.nodes[1].faces == 2 && x.nodes[1].water == 170
	      && x.nodes[2].water == 0 && c.waterNodes == 2 && c.noFaceData == 0,
	      "live: a node's water is the area share of its FaceData-3 faces");

	// No array (the base world), a striding of 0, and an array two words short of the four faces.
	World none;
	Build(none, 3);
	Buffer y;
	MakeBuffer(y, 8, 8, 8);
	CgLiveCounts cn;
	bool noArray = Copy(none, y, &cn) == CGL_OK && y.nodes[0].water == 0 && y.nodes[1].water == 0
	            && cn.noFaceData == 1 && cn.waterNodes == 0;
	PutInt(w.mesh, LIVE_NM_FACE_DATA_STRIDING, 0);
	CgLiveCounts cz;
	bool zeroStride = Copy(w, x, &cz) == CGL_OK && x.nodes[0].water == 0 && x.nodes[1].water == 0
	               && cz.noFaceData == 1 && cz.waterNodes == 0;
	PutInt(w.mesh, LIVE_NM_FACE_DATA_STRIDING, 1);
	PutArray(w.mesh, LIVE_NM_FACE_DATA, faceData, 2);
	CgLiveCounts cs;
	bool shortArray = Copy(w, x, &cs) == CGL_OK && x.nodes[0].water == 0 && x.nodes[1].water == 0
	               && cs.noFaceData == 1 && cs.waterNodes == 0;
	Check(noArray && zeroStride && shortArray, "live: a mesh without face data reads dry and counts noFaceData");
}

static void CheckRefusals()
{
	World w;
	Build(w, 3);
	Buffer x;
	MakeBuffer(x, 8, 8, 8);
	CgLiveCounts c;

	PutInt(w.gi, OFF_GI_SECTION, 3);
	CgLiveResult past = Copy(w, x, &c);
	int pastSlot = c.slot;
	PutInt(w.gi, OFF_GI_SECTION, -1);
	CgLiveResult negative = Copy(w, x, &c);
	Check(past == CGL_BAD_SLOT && pastSlot == 3 && negative == CGL_BAD_SLOT, "live: a slot past the count is refused");
	PutInt(w.gi, OFF_GI_SECTION, SLOT);

	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO, NULL);
	CgLiveResult noInstance = Copy(w, x, &c);
	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO, &w.nmi[0]);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_MESH, NULL);
	CgLiveResult noMesh = Copy(w, x, &c);
	PutPtr(w.nmi, OFF_NMI_ORIGINAL_MESH, &w.mesh[0]);
	Check(noInstance == CGL_NO_MESH && noMesh == CGL_NO_MESH, "live: a NULL mesh instance is refused");

	Check(Copy(w, x, &c) == CGL_OK, "live: the restored world copies again");
}

// CG_LIVE_MAX_NODES + 1 nodes, each with no edge and at the origin, in a buffer of as many nodes.
static void CheckOverNodes()
{
	World w;
	Build(w, CG_LIVE_MAX_NODES + 1);
	Buffer x;
	MakeBuffer(x, CG_LIVE_MAX_NODES + 1, 8, 8);
	CgLiveCounts c;
	Check(Copy(w, x, &c) == CGL_OVER_NODES, "live: a section over the node capacity is skipped and counted");
}

// CG_LIVE_MAX_BORDERS + 1 good connections (face 0, edge 0) in set 0 and none elsewhere, in a
// buffer of as many.
static void CheckOverBorders()
{
	World w;
	Build(w, 3);
	w.conns[0].assign((size_t)(CG_LIVE_MAX_BORDERS + 1) * 16, 0);
	for (int j = 0; j <= CG_LIVE_MAX_BORDERS; ++j)
		SetConnection(w.conns[0], j, 0, 0, j, 0);
	PutInt(w.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_THIS_UID, OTHER_UID);   // set 2 adds nothing
	Link(w, 3, 4, 4, 6);
	Buffer x;
	MakeBuffer(x, 8, 8, CG_LIVE_MAX_BORDERS + 1);
	CgLiveCounts c;
	Check(Copy(w, x, &c) == CGL_OVER_BORDERS, "live: a section over the border capacity is skipped");
}

// One node run of CG_LIVE_MAX_ARCS + 1 edges to node 1, in a buffer of as many arcs.
static void CheckOverArcs()
{
	World w;
	Build(w, 3);
	w.gedges.assign((size_t)(CG_LIVE_MAX_ARCS + 1) * 8, 0);
	for (int e = 0; e <= CG_LIVE_MAX_ARCS; ++e)
		SetGraphEdge(w.gedges, e, 0x3F80, 1);
	PutInt(w.gnodes, 4, CG_LIVE_MAX_ARCS + 1);
	PutInt(w.gnodes, 8, 0);
	PutInt(w.gnodes, 12, 0);
	PutInt(w.gnodes, 16, 0);
	Link(w, 3, CG_LIVE_MAX_ARCS + 1, 4, 6);
	Buffer x;
	MakeBuffer(x, 8, CG_LIVE_MAX_ARCS + 1, 8);
	CgLiveCounts c;
	Check(Copy(w, x, &c) == CGL_OVER_ARCS, "live: a section over the arc capacity is skipped");
}

// Node 0 with one intra arc and CG_NODE_ARCS_MAX good borders: one node past the report cap.
static void CheckArcsTrunc()
{
	World w;
	Build(w, 3);
	w.conns[0].assign((size_t)CG_NODE_ARCS_MAX * 16, 0);
	for (int j = 0; j < CG_NODE_ARCS_MAX; ++j)
		SetConnection(w.conns[0], j, 0, 0, j, 0);
	Link(w, 3, 4, 4, 6);
	Buffer x;
	MakeBuffer(x, 8, 8, CG_NODE_ARCS_MAX + 4);
	CgLiveCounts c;
	CgLiveResult r = Copy(w, x, &c);
	Check(r == CGL_OK && x.nodes[0].arcCount + x.nodes[0].borderCount == CG_NODE_ARCS_MAX + 2 && x.b.arcsTrunc == 1,
	      "live: a node over the arc report cap counts in arcsTrunc");
}

// Edge 0 names a vertex past both halves: face 0 counts nowhere and its border over edge 0 is
// skipped.
static void CheckVertexBound()
{
	World w;
	Build(w, 3);
	SetEdge(w.edges, 0, 5, 1);
	Buffer x;
	MakeBuffer(x, 8, 8, 8);
	CgLiveCounts c;
	CgLiveResult r = Copy(w, x, &c);
	Check(r == CGL_OK && x.nodes[0].faces == 0 && c.faces == 1 && x.b.borderCount == 2 && c.bordersSkipped == 3,
	      "live: a vertex past both halves fails its face and its border");
}

static void CheckCollectionReads()
{
	World w;
	Build(w, 3);
	Check(CgCollectionCount(&w.coll[0]) == 3 && CgCollectionCount(NULL) == 0, "reads: the collection's count");
	Check(CgCollectionMeshInstance(&w.coll[0], SLOT) == &w.nmi[0] && CgCollectionMeshInstance(&w.coll[0], 3) == NULL
	      && CgCollectionMeshInstance(&w.coll[0], -1) == NULL && CgCollectionMeshInstance(&w.coll[0], 0) == NULL,
	      "reads: a slot's mesh instance, NULL out of range");
	Check(CgMeshInstanceUid(&w.nmi[0]) == UID && CgMeshInstanceUid(NULL) == 0, "reads: a mesh instance's uid");
}

// The face map [-1, 0, -1, -1] with one instanced face (cluster 9); one owned face (cluster 5).
static void CheckFaceCluster()
{
	World w;
	Build(w, 3);
	Bytes map(16, 0), inst(16, 0), ownedFaces(16, 0);
	PutInt(map, 0, -1);
	PutInt(map, 4, 0);
	PutInt(map, 8, -1);
	PutInt(map, 12, -1);
	SetFace(inst, 0, 0, 3, 9);
	SetFace(ownedFaces, 0, 0, 3, 5);
	PutArray(w.nmi, OFF_NMI_FACE_MAP, map, 4);
	PutArray(w.nmi, OFF_NMI_INSTANCED_FACES, inst, 1);
	PutArray(w.nmi, OFF_NMI_OWNED_FACES, ownedFaces, 1);
	const void* m = &w.nmi[0];
	Check(CgInstanceFaceCluster(m, 1) == 9 && CgInstanceFaceCluster(m, 0) == 0 && CgInstanceFaceCluster(m, 2) == 7,
	      "face: an instanced face reads through the face map");
	Check(CgInstanceFaceCluster(m, 4) == 5 && CgInstanceFaceCluster(m, 5) == -1 && CgInstanceFaceCluster(NULL, 0) == -1,
	      "face: an owned face reads past the original count");
	PutInt(map, 4, 1);
	Check(CgInstanceFaceCluster(m, 1) == -1, "face: a map entry past the instanced array is out of range");
}

// The copy through the store: a pool buffer, its record, and the frame step's promotion into the
// section's over block. Counters are read as deltas.
static void CheckThroughStore()
{
	CgStoreDestroy();
	bool created = CgStoreCreate();
	World w;
	Build(w, 3);
	CgBlock* buf = CgLiveAcquire();
	CgLiveCounts c;
	CgLiveResult r = (created && buf) ? CgLiveCopy(&w.gi[0], &w.coll[0], w.shift, buf, &c) : CGL_NO_MESH;
	CgStats before;
	CgStatsGet(&before);
	if (r == CGL_OK)
		CgLivePost(c.slot, buf);
	else if (buf)
		CgLiveRelease(buf);
	int published = CgPromoteLive(8);
	CgStats after;
	CgStatsGet(&after);
	CgView v;
	bool read = CgRead(CgExteriorIndex(5, 19), &v);
	Check(r == CGL_OK && published == 1 && after.promoted - before.promoted == 1 && read
	      && v.block->source == CG_LIVE && v.block->uid == UID && v.block->collSlot == SLOT
	      && v.block->nodeCount == 3 && v.block->borderCount == 3 && v.block->arcCount == 2
	      && NearV(v.block->nodes[0].centre, 730.0f, 70.0f, 2390.0f),
	      "live: a copied buffer promotes into its section's over block");
	CgStoreDestroy();
}

int main()
{
	CheckBaseWorld();
	CheckWater();
	CheckRefusals();
	CheckOverNodes();
	CheckOverBorders();
	CheckOverArcs();
	CheckArcsTrunc();
	CheckVertexBound();
	CheckCollectionReads();
	CheckFaceCluster();
	CheckThroughStore();
	return CheckExit("coarse_graph_live_units");
}
