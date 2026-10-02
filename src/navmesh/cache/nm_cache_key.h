#ifndef KEO_NM_CACHE_KEY_H
#define KEO_NM_CACHE_KEY_H

// The L1 ring's key and entry types, with no game header, so pure policies
// and their host suites can use them.

struct NavMeshCacheKey {
	int gridX;
	int gridY;
	int sectionTileId;
	int jobType;
	unsigned int aabbHash;
	unsigned int buildingHash;
};

struct NavMeshCacheEntry {
	NavMeshCacheKey key;

	void*  cachedFaces;      int faceCount;
	void*  cachedEdges;      int edgeCount;
	void*  cachedVertices;   int vertexCount;
	void*  cachedFaceData;   int faceDataCount;
	void*  cachedEdgeData;   int edgeDataCount;

	int            faceDataStriding;   // +112
	int            edgeDataStriding;   // +116
	unsigned char  navMeshFlags;       // +120
	char           aabb[32];           // +128: 2 x __m128
	float          erosionRadius;      // +160
	unsigned __int64 userData;         // +168

	bool   valid;
};

const int NM_CACHE_SIZE = 256;

#endif
