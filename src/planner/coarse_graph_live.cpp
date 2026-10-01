// coarse_graph_live.cpp - The live overlay's walk: one registering section's nodes, intra arcs,
// footprints and border connections, read from its graph instance, its mesh instance and the
// original mesh, written into a live buffer in world units. Pure over the memory it is handed:
// every index is bounded by its own array's count and every pointer NULL-checked before a read;
// no allocation, no lock, no log. Any thread; the caller keeps the instances stable.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/coarse_graph_live.h"
#include "planner/coarse_graph.h"

#include <float.h>
#include <string.h>

namespace planner {

namespace coarse_graph_live_detail {

// The mesh instance's arrays the walk reads, each count clamped to zero when negative and to zero
// when its pointer is NULL. frame holds the three columns and the translation, 16 bytes apart.
struct MeshView
{
	const unsigned char* faces;  int faceCount;
	const unsigned char* edges;  int edgeCount;
	const unsigned char* verts;  int vertCount;
	const unsigned char* owned;  int ownedCount;
	const float*         frame;
};

// The graph instance's original nodes ({startEdge, numEdges}), edges ({half cost, flags, target})
// and positions (16-byte stride), and its frame laid out as the mesh instance's.
struct GraphView
{
	const unsigned char* nodes;
	const unsigned char* edges;  int edgeCount;
	const unsigned char* positions;
	int                  nodeCount;
	const float*         frame;
};

} // namespace coarse_graph_live_detail
using namespace coarse_graph_live_detail;

static const size_t   EDGE_END             = 4;      // an edge record's end vertex
static const size_t   CONNECTION_STRIDE    = 16;     // face, edge, opposite face, opposite edge
static const size_t   GRAPH_NODE_STRIDE    = 8;
static const size_t   GRAPH_EDGE_STRIDE    = 8;
static const size_t   GRAPH_EDGE_TARGET    = 4;
static const size_t   POSITION_STRIDE      = 16;
static const size_t   INSTANCE_INFO_MESH   = 0;
static const unsigned TARGET_SECTION_SHIFT = 22;
static const unsigned TARGET_NODE_MASK     = 0x3FFFFFu;

static int IntAt(const void* p, size_t off)
{
	return *(const int*)((const unsigned char*)p + off);
}

static const unsigned char* PtrAt(const void* p, size_t off)
{
	return *(const unsigned char* const*)((const unsigned char*)p + off);
}

static int Count(const unsigned char* data, int n)
{
	return (data && n > 0) ? n : 0;
}

static MeshView LoadMesh(const void* meshInst)
{
	MeshView m;
	m.faces = PtrAt(meshInst, OFF_NMI_ORIGINAL_FACES);
	m.faceCount = Count(m.faces, IntAt(meshInst, OFF_NMI_NUM_ORIGINAL_FACES));
	m.edges = PtrAt(meshInst, OFF_NMI_ORIGINAL_EDGES);
	m.edgeCount = Count(m.edges, IntAt(meshInst, OFF_NMI_NUM_ORIGINAL_EDGES));
	m.verts = PtrAt(meshInst, OFF_NMI_ORIGINAL_VERTICES);
	m.vertCount = Count(m.verts, IntAt(meshInst, OFF_NMI_NUM_ORIGINAL_VERTICES));
	m.owned = PtrAt(meshInst, OFF_NMI_OWNED_VERTICES);
	m.ownedCount = Count(m.owned, IntAt(meshInst, OFF_NMI_OWNED_VERTICES + OFF_HKARRAY_SIZE));
	m.frame = (const float*)((const unsigned char*)meshInst + LIVE_NMI_FRAME_COL0);
	return m;
}

static GraphView LoadGraph(const void* graphInst, int n)
{
	GraphView g;
	g.nodes = PtrAt(graphInst, LIVE_GI_ORIGINAL_NODES);
	g.edges = PtrAt(graphInst, LIVE_GI_ORIGINAL_EDGES);
	g.edgeCount = Count(g.edges, IntAt(graphInst, OFF_GI_BASE_EDGE_COUNT));
	g.positions = PtrAt(graphInst, OFF_GI_POSITIONS);
	g.nodeCount = n;
	g.frame = (const float*)((const unsigned char*)graphInst + OFF_GI_ROW0);
	return g;
}

// v[0]*col0 + v[1]*col1 + v[2]*col2 + translation: a point in the shifted Havok frame.
static void HavokPoint(const float* frame, const float* v, float out[3])
{
	for (int k = 0; k < 3; ++k)
		out[k] = v[0] * frame[k] + v[1] * frame[4 + k] + v[2] * frame[8 + k] + frame[12 + k];
}

// Vertex v: an original vertex below the original count, an owned one past it; NULL out of range.
static const float* VertexAt(const MeshView& m, int v)
{
	if (v < 0)
		return NULL;
	if (v < m.vertCount)
		return (const float*)(m.verts + (size_t)v * LIVE_VERTEX_STRIDE);
	int o = v - m.vertCount;
	if (o < m.ownedCount)
		return (const float*)(m.owned + (size_t)o * LIVE_VERTEX_STRIDE);
	return NULL;
}

static int FaceCluster(const MeshView& m, int f)
{
	return *(const short*)(m.faces + (size_t)f * LIVE_FACE_STRIDE + LIVE_FACE_CLUSTER);
}

static float HalfToFloat(unsigned short half)
{
	unsigned bits = (unsigned)half << 16;
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

// Each node's world centre; its box starts empty and its counts at zero.
static void CopyCentres(const GraphView& g, const float shift[3], CgBlock* buf)
{
	for (int k = 0; k < g.nodeCount; ++k)
	{
		CgNode& node = buf->nodes[k];
		float h[3];
		HavokPoint(g.frame, (const float*)(g.positions + (size_t)k * POSITION_STRIDE), h);
		for (int ax = 0; ax < 3; ++ax)
		{
			node.centre[ax] = (h[ax] - shift[ax]) * HAVOK_TO_WORLD;
			node.boxMin[ax] = FLT_MAX;
			node.boxMax[ax] = -FLT_MAX;
		}
		node.faces = 0;
		node.firstArc = node.arcCount = 0;
		node.firstBorder = node.borderCount = 0;
	}
}

// Node k's original edges that stay inside the section and name another node. A run outside the
// edge array counts one skip; an edge to another section, past the nodes or to k itself, one each.
static CgLiveResult CopyArcs(const GraphView& g, CgBlock* buf, CgLiveCounts* counts)
{
	int arc = 0;
	for (int k = 0; k < g.nodeCount; ++k)
	{
		CgNode& node = buf->nodes[k];
		node.firstArc = arc;
		const unsigned char* rec = g.nodes + (size_t)k * GRAPH_NODE_STRIDE;
		int start = IntAt(rec, 0);
		int num = IntAt(rec, 4);
		if (start < 0 || num < 0 || (__int64)start + num > (__int64)g.edgeCount)
		{
			++counts->arcsSkipped;
			continue;
		}
		for (int i = 0; i < num; ++i)
		{
			const unsigned char* e = g.edges + (size_t)(start + i) * GRAPH_EDGE_STRIDE;
			unsigned target = *(const unsigned*)(e + GRAPH_EDGE_TARGET);
			unsigned to = target & TARGET_NODE_MASK;
			if ((target >> TARGET_SECTION_SHIFT) != 0 || to >= (unsigned)g.nodeCount || to == (unsigned)k)
			{
				++counts->arcsSkipped;
				continue;
			}
			if (arc >= CG_LIVE_MAX_ARCS)
				return CGL_OVER_ARCS;
			buf->arcs[arc].to = (int)to;
			buf->arcs[arc].cost = HalfToFloat(*(const unsigned short*)e) * HAVOK_TO_WORLD;
			++arc;
		}
		node.arcCount = arc - node.firstArc;
	}
	buf->arcCount = arc;
	return CGL_OK;
}

// Whether every edge of face f's run, and vertex a of each, is in range.
static bool FaceInRange(const MeshView& m, int start, int num)
{
	if (start < 0 || num < 0 || (__int64)start + num > (__int64)m.edgeCount)
		return false;
	for (int i = 0; i < num; ++i)
		if (!VertexAt(m, IntAt(m.edges + (size_t)(start + i) * LIVE_EDGE_STRIDE, OFF_EDGE_VERTEX_INDEX)))
			return false;
	return true;
}

// Each original face whose cluster names a node folds vertex a of each edge of its run into that
// node's box and face count; a face with any index out of range counts nowhere. A node left
// without a vertex takes its centre as its box.
static void CopyFootprints(const MeshView& m, const float shift[3], CgBlock* buf, CgLiveCounts* counts)
{
	for (int f = 0; f < m.faceCount; ++f)
	{
		const unsigned char* face = m.faces + (size_t)f * LIVE_FACE_STRIDE;
		int c = FaceCluster(m, f);
		int start = IntAt(face, OFF_FACE_START_EDGE);
		int num = *(const short*)(face + OFF_FACE_NUM_EDGES);
		if (c < 0 || c >= buf->nodeCount || !FaceInRange(m, start, num))
			continue;
		CgNode& node = buf->nodes[c];
		++node.faces;
		++counts->faces;
		for (int i = 0; i < num; ++i)
		{
			const float* v = VertexAt(m, IntAt(m.edges + (size_t)(start + i) * LIVE_EDGE_STRIDE, OFF_EDGE_VERTEX_INDEX));
			float h[3];
			HavokPoint(m.frame, v, h);
			for (int ax = 0; ax < 3; ++ax)
			{
				float w = (h[ax] - shift[ax]) * HAVOK_TO_WORLD;
				if (w < node.boxMin[ax]) node.boxMin[ax] = w;
				if (w > node.boxMax[ax]) node.boxMax[ax] = w;
			}
		}
	}
	for (int k = 0; k < buf->nodeCount; ++k)
	{
		CgNode& node = buf->nodes[k];
		if (node.boxMin[0] <= node.boxMax[0])
			continue;
		for (int ax = 0; ax < 3; ++ax)
			node.boxMin[ax] = node.boxMax[ax] = node.centre[ax];
	}
}

// One set's connections: each whose face, edge and the edge's two vertices are in range becomes a
// border in world units, its portal the edge's midpoint; any other counts in bordersSkipped.
static CgLiveResult CopySetBorders(const MeshView& m, const unsigned char* set, const float shift[3],
                                   CgBlock* buf, CgLiveCounts* counts)
{
	const unsigned char* conns = PtrAt(set, LIVE_SET_CONNECTIONS);
	int connCount = IntAt(set, LIVE_SET_CONNECTIONS + OFF_HKARRAY_SIZE);
	if (!conns)
	{
		if (connCount > 0)
			++counts->bordersSkipped;   // a NULL array with a count counts once
		return CGL_OK;
	}
	for (int j = 0; j < connCount; ++j)
	{
		const unsigned char* c = conns + (size_t)j * CONNECTION_STRIDE;
		int face = IntAt(c, 0);
		int edge = IntAt(c, 4);
		if (face < 0 || face >= m.faceCount)
		{
			++counts->bordersSkipped;
			continue;
		}
		const unsigned char* er = (edge >= 0 && edge < m.edgeCount) ? m.edges + (size_t)edge * LIVE_EDGE_STRIDE : NULL;
		const float* va = er ? VertexAt(m, IntAt(er, OFF_EDGE_VERTEX_INDEX)) : NULL;
		const float* vb = er ? VertexAt(m, IntAt(er, EDGE_END)) : NULL;
		if (!va || !vb)
		{
			++counts->bordersSkipped;
			continue;
		}
		if (buf->borderCount >= CG_LIVE_MAX_BORDERS)
			return CGL_OVER_BORDERS;
		CgBorder& br = buf->borders[buf->borderCount++];
		br.oppUid = IntAt(set, LIVE_SET_OPP_UID);
		br.face = face;
		br.oppFace = IntAt(c, 8);
		int cluster = FaceCluster(m, face);
		br.from = (cluster >= 0 && cluster < buf->nodeCount) ? cluster : -1;
		float ha[3], hb[3];
		HavokPoint(m.frame, va, ha);
		HavokPoint(m.frame, vb, hb);
		for (int ax = 0; ax < 3; ++ax)
		{
			br.a[ax] = (ha[ax] - shift[ax]) * HAVOK_TO_WORLD;
			br.b[ax] = (hb[ax] - shift[ax]) * HAVOK_TO_WORLD;
			br.portal[ax] = ((ha[ax] + hb[ax]) * 0.5f - shift[ax]) * HAVOK_TO_WORLD;
		}
	}
	return CGL_OK;
}

// The original mesh's streaming sets whose this-side uid is the section's (compared as unsigned).
static CgLiveResult CopyBorders(const MeshView& m, const void* mesh, int uid, const float shift[3],
                                CgBlock* buf, CgLiveCounts* counts)
{
	const unsigned char* sets = PtrAt(mesh, LIVE_NM_STREAMING_SETS);
	int setCount = IntAt(mesh, LIVE_NM_STREAMING_SETS + OFF_HKARRAY_SIZE);
	if (!sets)
	{
		if (setCount > 0)
			++counts->bordersSkipped;
		return CGL_OK;
	}
	for (int s = 0; s < setCount; ++s)
	{
		const unsigned char* set = sets + (size_t)s * LIVE_SET_STRIDE;
		if ((unsigned)IntAt(set, LIVE_SET_THIS_UID) != (unsigned)uid)
			continue;
		CgLiveResult r = CopySetBorders(m, set, shift, buf, counts);
		if (r != CGL_OK)
			return r;
	}
	return CGL_OK;
}

// The store's border order: oppUid, then face, both as signed ints.
static bool BorderBefore(const CgBorder& a, const CgBorder& b)
{
	if (a.oppUid != b.oppUid)
		return a.oppUid < b.oppUid;
	return a.face < b.face;
}

static void SortBorders(CgBorder* br, int n)
{
	for (int gap = n / 2; gap > 0; gap /= 2)
	{
		for (int i = gap; i < n; ++i)
		{
			CgBorder t = br[i];
			int j = i;
			while (j >= gap && BorderBefore(t, br[j - gap]))
			{
				br[j] = br[j - gap];
				j -= gap;
			}
			br[j] = t;
		}
	}
}

// Each node's border range in nodeBorders, in border order, and the nodes whose intra arcs plus
// borders exceed what a node reports to the search. A border with no node is listed nowhere.
static void IndexBorders(CgBlock* buf)
{
	for (int i = 0; i < buf->borderCount; ++i)
	{
		int f = buf->borders[i].from;
		if (f >= 0 && f < buf->nodeCount)
			++buf->nodes[f].borderCount;
	}
	int listed = 0;
	for (int k = 0; k < buf->nodeCount; ++k)
	{
		buf->nodes[k].firstBorder = listed;
		listed += buf->nodes[k].borderCount;
		buf->nodes[k].borderCount = 0;
	}
	for (int i = 0; i < buf->borderCount; ++i)
	{
		int f = buf->borders[i].from;
		if (f >= 0 && f < buf->nodeCount)
		{
			CgNode& node = buf->nodes[f];
			buf->nodeBorders[node.firstBorder + node.borderCount++] = i;
		}
	}
	for (int i = listed; i < buf->borderCount; ++i)
		buf->nodeBorders[i] = -1;
	buf->arcsTrunc = 0;
	for (int k = 0; k < buf->nodeCount; ++k)
		if (buf->nodes[k].arcCount + buf->nodes[k].borderCount > CG_NODE_ARCS_MAX)
			++buf->arcsTrunc;
}

// A NULL argument, or a graph instance with nodes but no node or position array, answers
// CGL_NO_MESH; a negative node count answers CGL_OVER_NODES (it is not a count the buffer holds).
CgLiveResult CgLiveCopy(const void* graphInst, const void* coll, const float shift[3],
                        CgBlock* buf, CgLiveCounts* counts)
{
	CgLiveCounts unused;
	if (!counts)
		counts = &unused;
	counts->slot = -1;
	counts->faces = counts->bordersSkipped = counts->arcsSkipped = 0;
	if (!graphInst || !coll || !shift || !buf)
		return CGL_NO_MESH;
	int slot = IntAt(graphInst, OFF_GI_SECTION);
	counts->slot = slot;
	if (slot < 0 || slot >= CgCollectionCount(coll))
		return CGL_BAD_SLOT;
	const void* meshInst = CgCollectionMeshInstance(coll, slot);
	const void* mesh = meshInst ? PtrAt(meshInst, OFF_NMI_ORIGINAL_MESH) : NULL;
	if (!meshInst || !mesh)
		return CGL_NO_MESH;
	int uid = CgMeshInstanceUid(meshInst);
	int n = IntAt(graphInst, LIVE_GI_NUM_ORIGINAL_NODES);
	if (n > CG_LIVE_MAX_NODES)
		return CGL_OVER_NODES;
	if (n < 0)
		return CGL_OVER_NODES;
	GraphView g = LoadGraph(graphInst, n);
	if (n > 0 && (!g.nodes || !g.positions))
		return CGL_NO_MESH;
	MeshView m = LoadMesh(meshInst);

	buf->nodeCount = n;
	buf->arcCount = buf->borderCount = 0;
	CopyCentres(g, shift, buf);
	CgLiveResult r = CopyArcs(g, buf, counts);
	if (r != CGL_OK)
		return r;
	CopyFootprints(m, shift, buf, counts);
	r = CopyBorders(m, mesh, uid, shift, buf, counts);
	if (r != CGL_OK)
		return r;
	SortBorders(buf->borders, buf->borderCount);
	IndexBorders(buf);
	buf->uid = uid;
	buf->source = CG_LIVE;
	return CGL_OK;
}

int CgCollectionCount(const void* coll)
{
	return coll ? IntAt(coll, OFF_CCC_INSTANCE_COUNT) : 0;
}

const void* CgCollectionMeshInstance(const void* coll, int slot)
{
	const unsigned char* infos = coll ? PtrAt(coll, OFF_CCC_INSTANCE_DATA) : NULL;
	if (!infos || slot < 0 || slot >= CgCollectionCount(coll))
		return NULL;
	return PtrAt(infos + (size_t)slot * SIZE_CCC_INSTANCE_INFO, INSTANCE_INFO_MESH);
}

int CgMeshInstanceUid(const void* meshInst)
{
	return meshInst ? IntAt(meshInst, OFF_NMI_SECTION_UID) : 0;
}

// The instance's own face lookup, bounded: a face past the original count is an owned face; below
// it, the face map names an instanced copy, or -1 for the original face; an empty map reads the
// instanced array at the face's own index.
int CgInstanceFaceCluster(const void* meshInst, unsigned face)
{
	if (!meshInst)
		return -1;
	const unsigned char* orig = PtrAt(meshInst, OFF_NMI_ORIGINAL_FACES);
	int numOrig = IntAt(meshInst, OFF_NMI_NUM_ORIGINAL_FACES);
	if (numOrig < 0)
		numOrig = 0;
	const unsigned char* rec = NULL;
	if (face >= (unsigned)numOrig)
	{
		const unsigned char* owned = PtrAt(meshInst, OFF_NMI_OWNED_FACES);
		unsigned o = face - (unsigned)numOrig;
		if (owned && o < (unsigned)Count(owned, IntAt(meshInst, OFF_NMI_OWNED_FACES + OFF_HKARRAY_SIZE)))
			rec = owned + (size_t)o * LIVE_FACE_STRIDE;
	}
	else
	{
		const unsigned char* map = PtrAt(meshInst, OFF_NMI_FACE_MAP);
		int mapCount = Count(map, IntAt(meshInst, OFF_NMI_FACE_MAP + OFF_HKARRAY_SIZE));
		const unsigned char* inst = PtrAt(meshInst, OFF_NMI_INSTANCED_FACES);
		int instCount = Count(inst, IntAt(meshInst, OFF_NMI_INSTANCED_FACES + OFF_HKARRAY_SIZE));
		int mapped = (int)face;
		if (mapCount > 0)
			mapped = face < (unsigned)mapCount ? IntAt(map, (size_t)face * 4) : -2;
		if (mapped == -1)
			rec = orig ? orig + (size_t)face * LIVE_FACE_STRIDE : NULL;
		else if (mapped >= 0 && mapped < instCount)
			rec = inst + (size_t)mapped * LIVE_FACE_STRIDE;
	}
	return rec ? (int)*(const short*)(rec + LIVE_FACE_CLUSTER) : -1;
}

} // namespace planner
