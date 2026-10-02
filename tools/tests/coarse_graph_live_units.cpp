// The live overlay's walk over fabricated instances at the real offsets: a three-slot collection,
// a graph instance in slot 2 rotated 90 degrees about y and translated, a mesh instance translated,
// its original mesh with three streaming sets, and a non-zero world shift. Every expected world
// value below is worked by hand from the fabricated numbers. Each over-capacity row allocates its
// section and its buffer one element past the capacity, so a dropped bound prints the row's FAIL
// line instead of writing past an allocation. One row runs a copy through the store's own pool,
// record and promotion. The stitch rows add a second section to the collection, rewrite the
// exterior's streaming sets as the game's stitch does, and run the registration's comparison and
// re-copy through the store into the cross resolution. One row packs the first section of a shipped
// tile into the live layout.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "planner/coarse_graph_live.h"
#include "base/hash.h"
#include "planner/tagfile_reader.h"

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

// A second section in the collection: a graph instance of two nodes with no arcs, a mesh instance
// in the exterior's frame, its mesh with one streaming set. Face 0 (cluster 0) and face 1 (cluster
// 1) both run edges 0-2 over vertices (0,0,2), (2,0,2), (1,0,3); edge 0 lies on the exterior's
// edge 3, reversed.
struct Side
{
	Bytes gi, nmi, mesh, gnodes, gpos, faces, edges, verts, sets, conn;
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

	// No array with a striding of 1, and an array two words short of the four faces.
	World none;
	Build(none, 3);
	PutInt(none.mesh, LIVE_NM_FACE_DATA_STRIDING, 1);
	Buffer y;
	MakeBuffer(y, 8, 8, 8);
	CgLiveCounts cn;
	bool noArray = Copy(none, y, &cn) == CGL_OK && y.nodes[0].water == 0 && y.nodes[1].water == 0
	            && cn.noFaceData == 1 && cn.zeroStride == 0 && cn.waterNodes == 0;
	PutArray(w.mesh, LIVE_NM_FACE_DATA, faceData, 2);
	CgLiveCounts cs;
	bool shortArray = Copy(w, x, &cs) == CGL_OK && x.nodes[0].water == 0 && x.nodes[1].water == 0
	               && cs.noFaceData == 1 && cs.zeroStride == 0 && cs.waterNodes == 0;
	Check(noArray && shortArray, "live: a mesh without face data reads dry and counts noFaceData");

