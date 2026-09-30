// tile_graph_extract.cpp - A tile's coarse graph from its parsed tagfile. Pure; runs on the
// caller's thread, takes no lock and shares nothing between calls.

#include "planner/tile_graph_extract.h"

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace planner {

namespace tile_graph_extract_detail {

// One navmesh/graph group of the root container's named variants. Each object is its STRUCT
// value, -1 when the variant is absent, incomplete or of another class.
struct Group
{
	int  mesh;
	int  graph;
	int  info;
	bool graphSeen;
	bool infoSeen;
};

// A section's arrays, looked up once.
struct SectionArrays
{
	int faces, edges, vertices, nodes, graphEdges, positions;
	int faceCount, edgeCount, vertexCount, nodeCount, graphEdgeCount, positionCount;
};

struct BorderLess
{
	bool operator()(const TgBorder& a, const TgBorder& b) const
	{
		if (a.oppUid != b.oppUid)
			return a.oppUid < b.oppUid;
		if (a.face != b.face)
			return a.face < b.face;
		return a.edge < b.edge;
	}
};

enum SectionResult { SEC_OK = 0, SEC_BAD_INDEX };

} // namespace tile_graph_extract_detail
using namespace tile_graph_extract_detail;

// An integer field; an absent field, or one of another kind, reads 0.
static __int64 IntField(const TfDoc& d, int st, const char* name)
{
	__int64 v = 0;
	return TfAsInt(d, TfFind(d, st, name), &v) ? v : 0;
}

static float RealField(const TfDoc& d, int st, const char* name)
{
	float v = 0.0f;
	return TfAsReal(d, TfFind(d, st, name), &v) ? v : 0.0f;
}

static const char* StringField(const TfDoc& d, int st, const char* name)
{
	return TfAsString(d, TfFind(d, st, name));
}

static int ArrayField(const TfDoc& d, int st, const char* name, int* count)
{
	int a = TfFind(d, st, name);
	int n = TfArrayCount(d, a);
	*count = n < 0 ? 0 : n;
	return a;
}

// The three components of element k of a vector array; missing components read 0.
static void VecAt(const TfDoc& d, int array, int k, float out[3])
{
	float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	TfAsVec(d, TfArrayAt(d, array, k), v, 4);
	out[0] = v[0];
	out[1] = v[1];
	out[2] = v[2];
}

static void ToWorld(const float origin[3], const float local[3], float out[3])
{
	for (int a = 0; a < 3; ++a)
		out[a] = origin[a] + local[a] * HAVOK_TO_WORLD;
}

// A variant's object when complete and of the class its entry names, else -1.
static int VariantObject(const TfDoc& d, int entry, const char* cls)
{
	int obj = TfDeref(d, TfFind(d, entry, "variant"));
	if (obj < 0 || strcmp(TfClassName(d, obj), cls) != 0)
		return -1;
	return obj;
}

// Groups open at each NavMesh entry; the first Graph and the first Info entry before the next
// NavMesh belong to it. Entries are recognised by their name and class strings, so an incomplete
// object still holds its place in its group.
static void FindGroups(const TfDoc& d, std::vector<Group>* groups)
{
	int root = TfRoot(d);
	int count = 0;
	int variants = ArrayField(d, root, "namedVariants", &count);
	for (int k = 0; k < count; ++k)
	{
		int e = TfArrayAt(d, variants, k);
		const char* name = StringField(d, e, "name");
		const char* cls = StringField(d, e, "className");
		if (strcmp(name, "NavMesh") == 0 && strcmp(cls, "hkaiNavMesh") == 0)
		{
			Group g;
			g.mesh = VariantObject(d, e, cls);
			g.graph = -1;
			g.info = -1;
			g.graphSeen = false;
			g.infoSeen = false;
			groups->push_back(g);
		}
		else if (groups->empty())
		{
			continue;
		}
		else if (strcmp(name, "Graph") == 0 && strcmp(cls, "hkaiDirectedGraphExplicitCost") == 0 && !groups->back().graphSeen)
		{
			groups->back().graphSeen = true;
			groups->back().graph = VariantObject(d, e, cls);
		}
		else if (strcmp(name, "Info") == 0 && strcmp(cls, "hkStringObject") == 0 && !groups->back().infoSeen)
		{
			groups->back().infoSeen = true;
			groups->back().info = VariantObject(d, e, cls);
		}
	}
}

static bool IsHex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

// One decimal real, [-+]?(digits[.digits?]|.digits)([eE][-+]?digits)?, ending at ';' or at the
// string's end; *s is left on the terminator.
static bool ParseReal(const char** s, float* out)
{
	const char* p = *s;
	const char* start = p;
	if (*p == '-' || *p == '+')
		++p;
	int digits = 0;
	for (; IsDigit(*p); ++p)
		++digits;
	if (*p == '.')
	{
		++p;
		for (; IsDigit(*p); ++p)
			++digits;
	}
	if (digits == 0)
		return false;
	if (*p == 'e' || *p == 'E')
	{
		const char* q = p + 1;
		if (*q == '-' || *q == '+')
			++q;
		if (IsDigit(*q))
		{
			while (IsDigit(*q))
				++q;
			p = q;
		}
	}
	if (*p != ';' && *p != 0)
		return false;
	char buf[64];
	size_t n = (size_t)(p - start);
	if (n >= sizeof(buf))
		return false;
	memcpy(buf, start, n);
	buf[n] = 0;
	*out = (float)strtod(buf, 0);
	*s = p;
	return true;
}

// An interior's Info string: "<1-8 hex digits>;<x>;<y>;<z>" and nothing after.
static bool ParseInfo(const char* s, unsigned* uid, float xyz[3])
{
	unsigned v = 0;
	int n = 0;
	for (; IsHex(*s); ++s, ++n)
	{
		if (n == 8)
			return false;
		v = v * 16u + (unsigned)(IsDigit(*s) ? *s - '0' : (*s | 0x20) - 'a' + 10);
	}
	if (n == 0 || *s != ';')
		return false;
	++s;
	for (int a = 0; a < 3; ++a)
	{
		if (!ParseReal(&s, &xyz[a]))
			return false;
		if (a < 2)
		{
			if (*s != ';')
				return false;
			++s;
		}
	}
	if (*s != 0)
		return false;
	*uid = v;
	return true;
}

static SectionArrays LookUp(const TfDoc& d, const Group& g)
{
	SectionArrays s;
	s.faces = ArrayField(d, g.mesh, "faces", &s.faceCount);
	s.edges = ArrayField(d, g.mesh, "edges", &s.edgeCount);
	s.vertices = ArrayField(d, g.mesh, "vertices", &s.vertexCount);
	s.nodes = ArrayField(d, g.graph, "nodes", &s.nodeCount);
	s.graphEdges = ArrayField(d, g.graph, "edges", &s.graphEdgeCount);
	s.positions = ArrayField(d, g.graph, "positions", &s.positionCount);
	return s;
}

// Each node's centre, face count and footprint, and each face's node (-1 when its clusterIndex
// names none). A counted face (one whose clusterIndex names a node) with its edge run or a vertex
// index out of range answers SEC_BAD_INDEX. A node whose faces give no vertex keeps its centre as
// its box.
static SectionResult ReadFaces(const TfDoc& d, const SectionArrays& s, const float origin[3],
	std::vector<TgNode>& nodes, std::vector<int>& cluster)
{
	std::vector<bool> boxed(s.nodeCount, false);
	for (int k = 0; k < s.nodeCount; ++k)
	{
		float local[3];
		VecAt(d, s.positions, k, local);
		ToWorld(origin, local, nodes[k].centre);
		nodes[k].faces = 0;
		nodes[k].firstArc = 0;
		nodes[k].arcCount = 0;
	}
	for (int f = 0; f < s.faceCount; ++f)
	{
		int face = TfArrayAt(d, s.faces, f);
		__int64 c = IntField(d, face, "clusterIndex");
		if (c < 0 || c >= s.nodeCount)
			continue;
		cluster[f] = (int)c;
		__int64 start = IntField(d, face, "startEdgeIndex");
		__int64 count = IntField(d, face, "numEdges");
		if (start < 0 || count < 0 || start + count > s.edgeCount)
			return SEC_BAD_INDEX;
		TgNode& n = nodes[(int)c];
		++n.faces;
		for (__int64 e = start; e < start + count; ++e)
		{
			__int64 a = IntField(d, TfArrayAt(d, s.edges, (int)e), "a");
			if (a < 0 || a >= s.vertexCount)
				return SEC_BAD_INDEX;
			float local[3], w[3];
			VecAt(d, s.vertices, (int)a, local);
			ToWorld(origin, local, w);
			for (int ax = 0; ax < 3; ++ax)
			{
				if (!boxed[(int)c] || w[ax] < n.boxMin[ax])
					n.boxMin[ax] = w[ax];
				if (!boxed[(int)c] || w[ax] > n.boxMax[ax])
					n.boxMax[ax] = w[ax];
			}
			boxed[(int)c] = true;
		}
	}
	for (int k = 0; k < s.nodeCount; ++k)
	{
		if (boxed[k])
			continue;
		for (int ax = 0; ax < 3; ++ax)
		{
			nodes[k].boxMin[ax] = nodes[k].centre[ax];
			nodes[k].boxMax[ax] = nodes[k].centre[ax];
		}
	}
	return SEC_OK;
}

// Appends the nodes with their intra arcs. A node's graph edge run is clipped to the edge array.
static void AddNodes(const TfDoc& d, const SectionArrays& s, std::vector<TgNode>& nodes, TgSection* sec, TileGraph* out)
{
	sec->firstNode = (int)out->nodes.size();
	sec->nodeCount = s.nodeCount;
	for (int k = 0; k < s.nodeCount; ++k)
	{
		TgNode& n = nodes[k];
		int node = TfArrayAt(d, s.nodes, k);
		__int64 start = IntField(d, node, "startEdgeIndex");
		__int64 end = start + IntField(d, node, "numEdges");
		if (start < 0)
			start = 0;
		if (end > s.graphEdgeCount)
			end = s.graphEdgeCount;
		n.firstArc = (int)out->arcs.size();
		for (__int64 e = start; e < end; ++e)
		{
			int edge = TfArrayAt(d, s.graphEdges, (int)e);
			__int64 target = IntField(d, edge, "target");
			if ((target >> TG_SECTION_SHIFT) != 0)
				continue;
			int to = (int)(target & TG_NODE_MASK);
			if (to >= s.nodeCount || to == k)
				continue;
			TgArc arc;
			arc.to = to;
			arc.cost = RealField(d, edge, "cost") * HAVOK_TO_WORLD;
			out->arcs.push_back(arc);
		}
		n.arcCount = (int)out->arcs.size() - n.firstArc;
		out->nodes.push_back(n);
	}
}

// Appends the connections of the mesh's streaming sets whose thisUid is the section's (compared
// as 32-bit unsigned), sorted by (oppUid, face, edge); an out-of-range face, edge or vertex index
// counts in bordersSkipped.
static void AddBorders(const TfDoc& d, const Group& g, const SectionArrays& s, const std::vector<int>& cluster,
	TgSection* sec, TileGraph* out)
{
	sec->firstBorder = (int)out->borders.size();
	int setCount = 0;
	int sets = ArrayField(d, g.mesh, "streamingSets", &setCount);
	for (int k = 0; k < setCount; ++k)
	{
		int set = TfArrayAt(d, sets, k);
		if ((unsigned)IntField(d, set, "thisUid") != (unsigned)sec->uid)
			continue;
		int opp = (int)(unsigned)IntField(d, set, "oppositeUid");
		int connCount = 0;
		int conns = ArrayField(d, set, "meshConnections", &connCount);
		for (int j = 0; j < connCount; ++j)
		{
			int conn = TfArrayAt(d, conns, j);
			__int64 face = IntField(d, conn, "faceIndex");
			__int64 edge = IntField(d, conn, "edgeIndex");
			if (face < 0 || face >= s.faceCount || edge < 0 || edge >= s.edgeCount)
			{
				++out->bordersSkipped;
				continue;
			}
			int edgeSt = TfArrayAt(d, s.edges, (int)edge);
			__int64 a = IntField(d, edgeSt, "a");
			__int64 b = IntField(d, edgeSt, "b");
			if (a < 0 || a >= s.vertexCount || b < 0 || b >= s.vertexCount)
			{
				++out->bordersSkipped;
				continue;
			}
			TgBorder br;
			br.oppUid = opp;
			br.face = (int)face;
			br.edge = (int)edge;
			br.oppFace = (int)IntField(d, conn, "oppositeFaceIndex");
			br.oppEdge = (int)IntField(d, conn, "oppositeEdgeIndex");
			br.from = cluster[(int)face];
			float local[3];
			VecAt(d, s.vertices, (int)a, local);
			ToWorld(sec->origin, local, br.a);
			VecAt(d, s.vertices, (int)b, local);
			ToWorld(sec->origin, local, br.b);
			out->borders.push_back(br);
		}
	}
	sec->borderCount = (int)out->borders.size() - sec->firstBorder;
	std::stable_sort(out->borders.begin() + sec->firstBorder, out->borders.end(), BorderLess());
}

// Appends one section's nodes, arcs and borders, or nothing when its faces answer SEC_BAD_INDEX.
static SectionResult AddSection(const TfDoc& d, const Group& g, const SectionArrays& s, TgSection* sec, TileGraph* out)
{
	std::vector<TgNode> nodes(s.nodeCount);
	std::vector<int> cluster(s.faceCount, -1);
	if (ReadFaces(d, s, sec->origin, nodes, cluster) != SEC_OK)
		return SEC_BAD_INDEX;
	AddNodes(d, s, nodes, sec, out);
	AddBorders(d, g, s, cluster, sec, out);
	return SEC_OK;
}

static void Clear(TileGraph* out)
{
	out->sections.clear();
	out->nodes.clear();
	out->arcs.clear();
	out->borders.clear();
	out->interiorsDropped = 0;
	out->bordersSkipped = 0;
}

static TgResult Refuse(TileGraph* out, TgResult r)
{
	Clear(out);
	return r;
}

// Checks run in a fixed order per group: its objects and Info, its positions against its nodes,
// the node cap, then its faces. The exterior fails the tile at the first failed check; an
// interior is dropped at any of them but the node cap, which fails the tile.
TgResult TgExtract(const TfDoc& doc, int gx, int gy, TileGraph* out)
{
	Clear(out);
	std::vector<Group> groups;
	FindGroups(doc, &groups);
	if (groups.empty())
		return Refuse(out, TG_NO_EXTERIOR);
	for (size_t g = 0; g < groups.size(); ++g)
	{
		const Group& grp = groups[g];
		bool exterior = g == 0;
		TgSection sec;
		memset(&sec, 0, sizeof(sec));
		sec.kind = exterior ? TGS_EXTERIOR : TGS_INTERIOR;
		sec.gx = gx;
		sec.gy = gy;
		bool usable = grp.mesh >= 0 && grp.graph >= 0;
		if (exterior)
		{
			sec.uid = gx | (gy << 8);
			TgTileCorner(gx, gy, &sec.origin[0], &sec.origin[2]);
			sec.origin[1] = 0.0f;
		}
		else if (usable)
		{
			unsigned uid = 0;
			usable = grp.info >= 0 && ParseInfo(StringField(doc, grp.info, "string"), &uid, sec.origin);
			sec.uid = (int)uid;
		}
		SectionArrays arrays;
		memset(&arrays, 0, sizeof(arrays));
		if (usable)
		{
			arrays = LookUp(doc, grp);
			usable = arrays.positionCount >= arrays.nodeCount;
		}
		if (!usable)
		{
			if (exterior)
				return Refuse(out, TG_NO_EXTERIOR);
			++out->interiorsDropped;
			continue;
		}
		if (arrays.nodeCount > TG_MAX_CLUSTERS)
			return Refuse(out, TG_TOO_MANY_NODES);
		if (AddSection(doc, grp, arrays, &sec, out) != SEC_OK)
		{
			if (exterior)
				return Refuse(out, TG_BAD_INDEX);
			++out->interiorsDropped;
			continue;
		}
		out->sections.push_back(sec);
	}
	return TG_OK;
}

void TgTileCorner(int gx, int gy, float* x, float* z)
{
	*x = (float)gx * TILE_CELL + TILE_ORIGIN;
	*z = (float)gy * TILE_CELL + TILE_ORIGIN;
}

void TgCellOf(float x, float z, int* gx, int* gy)
{
	*gx = (int)floorf((x - TILE_ORIGIN) / TILE_CELL);
	*gy = (int)floorf((z - TILE_ORIGIN) / TILE_CELL);
}

} // namespace planner
