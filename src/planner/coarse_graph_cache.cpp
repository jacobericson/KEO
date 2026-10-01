// coarse_graph_cache.cpp - The base cache's records, CRC32, file layout and read-whole loader.
#include "planner/coarse_graph_cache.h"

#include <algorithm>
#include <new>
#include <string.h>

namespace planner {

namespace coarse_graph_cache_detail {

struct TileHead    { unsigned sectionCount, interiorsDropped, bordersSkipped, zero; };
struct SectionHead { int uid, kind, gx, gy; float origin[3]; int nodeCount, arcCount, borderCount; };

// A bounded cursor over one record.
struct Reader { const unsigned char* p; size_t n, at; };

} // namespace coarse_graph_cache_detail
using namespace coarse_graph_cache_detail;

static_assert(sizeof(CgCacheHeader) == 64, "cache header size");
static_assert(sizeof(CgCacheIndexEntry) == 40, "cache index entry size");
static_assert(sizeof(TgNode) == 48 && sizeof(TgArc) == 8 && sizeof(TgBorder) == 48, "record element sizes");
static_assert(sizeof(TileHead) == 16 && sizeof(SectionHead) == 40, "record head sizes");

static bool Take(Reader* r, void* out, size_t k)
{
	if (k > r->n - r->at)
		return false;
	memcpy(out, r->p + r->at, k);
	r->at += k;
	return true;
}

// ---- CRC32 -------------------------------------------------------------------------------------

static unsigned Crc32Update(unsigned crc, const void* data, size_t n)
{
	unsigned table[256];
	for (unsigned i = 0; i < 256; ++i)
	{
		unsigned c = i;
		for (int k = 0; k < 8; ++k)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		table[i] = c;
	}
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < n; ++i)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

unsigned CgCrc32(const void* data, size_t n)
{
	return Crc32Update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

// ---- Tile records ------------------------------------------------------------------------------

static void Append(std::vector<unsigned char>* out, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	out->insert(out->end(), b, b + n);
}

void CgCacheEncodeTile(const TileGraph& g, std::vector<unsigned char>* out)
{
	TileHead th = { (unsigned)g.sections.size(), (unsigned)g.interiorsDropped, (unsigned)g.bordersSkipped, 0 };
	Append(out, &th, sizeof(th));
	for (size_t s = 0; s < g.sections.size(); ++s)
	{
		const TgSection& sec = g.sections[s];
		std::vector<TgNode> nodes;
		std::vector<TgArc> arcs;
		for (int i = 0; i < sec.nodeCount; ++i)
		{
			TgNode n = g.nodes[(size_t)(sec.firstNode + i)];
			int first = (int)arcs.size();
			for (int k = 0; k < n.arcCount; ++k)
				if (n.firstArc + k >= 0 && n.firstArc + k < (int)g.arcs.size())
					arcs.push_back(g.arcs[(size_t)(n.firstArc + k)]);
			n.firstArc = first;
			n.arcCount = (int)arcs.size() - first;
			nodes.push_back(n);
		}
		SectionHead sh;
		sh.uid = sec.uid;
		sh.kind = sec.kind;
		sh.gx = sec.gx;
		sh.gy = sec.gy;
		memcpy(sh.origin, sec.origin, sizeof(sh.origin));
		sh.nodeCount = sec.nodeCount;
		sh.arcCount = (int)arcs.size();
		sh.borderCount = sec.borderCount;
		Append(out, &sh, sizeof(sh));
		if (!nodes.empty())
			Append(out, &nodes[0], nodes.size() * sizeof(TgNode));
		if (!arcs.empty())
			Append(out, &arcs[0], arcs.size() * sizeof(TgArc));
		if (sec.borderCount > 0)
			Append(out, &g.borders[(size_t)sec.firstBorder], (size_t)sec.borderCount * sizeof(TgBorder));
	}
}

static bool DecodeSection(Reader* r, TileGraph* out)
{
	SectionHead sh;
	if (!Take(r, &sh, sizeof(sh)))
		return false;
	if (sh.nodeCount < 0 || sh.nodeCount > TG_MAX_CLUSTERS || sh.arcCount < 0 || sh.borderCount < 0)
		return false;
	size_t need = (size_t)sh.nodeCount * sizeof(TgNode) + (size_t)sh.arcCount * sizeof(TgArc)
	            + (size_t)sh.borderCount * sizeof(TgBorder);
	if (need > r->n - r->at)
		return false;
	TgSection sec;
	sec.uid = sh.uid;
	sec.kind = sh.kind;
	sec.gx = sh.gx;
	sec.gy = sh.gy;
	memcpy(sec.origin, sh.origin, sizeof(sec.origin));
	sec.firstNode = (int)out->nodes.size();
	sec.nodeCount = sh.nodeCount;
	sec.firstBorder = (int)out->borders.size();
	sec.borderCount = sh.borderCount;
	int arcBase = (int)out->arcs.size();
	for (int i = 0; i < sh.nodeCount; ++i)
	{
		TgNode n;
		Take(r, &n, sizeof(n));
		if (n.arcCount < 0 || n.firstArc < 0 || n.firstArc > sh.arcCount || n.arcCount > sh.arcCount - n.firstArc)
			return false;
		n.firstArc += arcBase;
		out->nodes.push_back(n);
	}
	for (int i = 0; i < sh.arcCount; ++i)
	{
		TgArc a;
		Take(r, &a, sizeof(a));
		if (a.to < 0 || a.to >= sh.nodeCount)
			return false;
		out->arcs.push_back(a);
	}
	for (int i = 0; i < sh.borderCount; ++i)
	{
		TgBorder b;
		Take(r, &b, sizeof(b));
		if (b.from < -1 || b.from >= sh.nodeCount)
			return false;
		out->borders.push_back(b);
	}
	out->sections.push_back(sec);
	return true;
}

bool CgCacheDecodeTile(const unsigned char* p, size_t n, TileGraph* out)
{
	out->sections.clear();
	out->nodes.clear();
	out->arcs.clear();
	out->borders.clear();
	out->interiorsDropped = 0;
	out->bordersSkipped = 0;
	Reader r = { p, n, 0 };
	TileHead th;
	if (!p || !Take(&r, &th, sizeof(th)) || th.zero != 0)
		return false;
	if (th.sectionCount > (n - r.at) / sizeof(SectionHead))
		return false;
	try
	{
		for (unsigned s = 0; s < th.sectionCount; ++s)
			if (!DecodeSection(&r, out))
				return false;
	}
	catch (const std::bad_alloc&)
	{
		return false;
	}
	if (r.at != n)
		return false;
	out->interiorsDropped = (int)th.interiorsDropped;
	out->bordersSkipped = (int)th.bordersSkipped;
	return true;
}

// ---- The file ----------------------------------------------------------------------------------

static unsigned HeaderCrc(const unsigned char* file, const CgCacheHeader& h)
{
	unsigned crc = Crc32Update(0xFFFFFFFFu, file, 32);
	crc = Crc32Update(crc, file + h.indexOffset, h.indexBytes);
	return crc ^ 0xFFFFFFFFu;
}

CgCacheCheck CgCacheValidate(const unsigned char* file, size_t n)
{
	if (!file || n < sizeof(CgCacheHeader))
		return CGC_SHORT;
	CgCacheHeader h;
	memcpy(&h, file, sizeof(h));
	if (h.magic != PLANNER_CACHE_MAGIC)
		return CGC_MAGIC;
	if (h.version != PLANNER_CACHE_VERSION)
		return CGC_VERSION;
	if (h.readerVersion != (unsigned)TAGFILE_READER_VERSION)
		return CGC_READER;
	if (h.indexOffset != sizeof(CgCacheHeader) || h.tileCount > (n - sizeof(CgCacheHeader)) / sizeof(CgCacheIndexEntry)
	    || h.indexBytes != h.tileCount * sizeof(CgCacheIndexEntry)
	    || h.payloadOffset != h.indexOffset + h.indexBytes || h.payloadBytes != n - h.payloadOffset)
		return CGC_BOUNDS;
	if (HeaderCrc(file, h) != h.headerCrc)
		return CGC_CRC;
	for (unsigned i = 0; i < h.tileCount; ++i)
	{
		CgCacheIndexEntry e;
		memcpy(&e, file + h.indexOffset + i * sizeof(CgCacheIndexEntry), sizeof(e));
		if (e.payloadOffset > h.payloadBytes || e.payloadBytes > h.payloadBytes - e.payloadOffset)
			return CGC_BOUNDS;
	}
	return CGC_OK;
}

static bool EntryBefore(const CgCacheIndexEntry& a, const CgCacheIndexEntry& b)
{
	if (a.gy != b.gy)
		return a.gy < b.gy;
	return a.gx < b.gx;
}

const CgCacheIndexEntry* CgCacheFind(const unsigned char* file, size_t n, int gx, int gy,
                                     unsigned fileSize, unsigned __int64 mtime)
{
	if (!file || n < sizeof(CgCacheHeader))
		return NULL;
	const CgCacheHeader* h = (const CgCacheHeader*)file;
	if ((size_t)h->payloadOffset > n || h->indexOffset != sizeof(CgCacheHeader)
	    || (size_t)h->indexOffset + (size_t)h->tileCount * sizeof(CgCacheIndexEntry) > n)
		return NULL;
	const CgCacheIndexEntry* first = (const CgCacheIndexEntry*)(file + h->indexOffset);
	const CgCacheIndexEntry* last = first + h->tileCount;
	CgCacheIndexEntry key;
	memset(&key, 0, sizeof(key));
	key.gx = (short)gx;
	key.gy = (short)gy;
	const CgCacheIndexEntry* e = std::lower_bound(first, last, key, EntryBefore);
	if (e == last || e->gx != gx || e->gy != gy)
		return NULL;
	if (e->fileSize != fileSize || e->mtime != mtime || e->readerVersion != (unsigned)TAGFILE_READER_VERSION)
		return NULL;
	size_t start = (size_t)h->payloadOffset + e->payloadOffset;
	if (start > n || e->payloadBytes > n - start)
		return NULL;
	if (CgCrc32(file + start, e->payloadBytes) != e->payloadCrc)
		return NULL;
	return e;
}

void CgCacheBuild(const std::vector<CgCacheIndexEntry>& entries,
                  const std::vector<unsigned char>& payload, std::vector<unsigned char>* file)
{
	std::vector<CgCacheIndexEntry> index(entries);
	std::stable_sort(index.begin(), index.end(), EntryBefore);
	for (size_t i = 0; i < index.size(); ++i)
	{
		CgCacheIndexEntry& e = index[i];
		bool inside = e.payloadOffset <= payload.size() && e.payloadBytes <= payload.size() - e.payloadOffset;
		e.payloadCrc = inside ? CgCrc32(payload.empty() ? NULL : &payload[e.payloadOffset], e.payloadBytes) : 0;
		e.readerVersion = (unsigned)TAGFILE_READER_VERSION;
		e.pad = 0;
	}
	CgCacheHeader h;
	memset(&h, 0, sizeof(h));
	h.magic = PLANNER_CACHE_MAGIC;
	h.version = PLANNER_CACHE_VERSION;
	h.readerVersion = (unsigned)TAGFILE_READER_VERSION;
	h.tileCount = (unsigned)index.size();
	h.indexOffset = sizeof(CgCacheHeader);
	h.indexBytes = (unsigned)(index.size() * sizeof(CgCacheIndexEntry));
	h.payloadOffset = h.indexOffset + h.indexBytes;
	h.payloadBytes = (unsigned)payload.size();
	file->clear();
	file->resize(sizeof(h));
	if (!index.empty())
		Append(file, &index[0], h.indexBytes);
	if (!payload.empty())
		Append(file, &payload[0], payload.size());
	memcpy(&(*file)[0], &h, sizeof(h));
	h.headerCrc = HeaderCrc(&(*file)[0], h);
	memcpy(&(*file)[0], &h, sizeof(h));
}

// ---- Read whole --------------------------------------------------------------------------------

bool CgReadWhole(const CgFileOps& ops, const wchar_t* path, size_t maxBytes, std::vector<unsigned char>* out)
{
	out->clear();
	if (!ops.open(ops.ctx, path))
		return false;
	unsigned __int64 size = 0;
	if (!ops.size(ops.ctx, &size) || size > (unsigned __int64)maxBytes)
	{
		ops.close(ops.ctx);
		return false;
	}
	bool ok = true;
	try
	{
		out->resize((size_t)size);
	}
	catch (const std::bad_alloc&)
	{
		ok = false;
	}
	if (ok && size > 0)
		ok = ops.read(ops.ctx, &(*out)[0], (size_t)size);
	ops.close(ops.ctx);
	if (!ok)
		out->clear();
	return ok;
}

} // namespace planner