	// The four-word array with a striding of 0: Havok's mesh without face data.
	PutArray(w.mesh, LIVE_NM_FACE_DATA, faceData, 4);
	PutInt(w.mesh, LIVE_NM_FACE_DATA_STRIDING, 0);
	CgLiveCounts cz;
	bool zeroStride = Copy(w, x, &cz) == CGL_OK && x.nodes[0].water == 0 && x.nodes[1].water == 0
	               && cz.zeroStride == 1 && cz.noFaceData == 0 && cz.waterNodes == 0;
	Check(zeroStride, "live: a zero-striding mesh reads dry and counts zeroStride, not noFaceData");
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

// ---- The stitch comparison ------------------------------------------------------------------------

static const int INTERIOR = 0x60499;

static int IntOf(const Bytes& b, size_t off)
{
	int v;
	memcpy(&v, &b[off], 4);
	return v;
}

// Points the exterior mesh at w.conns.size() sets of the given this-side and opposite uids.
static void SetExteriorSets(World& w, const int* thisUid, const int* oppUid)
{
	w.sets.assign(w.conns.size() * LIVE_SET_STRIDE, 0);
	for (size_t k = 0; k < w.conns.size(); ++k)
	{
		PutInt(w.sets, k * LIVE_SET_STRIDE + LIVE_SET_THIS_UID, thisUid[k]);
		PutInt(w.sets, k * LIVE_SET_STRIDE + LIVE_SET_OPP_UID, oppUid[k]);
	}
	Link(w, 3, 4, 4, 6);
}

// Builds side `x` of uid `uid` with one set toward oppUid holding (face, edge, oppFace, oppEdge),
// and places it in collection slot `slot` of w.
static void BuildSide(World& w, Side& x, int slot, int uid, int oppUid, int face, int edge, int oppFace, int oppEdge)
{
	x.gi.assign(272, 0);
	x.gnodes.assign(2 * 8, 0);
	x.gpos.assign(2 * 16, 0);
	PutInt(x.gi, OFF_GI_SECTION, slot);
	PutVec(x.gi, OFF_GI_ROW0, 1.0f, 0.0f, 0.0f);
	PutVec(x.gi, OFF_GI_ROW0 + 16, 0.0f, 1.0f, 0.0f);
	PutVec(x.gi, OFF_GI_ROW0 + 32, 0.0f, 0.0f, 1.0f);
	PutVec(x.gi, OFF_GI_TRANSLATION, 1000.0f, 0.0f, 2000.0f);
	PutVec(x.gpos, 0, 1.0f, 0.0f, 2.5f);
	PutVec(x.gpos, 16, 1.0f, 0.0f, 2.8f);
	PutPtr(x.gi, LIVE_GI_ORIGINAL_NODES, &x.gnodes[0]);
	PutInt(x.gi, LIVE_GI_NUM_ORIGINAL_NODES, 2);
	PutPtr(x.gi, OFF_GI_POSITIONS, &x.gpos[0]);
	x.nmi.assign(0x1B0, 0);
	x.mesh.assign(0xB0, 0);
	x.faces.assign(2 * 16, 0);
	x.edges.assign(3 * 20, 0);
	x.verts.assign(3 * 16, 0);
	PutInt(x.nmi, OFF_NMI_SECTION_UID, uid);
	PutVec(x.nmi, LIVE_NMI_FRAME_COL0, 1.0f, 0.0f, 0.0f);
	PutVec(x.nmi, LIVE_NMI_FRAME_COL1, 0.0f, 1.0f, 0.0f);
	PutVec(x.nmi, LIVE_NMI_FRAME_COL2, 0.0f, 0.0f, 1.0f);
	PutVec(x.nmi, LIVE_NMI_FRAME_TRANSLATION, 1000.0f, 0.0f, 2000.0f);
	PutVec(x.verts, 0, 0.0f, 0.0f, 2.0f);
	PutVec(x.verts, 16, 2.0f, 0.0f, 2.0f);
	PutVec(x.verts, 32, 1.0f, 0.0f, 3.0f);
	SetEdge(x.edges, 0, 0, 1);
	SetEdge(x.edges, 1, 1, 2);
	SetEdge(x.edges, 2, 2, 0);
	SetFace(x.faces, 0, 0, 3, 0);
	SetFace(x.faces, 1, 0, 3, 1);
	PutPtr(x.nmi, OFF_NMI_ORIGINAL_FACES, &x.faces[0]);
	PutInt(x.nmi, OFF_NMI_NUM_ORIGINAL_FACES, 2);
	PutPtr(x.nmi, OFF_NMI_ORIGINAL_EDGES, &x.edges[0]);
	PutInt(x.nmi, OFF_NMI_NUM_ORIGINAL_EDGES, 3);
	PutPtr(x.nmi, OFF_NMI_ORIGINAL_VERTICES, &x.verts[0]);
	PutInt(x.nmi, OFF_NMI_NUM_ORIGINAL_VERTICES, 3);
	PutPtr(x.nmi, OFF_NMI_ORIGINAL_MESH, &x.mesh[0]);
	x.conn.assign(16, 0);
	SetConnection(x.conn, 0, face, edge, oppFace, oppEdge);
	x.sets.assign(LIVE_SET_STRIDE, 0);
	PutInt(x.sets, LIVE_SET_THIS_UID, uid);
	PutInt(x.sets, LIVE_SET_OPP_UID, oppUid);
	PutArray(x.sets, LIVE_SET_CONNECTIONS, x.conn, 1);
	PutArray(x.mesh, LIVE_NM_STREAMING_SETS, x.sets, 1);
	PutPtr(w.infos, (size_t)slot * SIZE_CCC_INSTANCE_INFO, &x.nmi[0]);
	PutPtr(w.infos, (size_t)slot * SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH, &x.gi[0]);
}

static void CheckSignature()
{
	World w;
	Build(w, 3);
	unsigned base = CgLiveSetSignature(&w.mesh[0], UID);
	Check(base == CgLiveSetSignature(&w.mesh[0], UID) && base != FNV1A32_OFFSET, "sig: the signature is repeatable");

	w.conns.push_back(Bytes(16, 0));
	SetConnection(w.conns[3], 0, 0, 0, 1, 1);
	int thisUid[4] = { UID, OTHER_UID, UID, UID };
	int oppUid[4] = { OPP_HIGH, 0x7777, OPP_LOW, INTERIOR };
	SetExteriorSets(w, thisUid, oppUid);
	Check(CgLiveSetSignature(&w.mesh[0], UID) != base, "sig: a set added after the copy moves the signature");

	World r;
	Build(r, 3);
	SetConnection(r.conns[0], 0, 1, 3, 8, 9);
	Check(CgLiveSetSignature(&r.mesh[0], UID) != base && IntOf(r.sets, LIVE_SET_CONNECTIONS + 8) == 4,
	      "sig: a rewritten connection moves the signature with the count unchanged");

	World o;
	Build(o, 3);
	SetConnection(o.conns[1], 0, 3, 3, 3, 3);
	Check(CgLiveSetSignature(&o.mesh[0], UID) == base, "sig: a set naming another uid leaves the signature unchanged");

	World e;
	Build(e, 3);
	PutInt(e.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_CONNECTIONS + 8, 0);
	unsigned empty = CgLiveSetSignature(&e.mesh[0], UID);
	PutPtr(e.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_CONNECTIONS, NULL);
	PutInt(e.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_CONNECTIONS + 8, 5);
	unsigned nullArray = CgLiveSetSignature(&e.mesh[0], UID);
	PutArray(e.sets, 2 * LIVE_SET_STRIDE + LIVE_SET_CONNECTIONS, e.conns[2], -3);
	unsigned negative = CgLiveSetSignature(&e.mesh[0], UID);
	Check(empty != base && nullArray == empty && negative == empty && CgLiveSetSignature(NULL, UID) == FNV1A32_OFFSET,
	      "sig: a NULL connection array or a negative count reads as empty");
}

static void CheckStitchReads()
{
	World w;
	Build(w, 3);
	Side x;
	BuildSide(w, x, 1, INTERIOR, UID, 0, 0, 1, 3);
	const void* coll = &w.coll[0];
	bool found = CgCollectionSlotOfUid(coll, UID) == SLOT && CgCollectionSlotOfUid(coll, INTERIOR) == 1
	          && CgCollectionSlotOfUid(coll, 0x7777) == -1;
	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH, NULL);
	bool noGraph = CgCollectionSlotOfUid(coll, UID) == -1;
	Check(found && noGraph, "reads: a uid's slot by the mesh-instance walk, -1 when absent or without a graph instance");
	PutPtr(w.infos, SLOT * SIZE_CCC_INSTANCE_INFO + OFF_CCC_INFO_GRAPH, &w.gi[0]);
	Check(CgCollectionGraphInstance(coll, SLOT) == &w.gi[0] && CgCollectionGraphInstance(coll, 1) == &x.gi[0]
	      && CgCollectionGraphInstance(coll, 0) == NULL && CgCollectionGraphInstance(coll, 3) == NULL
	      && CgCollectionGraphInstance(coll, -1) == NULL && CgCollectionGraphInstance(NULL, 1) == NULL,
	      "reads: a slot's graph instance, NULL out of range");
}

