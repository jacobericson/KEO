// tile_graph_extract.h - A navmesh tile's coarse graph, extracted from its parsed tagfile: one
// section per navmesh/graph group, one node per cluster with its world-frame centre and footprint,
// intra arcs from the section's graph, and the border connections of the mesh's streaming sets.
// Pure: no Windows, KenshiLib or game header. Any thread; CRT allocation only.
#ifndef KEO_PLANNER_TILE_GRAPH_EXTRACT_H
#define KEO_PLANNER_TILE_GRAPH_EXTRACT_H

#include "planner/tagfile_reader.h"

namespace planner {

const float    TILE_CELL       = 4608.0f;      // world units a tile spans
const float    TILE_ORIGIN     = -147456.0f;   // world x and z of tile 0.0's corner
const float    HAVOK_TO_WORLD  = 10.0f;
const int      TG_MAX_CLUSTERS = 1023;         // a store node key carries 10 cluster bits
const unsigned TG_SECTION_SHIFT = 22;          // a graph edge target's section field
const unsigned TG_NODE_MASK    = 0x3FFFFFu;

enum TgSectionKind { TGS_EXTERIOR = 0, TGS_INTERIOR };

struct TgNode
{
	float centre[3];    // the graph position, world units
	float boxMin[3];    // the footprint: the bounding box of the node's faces' vertices, world
	float boxMax[3];    //   units; the centre alone when the node has no face
	int   faces;        // faces whose clusterIndex names this node
	int   water;        // 0..255: the x/z area share of the node's faces whose first face-data word is 3
	int   firstArc;     // into TileGraph::arcs
	int   arcCount;
};
struct TgArc { int to; float cost; };   // intra: a node index within the section; world units

// One mesh streaming connection, seen from this side.
struct TgBorder
{
	int   oppUid;       // the section across the edge
	int   face, edge;   // this side's face and edge
	int   oppFace, oppEdge;
	int   from;         // this side's node: clusterIndex[face], -1 when that is not a node
	float a[3], b[3];   // the edge's two ends, world units
};
struct TgSection
{
	int   uid;          // exterior: x | (y << 8); interior: the Info string's hex uid
	int   kind;         // TgSectionKind
	int   gx, gy;       // the tile
	float origin[3];    // world position of the section's local origin
	int   firstNode, nodeCount;
	int   firstBorder, borderCount;   // sorted by (oppUid, face, edge)
};
struct TileGraph
{
	std::vector<TgSection> sections;
	std::vector<TgNode>    nodes;
	std::vector<TgArc>     arcs;
	std::vector<TgBorder>  borders;
	int interiorsDropped;   // interior groups skipped: no Info, a bad uid, an incomplete object, or
	                        //   a face's edge run or vertex index out of range
	int bordersSkipped;     // connections whose face, edge or vertex index was out of range
};

enum TgResult
{
	TG_OK = 0,
	TG_NO_EXTERIOR,     // no complete NavMesh and Graph pair for the exterior
	TG_TOO_MANY_NODES,  // a section over TG_MAX_CLUSTERS nodes
	TG_BAD_INDEX        // an exterior face's edge run or vertex index out of range
};

// gx, gy: the tile's coordinates (from its file name). The exterior's origin is
// (gx*TILE_CELL + TILE_ORIGIN, 0, gy*TILE_CELL + TILE_ORIGIN); an interior's is its Info x, y, z.
// World = origin + local * HAVOK_TO_WORLD, per axis.
TgResult TgExtract(const TfDoc& doc, int gx, int gy, TileGraph* out);

// A tile's world-frame corner, and a world point's tile (floor division; no range check).
void TgTileCorner(int gx, int gy, float* x, float* z);
void TgCellOf(float x, float z, int* gx, int* gy);

} // namespace planner

#endif
