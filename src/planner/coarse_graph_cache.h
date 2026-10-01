// coarse_graph_cache.h - The whole-map base's disk cache: a header, a per-tile index and one record
// per tile holding its extracted graph, each checked by CRC32; and a read-whole loader that closes
// the file before it returns. Pure but for windows.h's integer types; any thread, CRT allocation only.
#ifndef KENSHI_ZONE_OPT_PLANNER_COARSE_GRAPH_CACHE_H
#define KENSHI_ZONE_OPT_PLANNER_COARSE_GRAPH_CACHE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <vector>
#include "planner/tile_graph_extract.h"

namespace planner {

const unsigned PLANNER_CACHE_MAGIC   = 0x31505A4Bu;   // 'K' 'Z' 'P' '1' in file order
const unsigned PLANNER_CACHE_VERSION = 1;              // any layout change bumps it

struct CgCacheHeader        // 64 bytes at offset 0
{
	unsigned magic, version, readerVersion, tileCount;
	unsigned indexOffset, indexBytes, payloadOffset, payloadBytes;
	unsigned headerCrc;     // CRC32 of bytes [0, 32) followed by the whole index
	unsigned reserved[7];
};
struct CgCacheIndexEntry    // 40 bytes, one per cached tile, sorted by (gy, gx)
{
	short gx, gy;
	unsigned fileSize;              // the tile file's size when it was read
	unsigned __int64 mtime;         // its last-write FILETIME
	unsigned readerVersion;         // TAGFILE_READER_VERSION when it was read
	unsigned payloadOffset;         // from the header's payloadOffset
	unsigned payloadBytes;
	unsigned payloadCrc;            // CRC32 of the record
	unsigned sectionCount;
	unsigned pad;
};
unsigned CgCrc32(const void* data, size_t n);     // reflected 0xEDB88320; "123456789" -> 0xCBF43926
// A tile's record: a 16-byte head {sectionCount, interiorsDropped, bordersSkipped, 0}, then per
// section {uid, kind, gx, gy, origin[3], nodeCount, arcCount, borderCount} then its nodes, arcs and
// borders as the TileGraph holds them (a node's firstArc counted from the section's first arc).
void CgCacheEncodeTile(const TileGraph& g, std::vector<unsigned char>* out);
bool CgCacheDecodeTile(const unsigned char* p, size_t n, TileGraph* out);   // false on any bound
// The whole file: header, index, payload. Validate refuses a bad magic, version, reader version,
// CRC or bound and says which.
enum CgCacheCheck { CGC_OK = 0, CGC_SHORT, CGC_MAGIC, CGC_VERSION, CGC_READER, CGC_CRC, CGC_BOUNDS };
CgCacheCheck CgCacheValidate(const unsigned char* file, size_t n);
// Finds a tile's record in a validated file: the entry when size, mtime and reader version match
// and the record's CRC holds, else NULL.
const CgCacheIndexEntry* CgCacheFind(const unsigned char* file, size_t n, int gx, int gy,
                                     unsigned fileSize, unsigned __int64 mtime);
// entries' payloadOffset and payloadBytes locate each record in payload; their order and
// payloadCrc are the builder's (sorted, and computed from payload).
void CgCacheBuild(const std::vector<CgCacheIndexEntry>& entries,
                  const std::vector<unsigned char>& payload, std::vector<unsigned char>* file);

// Read a file whole and close it before returning, on every path. The operations are the game
// side's Win32 calls, or a host test's fakes.
struct CgFileOps
{
	void* ctx;
	bool (*open)(void* ctx, const wchar_t* path);
	bool (*size)(void* ctx, unsigned __int64* out);
	bool (*read)(void* ctx, void* buf, size_t n);
	void (*close)(void* ctx);
};
bool CgReadWhole(const CgFileOps& ops, const wchar_t* path, size_t maxBytes, std::vector<unsigned char>* out);

} // namespace planner

#endif