// The exterior (slot 2) names the interior (slot 1) through one extra set; the listing is taken
// from the exterior's mesh instance against the record table.
struct Listing { int n; int slot; unsigned sig; int checked; int overflow; };

static Listing List(World& w, const std::vector<CgLiveSig>& sigs, unsigned gen, int max)
{
	Listing l;
	int slots[CG_LIVE_REFRESH_MAX];
	unsigned got[CG_LIVE_REFRESH_MAX];
	l.n = CgLiveStaleNeighbours(&w.coll[0], &w.nmi[0], SLOT, &sigs[0], (int)sigs.size(), gen, slots, got, max,
	                            &l.checked, &l.overflow);
	l.slot = l.n > 0 ? slots[0] : -1;
	l.sig = l.n > 0 ? got[0] : 0;
	return l;
}

static void NameInterior(World& w, int sets)
{
	w.conns.assign((size_t)sets, Bytes(16, 0));
	std::vector<int> thisUid((size_t)sets, UID), oppUid((size_t)sets, INTERIOR);
	for (int k = 0; k < sets; ++k)
		SetConnection(w.conns[(size_t)k], 0, 1, 3, 0, 0);
	SetExteriorSets(w, &thisUid[0], &oppUid[0]);
}

static void CheckStaleListing()
{
	World w;
	Build(w, 3);
	Side x;
	BuildSide(w, x, 1, INTERIOR, UID, 0, 0, 1, 3);
	NameInterior(w, 1);
	std::vector<CgLiveSig> sigs((size_t)CG_LIVE_RECORDS);
	memset(&sigs[0], 0, sizeof(CgLiveSig) * sigs.size());
	unsigned current = CgLiveSetSignature(&x.mesh[0], INTERIOR);
	Listing a = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(a.n == 1 && a.slot == 1 && a.sig == current && a.checked == 1 && a.overflow == 0,
	      "refresh: an opposite with no record is listed");

	CgLiveSig rec = { 1, INTERIOR, 7u, current };
	sigs[1] = rec;
	Listing b = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(b.n == 0 && b.checked == 1, "refresh: an opposite whose signature matches its record is not listed");

	SetConnection(x.conn, 0, 0, 0, 2, 3);
	Listing c = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(c.n == 1 && c.slot == 1 && c.sig != current && c.checked == 1,
	      "refresh: an opposite whose set changed since its record is listed");
	SetConnection(x.conn, 0, 0, 0, 1, 3);

	Listing d = List(w, sigs, 8u, CG_LIVE_REFRESH_MAX);
	Check(d.n == 1 && d.slot == 1 && d.checked == 1, "refresh: a record from an older generation is listed");

	sigs[1].uid = 0x1405;
	Listing e = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(e.n == 1 && e.slot == 1 && e.checked == 1, "refresh: a record naming another uid is listed");
	sigs[1] = rec;

	NameInterior(w, 2);
	memset(&sigs[0], 0, sizeof(CgLiveSig) * sigs.size());
	Listing f = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(f.n == 1 && f.slot == 1 && f.checked == 1, "refresh: an opposite named twice is listed once");

	int thisUid[1] = { UID };
	int oppUid[1] = { 0x7777 };
	w.conns.assign(1, Bytes(16, 0));
	SetExteriorSets(w, thisUid, oppUid);
	Listing g = List(w, sigs, 7u, CG_LIVE_REFRESH_MAX);
	Check(g.n == 0 && g.checked == 0 && g.overflow == 0, "refresh: an unregistered opposite is neither listed nor checked");

	Side y;
	BuildSide(w, y, 0, 0x1405, UID, 0, 0, 1, 3);
	int thisTwo[2] = { UID, UID };
	int oppTwo[2] = { INTERIOR, 0x1405 };
	w.conns.assign(2, Bytes(16, 0));
	SetExteriorSets(w, thisTwo, oppTwo);
	Listing h = List(w, sigs, 7u, 1);
	Check(h.n == 1 && h.slot == 1 && h.checked == 1 && h.overflow == 1,
	      "refresh: opposites past the cap count in overflow and are not listed");
}

// The site's registration on the fabricated collection: the registering copy posted with its
// slot's record, then the comparison from the registering mesh and a re-copy of each slot it lists,
// posted only when the slot's signature held across the copy.
static void Register(World& w, const void* gi, std::vector<CgLiveSig>& sigs)
{
	const void* coll = &w.coll[0];
	int slot = *(const int*)((const unsigned char*)gi + OFF_GI_SECTION);
	int uid = 0;
	unsigned before = 0;
	CgBlock* buf = CgLiveAcquire();
	CgLiveCounts c;
	if (buf && CgLiveSlotSignature(coll, slot, &uid, &before) && CgLiveCopy(gi, coll, w.shift, buf, &c) == CGL_OK)
	{
		CgLiveSig rec = { 1, uid, buf->storeGen, before };
		CgLivePost(c.slot, buf);
		sigs[(size_t)slot] = rec;
	}
	else if (buf)
		CgLiveRelease(buf);
	int slots[CG_LIVE_REFRESH_MAX];
	unsigned got[CG_LIVE_REFRESH_MAX];
	int checked = 0, overflow = 0;
	int n = CgLiveStaleNeighbours(coll, CgCollectionMeshInstance(coll, slot), slot, &sigs[0], (int)sigs.size(),
	                              CgStoreGen(), slots, got, CG_LIVE_REFRESH_MAX, &checked, &overflow);
	for (int i = 0; i < n; ++i)
	{
		CgBlock* re = CgLiveAcquire();
		unsigned b0 = 0, b1 = 0;
		int u0 = 0, u1 = 0;
		bool ok = re && CgLiveSlotSignature(coll, slots[i], &u0, &b0)
		       && CgLiveCopy(CgCollectionGraphInstance(coll, slots[i]), coll, w.shift, re, &c) == CGL_OK
		       && CgLiveSlotSignature(coll, slots[i], &u1, &b1) && b0 == b1;
		if (ok)
		{
			CgLiveSig rec = { 1, u0, re->storeGen, b0 };
			CgLivePost(c.slot, re);
			sigs[(size_t)slots[i]] = rec;
		}
		else if (re)
			CgLiveRelease(re);
	}
}

static const CgBlock* StoreNeighbour(void*, int uid, int* dirOut)
{
	int dir = CgIndexOfUid(uid);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return NULL;
	*dirOut = dir;
	return v.block;
}

// Whether the store's block of `from` resolves node `node` to node `to` of `toUid`, and that the
// call counted no one-sided or dropped border.
static bool ResolvesTo(int from, int node, int toUid, int to)
{
	int dir = CgIndexOfUid(from);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return false;
	CgStats s0;
	CgStatsGet(&s0);
	CgResolved out[8];
	int n = CgCrossArcs(v.block, node, StoreNeighbour, NULL, out, 8);
	CgStats s1;
	CgStatsGet(&s1);
	bool hit = false;
	for (int i = 0; i < n; ++i)
		hit = hit || (out[i].dirIndex == CgIndexOfUid(toUid) && out[i].node == to);
	return hit && s1.crossOneSided == s0.crossOneSided && s1.crossDropped == s0.crossDropped;
}

static bool HasArcTo(int from, int node, int toUid)
{
	int dir = CgIndexOfUid(from);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return false;
	CgResolved out[8];
	int n = CgCrossArcs(v.block, node, StoreNeighbour, NULL, out, 8);
	bool hit = false;
	for (int i = 0; i < n; ++i)
		hit = hit || out[i].dirIndex == CgIndexOfUid(toUid);
	return hit;
}

// One stitch shape: the exterior registers with `before` in its set toward the partner (or no such
// set when before < 0), the partner's stitch rewrites that set to (1, 3, 0, 0), then the partner
// registers in slot 1 with its set (0, 0, 1, 3) toward the exterior.
static bool StitchResolves(int partner, int before, bool* arcBeforeRecopy)
{
	CgStoreDestroy();
	bool created = CgStoreCreate();
	World w;
	Build(w, 3);
	std::vector<CgLiveSig> sigs((size_t)CG_LIVE_RECORDS);
	memset(&sigs[0], 0, sizeof(CgLiveSig) * sigs.size());
	if (before >= 0)
	{
		NameInterior(w, 1);
		SetConnection(w.conns[0], 0, 1, 3, before, 0);
		int thisUid[1] = { UID };
		int oppUid[1] = { partner };
		SetExteriorSets(w, thisUid, oppUid);
	}
	Register(w, &w.gi[0], sigs);
	CgPromoteLive(CG_LIVE_RECORDS);
	int thisUid[1] = { UID };
	int oppUid[1] = { partner };
	w.conns.assign(1, Bytes(16, 0));
	SetConnection(w.conns[0], 0, 1, 3, 0, 0);
	SetExteriorSets(w, thisUid, oppUid);
	Side x;
	BuildSide(w, x, 1, partner, UID, 0, 0, 1, 3);
	if (arcBeforeRecopy)
	{
		CgBlock* buf = CgLiveAcquire();
		CgLiveCounts c;
		if (buf && CgLiveCopy(&x.gi[0], &w.coll[0], w.shift, buf, &c) == CGL_OK)
			CgLivePost(c.slot, buf);
		else if (buf)
			CgLiveRelease(buf);
		CgPromoteLive(CG_LIVE_RECORDS);
		*arcBeforeRecopy = HasArcTo(UID, 1, partner);
	}
	Register(w, &x.gi[0], sigs);
	CgPromoteLive(CG_LIVE_RECORDS);
	bool both = created && ResolvesTo(UID, 1, partner, 0) && ResolvesTo(partner, 0, UID, 1);
	CgStoreDestroy();
	return both;
}

static void CheckStitchRefresh()
{
	bool arcBefore = true;
	StitchResolves(INTERIOR, -1, &arcBefore);
	Check(!arcBefore, "refresh: before the re-copy the exterior has no arc into an interior stitched after its copy");
	Check(StitchResolves(INTERIOR, -1, NULL), "refresh: an interior stitched in after its exterior's copy resolves both ways after the re-copy");
	Check(StitchResolves(INTERIOR, 7, NULL),
	      "refresh: an exterior whose door set was refilled for a regenerated interior resolves after the re-copy");
	Check(StitchResolves(0x1405, 7, NULL), "refresh: a neighbour of a regenerated exterior resolves toward it after the re-copy");
}

// ---- A shipped tile ------------------------------------------------------------------------------

static std::vector<unsigned char> ReadBytes(const char* path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open())
		return std::vector<unsigned char>();
	return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static __int64 IntField(const TfDoc& d, int st, const char* name)
{
	__int64 v = 0;
	return TfAsInt(d, TfFind(d, st, name), &v) ? v : 0;
}

static int ArrayOf(const TfDoc& d, int st, const char* name, int* count)
{
	int a = TfFind(d, st, name);
	int n = TfArrayCount(d, a);
	*count = n < 0 ? 0 : n;
	return a;
}

// The first NavMesh entry of the tile's root and the first Graph after it, as the extractor groups
// them; -1 when either is missing.
static bool FirstSection(const TfDoc& d, int* mesh, int* graph)
{
	*mesh = *graph = -1;
	int count = 0;
	int variants = ArrayOf(d, TfRoot(d), "namedVariants", &count);
	for (int k = 0; k < count; ++k)
	{
		int e = TfArrayAt(d, variants, k);
		const char* name = TfAsString(d, TfFind(d, e, "name"));
		if (*mesh < 0 && strcmp(name, "NavMesh") == 0)
			*mesh = TfDeref(d, TfFind(d, e, "variant"));
		else if (*mesh >= 0 && strcmp(name, "Graph") == 0)
		{
			*graph = TfDeref(d, TfFind(d, e, "variant"));
			break;
		}
	}
	return *mesh >= 0 && *graph >= 0;
}

// tile63.28's first section packed into the live layout at slot 2 with identity frames and no
// shift: its faces (start, count, cluster), edges, vertices, face data and striding, no streaming
// set, and a graph instance of its node count with no arcs. *nodes receives the node count.
static bool PackTile(World& w, Bytes& faceData, int* nodes)
{
	std::vector<unsigned char> bytes = ReadBytes("tools/tests/fixtures/planner/tile63.28.hkt");
	if (bytes.empty())
		return false;
	TfDoc d;
	TfResult r = TfParse(&bytes[0], bytes.size(), &d);
	int mesh = -1, graph = -1;
	if ((r != TF_OK && r != TF_TAIL_STOP) || !FirstSection(d, &mesh, &graph))
		return false;
	int nf = 0, ne = 0, nv = 0, nd = 0, nn = 0;
	int faces = ArrayOf(d, mesh, "faces", &nf);
	int edges = ArrayOf(d, mesh, "edges", &ne);
	int verts = ArrayOf(d, mesh, "vertices", &nv);
	int data = ArrayOf(d, mesh, "faceData", &nd);
	ArrayOf(d, graph, "nodes", &nn);
	if (nf <= 0 || ne <= 0 || nv <= 0 || nn <= 0 || nn > CG_LIVE_MAX_NODES)
		return false;
	Build(w, 3);
	w.faces.assign((size_t)nf * 16, 0);
	w.edges.assign((size_t)ne * 20, 0);
	w.verts.assign((size_t)nv * 16, 0);
	for (int f = 0; f < nf; ++f)
	{
		int st = TfArrayAt(d, faces, f);
		SetFace(w.faces, f, (int)IntField(d, st, "startEdgeIndex"), (short)IntField(d, st, "numEdges"),
		        (short)IntField(d, st, "clusterIndex"));
	}
	for (int e = 0; e < ne; ++e)
	{
		int st = TfArrayAt(d, edges, e);
		SetEdge(w.edges, e, (int)IntField(d, st, "a"), (int)IntField(d, st, "b"));
	}
	for (int v = 0; v < nv; ++v)
	{
		float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		TfAsVec(d, TfArrayAt(d, verts, v), c, 4);
		PutVec(w.verts, (size_t)v * 16, c[0], c[1], c[2]);
	}
	faceData.assign((size_t)(nd > 0 ? nd : 1) * 4, 0);
	for (int k = 0; k < nd; ++k)
	{
		__int64 word = 0;
		TfAsInt(d, TfArrayAt(d, data, k), &word);
		PutInt(faceData, (size_t)k * 4, (int)word);
	}
	w.gnodes.assign((size_t)nn * 8, 0);
	w.gpos.assign((size_t)nn * 16, 0);
	PutVec(w.gi, OFF_GI_ROW0, 1.0f, 0.0f, 0.0f);
	PutVec(w.gi, OFF_GI_ROW0 + 16, 0.0f, 1.0f, 0.0f);
	PutVec(w.gi, OFF_GI_ROW0 + 32, 0.0f, 0.0f, 1.0f);
	PutVec(w.gi, OFF_GI_TRANSLATION, 0.0f, 0.0f, 0.0f);
	PutVec(w.nmi, LIVE_NMI_FRAME_TRANSLATION, 0.0f, 0.0f, 0.0f);
	w.conns.clear();
	w.sets.clear();
	Link(w, nn, 0, nf, ne);
	PutInt(w.nmi, OFF_NMI_NUM_ORIGINAL_VERTICES, nv);
	PutArray(w.nmi, OFF_NMI_OWNED_VERTICES, w.owned, 0);
	PutArray(w.mesh, LIVE_NM_STREAMING_SETS, w.sets, 0);
	PutArray(w.mesh, LIVE_NM_FACE_DATA, faceData, nd);
	PutInt(w.mesh, LIVE_NM_FACE_DATA_STRIDING, (int)IntField(d, mesh, "faceDataStriding"));
	w.shift[0] = w.shift[1] = w.shift[2] = 0.0f;
	*nodes = nn;
	return true;
}

static void CheckShippedTile()
{
	World w;
	Bytes faceData;
	int nodes = 0;
	bool packed = PackTile(w, faceData, &nodes);
	Buffer x;
	MakeBuffer(x, packed ? nodes : 1, 8, 8);
	CgLiveCounts c;
	CgLiveResult r = packed ? Copy(w, x, &c) : CGL_NO_MESH;
	if (r == CGL_OK && nodes == 3)
		printf("live: tile63.28 water bytes %d, %d, %d (node 0: %d faces)\n", x.nodes[0].water, x.nodes[1].water,
		       x.nodes[2].water, x.nodes[0].faces);
	Check(packed && r == CGL_OK && nodes == 3 && x.nodes[0].water == 254 && x.nodes[1].water == 255
	      && x.nodes[2].water == 0 && x.nodes[0].faces == 106 && c.noFaceData == 0 && c.zeroStride == 0,
	      "live: tile63.28's first section copies water 254, 255 and 0 by area");
}
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
	CheckSignature();
	CheckStitchReads();
	CheckStaleListing();
	CheckStitchRefresh();
	CheckShippedTile();
	return CheckExit("coarse_graph_live_units");
}
