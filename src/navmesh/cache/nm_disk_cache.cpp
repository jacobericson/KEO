// nm_disk_cache.cpp — L2 persistent navmesh disk cache
//
// File layout (L2_CACHE_VERSION: 3):
//   [L2Header, 96 bytes] [payload: faces | edges | vertices | faceData | edgeData]
// The header carries a magic, a format version, a hash of the generation
// settings, the payload length and a CRC32 covering the rest of the header and
// the payload. A file that fails any of those is ignored and counted in l2Rej.
// The write itself lives in nm_l2_writer.cpp, which also holds the per-cause
// failure counters; this file maps its own refusals onto the same causes.
//
// Threading: runs on the NavMesh bg thread and the worker threads. No CRT
// string objects — C file I/O, malloc, Interlocked and LogMsgDeferrable only.

#include "navmesh/cache/nm_disk_cache.h"
#include "navmesh/generation/nm_quality.h"   // NM_MATERIAL_SLOPE_BITS, NM_PRUNE_VANILLA (settings hash)
#include "navmesh/cache/nm_l2_writer.h" // outcomes, the atomic write, the failure counters
#include "navmesh/cache/nm_key_hash.h"


#include <cstdlib>
#include <cstdarg>
#include <cstddef>

// --------------------------------------------------------------------
// Header
// --------------------------------------------------------------------
//
// Every field is naturally aligned, so the compiler adds no padding and the
// struct can be read and written as raw bytes (size checked below).

struct L2Header {
	unsigned int magic;            // +0   L2_CACHE_MAGIC
	int          version;          // +4   L2_CACHE_VERSION
	unsigned int settingsHash;     // +8   L2SettingsHash()
	unsigned int payloadLen;       // +12  bytes following this header
	unsigned int payloadCrc;       // +16  CRC32 of +20..end of payload
	int          faceCount;        // +20
	int          edgeCount;        // +24
	int          vertexCount;      // +28
	int          faceDataCount;    // +32
	int          edgeDataCount;    // +36
	int          faceDataStriding; // +40  navmesh +112
	int          edgeDataStriding; // +44  navmesh +116
	unsigned int navMeshFlags;     // +48  navmesh +120 (low byte)
	float        erosionRadius;    // +52  navmesh +160
	unsigned __int64 userData;     // +56  navmesh +168
	char         aabb[32];         // +64  navmesh +128
};

// Compile-time size check (VS 2010: negative array size if the layout shifts).
typedef char L2HeaderSizeCheck[(sizeof(L2Header) == 96) ? 1 : -1];

// The checksum covers the header from the first field after the CRC slot to the
// end of the payload. Everything before it — magic, version, settings hash and
// payload length — is validated on its own terms before the CRC is even
// computed, and the CRC cannot cover itself.
const size_t L2_CRC_REGION_START = offsetof(L2Header, faceCount);
typedef char L2CrcRegionCheck[(offsetof(L2Header, faceCount) == 20) ? 1 : -1];


// --------------------------------------------------------------------
// CRC32 and the settings hash
// --------------------------------------------------------------------

// Nibble table for the reflected CRC-32 polynomial 0xEDB88320. A constant
// table needs no lazy initialization, so there is no init race on the bg
// threads and no global constructor.
static const unsigned int kL2CrcNibble[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
	0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
	0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

unsigned int L2Crc32Update(unsigned int crc, const void* data, size_t len)
{
	const unsigned char* p = (const unsigned char*)data;
	if (!p) return crc;
	for (size_t i = 0; i < len; ++i)
	{
		crc ^= p[i];
		crc = (crc >> 4) ^ kL2CrcNibble[crc & 0xF];
		crc = (crc >> 4) ^ kL2CrcNibble[crc & 0xF];
	}
	return crc;
}

unsigned int L2Crc32(const void* data, size_t len)
{
	return L2Crc32End(L2Crc32Update(L2Crc32Begin(), data, len));
}

unsigned int L2SettingsHash()
{
	return L2SettingsHashOf(NM_MATERIAL_SLOPE_BITS, sizeof(NM_MATERIAL_SLOPE_BITS), &NM_PRUNE_VANILLA, sizeof(NM_PRUNE_VANILLA), &NM_EXTRA_VERTEX_VANILLA, sizeof(NM_EXTRA_VERTEX_VANILLA), NmVanillaPruningActive(), NmNbrSeedStandInActive());
}

static inline void L2Reject(int reason)
{
	if (reason >= 0 && reason < L2REJ_REASON_COUNT)
		InterlockedIncrement(&l2RejCount[reason]);
}


// --------------------------------------------------------------------
// Paths
// --------------------------------------------------------------------

// Formats into a fixed buffer without ever tripping the secure CRT's
// invalid-parameter handler (which terminates the process). _TRUNCATE returns
// -1 instead, and every caller treats an empty buffer as "skip this file".
static bool L2FormatPathQuiet(char* out, size_t outSize, const char* fmt, ...)
{
	if (!out || outSize == 0) return false;
	out[0] = 0;

	va_list args;
	va_start(args, fmt);
	int n = _vsnprintf_s(out, outSize, _TRUNCATE, fmt, args);
	va_end(args);

	if (n < 0) { out[0] = 0; return false; }
	return true;
}

// Counts a truncation as a read-side rejection. Only the read, sweep and cap
// paths use this: a lost write is counted against the write outcomes instead,
// so one l2Rej /p tick always means one file that could not be read, and never
// one that could not be stored.
static bool L2FormatPath(char* out, size_t outSize, const char* fmt, ...)
{
	if (!out || outSize == 0) return false;
	out[0] = 0;

	va_list args;
	va_start(args, fmt);
	int n = _vsnprintf_s(out, outSize, _TRUNCATE, fmt, args);
	va_end(args);

	if (n < 0)
	{
		out[0] = 0;
		L2Reject(L2REJ_PATHLONG);
		return false;
	}
	return true;
}

void GetDiskCachePath(const NavMeshCacheKey& key, char* out, size_t outSize)
{
	if (!out || outSize == 0) return;
	out[0] = 0;
	if (!nmDiskCacheDirBuf[0]) return;   // InitNavMeshCacheCS has not run
	L2FormatPathQuiet(out, outSize, "%s%d_%d_%d_%d_%x_%x_%x.bin",
	                  nmDiskCacheDirBuf,
	                  key.gridX, key.gridY, key.sectionTileId, key.jobType,
	                  key.aabbHash, key.buildingHash, g_modSetHash);
}

// Every path that loses an L2 file ends here. The first loss of the session
// names itself in the log: without it a session can generate for an hour and
// bank nothing while every line it prints looks normal. Deferrable, because
// this runs on the NavMesh bg thread and the workers.
static void L2NoteWriteFailure(L2WriteOutcome outcome, unsigned long osErr, const char* path)
{
	if (!L2WriteNote(outcome, osErr))
		return;

	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "L2 disk cache: first write failure (%s, err=%lu) for \"%s\" "
	            "- generations are not being banked; later failures are counted in l2WrFail=",
	            L2WriteOutcomeName(outcome), osErr, (path && path[0]) ? path : "<no path>");
	LogMsgDeferrable(line);
}


// --------------------------------------------------------------------
// Directory size cap
// --------------------------------------------------------------------
//
// A full scan is expensive, so the directory total is measured once (on the
// first write of the session) and then tracked incrementally. A rescan only
// happens when the tracked total passes the cap. One thread evicts at a time;
// the others skip, because the next write still finds the total high.

struct L2FileRec {
	FILETIME     writeTime;
	unsigned int size;
	char         name[128];
};

static volatile long     g_l2SizeKnown = 0;
static volatile LONGLONG g_l2DirBytes  = 0;
static volatile long     g_l2EvictBusy = 0;

static int L2RecCompare(const void* a, const void* b)
{
	const L2FileRec* ra = (const L2FileRec*)a;
	const L2FileRec* rb = (const L2FileRec*)b;
	return CompareFileTime(&ra->writeTime, &rb->writeTime);   // oldest first
}

// True when the name is exactly the current key format:
//   <gridX>_<gridY>_<tile>_<type>_<aabb>_<bldg>_<modset>.bin
// six underscores, and every other character a hex digit or the '-' of a
// negative grid coordinate. A name that fails this is from an older key layout
// (an earlier format has five underscores) or is not ours at all; either way it
// can never be looked up again, so it is dead weight.
static bool L2NameIsCurrentFormat(const char* name)
{
	size_t len = strlen(name);
	if (len < 5) return false;
	if (_stricmp(name + len - 4, ".bin") != 0) return false;

	int underscores = 0;
	for (size_t i = 0; i < len - 4; ++i)
	{
		char c = name[i];
		if (c == '_') { underscores++; continue; }
		if (c == '-') continue;
		if (c >= '0' && c <= '9') continue;
		if (c >= 'a' && c <= 'f') continue;
		return false;
	}
	return underscores == 6;
}

// Deletes "<key>.bin.<tid>.tmp" leftovers (and the "<key>.bin.tmp" of builds
// before the thread id was added) older than this. A temp file is never a
// valid cache entry: it only exists between the write and the rename, so
// anything this old is the residue of a crash or a full disk.
static const long long L2_TMP_MAX_AGE_100NS = 60LL * 10000000LL;   // 60 s

static void L2SweepTempFiles()
{
	FILETIME nowFt;
	GetSystemTimeAsFileTime(&nowFt);
	ULARGE_INTEGER now;
	now.LowPart = nowFt.dwLowDateTime;
	now.HighPart = nowFt.dwHighDateTime;

	char pattern[MAX_PATH];
	if (!L2FormatPath(pattern, sizeof(pattern), "%s*.tmp", nmDiskCacheDirBuf))
		return;

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		ULARGE_INTEGER wt;
		wt.LowPart = fd.ftLastWriteTime.dwLowDateTime;
		wt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		if (now.QuadPart <= wt.QuadPart)
			continue;   // written in the future (clock skew): leave it
		if ((long long)(now.QuadPart - wt.QuadPart) < L2_TMP_MAX_AGE_100NS)
			continue;   // a write may be in flight right now

		char full[MAX_PATH];
		if (!L2FormatPath(full, sizeof(full), "%s%s", nmDiskCacheDirBuf, fd.cFileName))
			continue;
		if (DeleteFileA(full))
			InterlockedIncrement(&l2CapEvicted);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static void L2ScanAndEvict(long long cap)
{
	if (!nmDiskCacheDirBuf[0]) return;

	L2SweepTempFiles();

	const int MAX_RECORDS = 8192;
	L2FileRec* recs = (L2FileRec*)malloc(sizeof(L2FileRec) * MAX_RECORDS);
	if (!recs) return;

	char pattern[MAX_PATH];
	if (!L2FormatPath(pattern, sizeof(pattern), "%s*.bin", nmDiskCacheDirBuf))
	{ free(recs); return; }

	int n = 0;
	long long total = 0;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			// Unreachable names go now, whatever the directory total is.
			if (!L2NameIsCurrentFormat(fd.cFileName))
			{
				char full[MAX_PATH];
				if (!L2FormatPath(full, sizeof(full), "%s%s", nmDiskCacheDirBuf, fd.cFileName))
					continue;
				if (DeleteFileA(full))
				{
					InterlockedIncrement(&l2CapEvicted);
					continue;
				}
			}

			total += (long long)fd.nFileSizeLow;
			if (n < MAX_RECORDS && strlen(fd.cFileName) < sizeof(recs[0].name))
			{
				recs[n].writeTime = fd.ftLastWriteTime;
				recs[n].size = fd.nFileSizeLow;
				strcpy_s(recs[n].name, sizeof(recs[n].name), fd.cFileName);
				n++;
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	if (total > cap && n > 0)
	{
		qsort(recs, (size_t)n, sizeof(L2FileRec), L2RecCompare);
		long long target = (cap / 4) * 3;   // drop to 75% of the cap
		char full[MAX_PATH];
		for (int i = 0; i < n && total > target; ++i)
		{
			if (!L2FormatPath(full, sizeof(full), "%s%s", nmDiskCacheDirBuf, recs[i].name))
				continue;
			if (DeleteFileA(full))
			{
				total -= (long long)recs[i].size;
				InterlockedIncrement(&l2CapEvicted);
			}
		}
	}

	free(recs);
	InterlockedExchange64(&g_l2DirBytes, (LONGLONG)total);
	InterlockedExchange(&g_l2SizeKnown, 1);
}

static void L2EnforceCap(size_t justWrote)
{
	long long cap = (long long)cfg_navmeshDiskCacheMaxMB * 1024LL * 1024LL;
	if (cap <= 0) return;

	if (InterlockedCompareExchange(&g_l2SizeKnown, 0, 0) != 0)
	{
		long long total = (long long)InterlockedExchangeAdd64(&g_l2DirBytes, (LONGLONG)justWrote)
		                + (long long)justWrote;
		if (total <= cap)
			return;
	}

	// First write of the session, or over the cap: rescan and evict. Only one
	// thread at a time — the rest let the next write retry.
	if (InterlockedCompareExchange(&g_l2EvictBusy, 1, 0) != 0)
		return;
	L2ScanAndEvict(cap);
	InterlockedExchange(&g_l2EvictBusy, 0);
}

void L2StartupScan()
{
	long long cap = (long long)cfg_navmeshDiskCacheMaxMB * 1024LL * 1024LL;
	if (cap <= 0) return;
	if (InterlockedCompareExchange(&g_l2EvictBusy, 1, 0) != 0) return;
	L2ScanAndEvict(cap);
	InterlockedExchange(&g_l2EvictBusy, 0);
}


// --------------------------------------------------------------------
// Writing
// --------------------------------------------------------------------

static bool L2EntryConsistent(const NavMeshCacheEntry& e)
{
	if (e.faceCount     < 0 || e.faceCount     > L2_MAX_FACES)    return false;
	if (e.edgeCount     < 0 || e.edgeCount     > L2_MAX_EDGES)    return false;
	if (e.vertexCount   < 0 || e.vertexCount   > L2_MAX_VERTICES) return false;
	if (e.faceDataCount < 0 || e.faceDataCount > L2_MAX_FACEDATA) return false;
	if (e.edgeDataCount < 0 || e.edgeDataCount > L2_MAX_EDGEDATA) return false;
	if (e.faceCount     > 0 && !e.cachedFaces)    return false;
	if (e.edgeCount     > 0 && !e.cachedEdges)    return false;
	if (e.vertexCount   > 0 && !e.cachedVertices) return false;
	if (e.faceDataCount > 0 && !e.cachedFaceData) return false;
	if (e.edgeDataCount > 0 && !e.cachedEdgeData) return false;
	return true;
}

static long long L2PayloadLength(int faces, int edges, int verts, int faceData, int edgeData)
{
	return (long long)faces    * HKAI_FACE_SIZE
	     + (long long)edges    * HKAI_EDGE_SIZE
	     + (long long)verts    * HKAI_VERTEX_SIZE
	     + (long long)faceData * HKAI_FACEDATA_UNIT
	     + (long long)edgeData * HKAI_EDGEDATA_UNIT;
}

void FreeDiskCacheBlob(L2WriteBlob* blob)
{
	if (!blob) return;
	if (blob->data) free(blob->data);
	blob->data = NULL;
	blob->size = 0;
}

bool BuildDiskCacheBlob(const NavMeshCacheKey& key, const NavMeshCacheEntry& e, L2WriteBlob* out)
{
	if (!out) return false;
	out->key = key;
	out->data = NULL;
	out->size = 0;

	// When the real WB's generation settings differ from the tables
	// the settings hash describes, this mesh does not match its cache key.
	// The only producer of L2 bytes, so refusing here stops every write.
	if (L2Bypassed()) { InterlockedIncrement(&nmL2BypassWrites); return false; }

	if (!e.valid || !L2EntryConsistent(e))
	{ L2NoteWriteFailure(L2WR_BADENTRY, 0, NULL); return false; }

	// A zero-face mesh never reaches L2, whatever L1 did with it.
	// This is the only producer of blob bytes (WriteDiskCacheBlob writes nothing
	// else, and ProcessNavMeshJob only writes a blob this function filled), so
	// refusing here is the whole rule. StoreCacheEntry already refuses a
	// zero-face mesh and the only caller builds from a published L1
	// slot, so this cannot fire; it makes the L2 rule independent of that, and
	// l2ZeroSkip= on the stats line says if it ever does. Nothing zero-face is
	// ever written. Old zero-face files already on disk are
	// still refused on the read side (PromoteDiskEntryToL1), not deleted.
	if (e.faceCount <= 0)
	{
		InterlockedIncrement(&nmL2ZeroFaceSkip);
		return false;
	}

	long long payload = L2PayloadLength(e.faceCount, e.edgeCount, e.vertexCount,
	                                    e.faceDataCount, e.edgeDataCount);
	if (payload < 0 || payload > 0x7FFFFFFFLL)
	{ L2NoteWriteFailure(L2WR_PAYLOAD, 0, NULL); return false; }

	size_t total = sizeof(L2Header) + (size_t)payload;
	unsigned char* buf = (unsigned char*)malloc(total);
	if (!buf)
	{ L2NoteWriteFailure(L2WR_ALLOC, 0, NULL); return false; }

	unsigned char* p = buf + sizeof(L2Header);
	if (e.faceCount > 0)
	{ size_t sz = (size_t)e.faceCount * HKAI_FACE_SIZE; memcpy(p, e.cachedFaces, sz); p += sz; }
	if (e.edgeCount > 0)
	{ size_t sz = (size_t)e.edgeCount * HKAI_EDGE_SIZE; memcpy(p, e.cachedEdges, sz); p += sz; }
	if (e.vertexCount > 0)
	{ size_t sz = (size_t)e.vertexCount * HKAI_VERTEX_SIZE; memcpy(p, e.cachedVertices, sz); p += sz; }
	if (e.faceDataCount > 0)
	{ size_t sz = (size_t)e.faceDataCount * HKAI_FACEDATA_UNIT; memcpy(p, e.cachedFaceData, sz); p += sz; }
	if (e.edgeDataCount > 0)
	{ size_t sz = (size_t)e.edgeDataCount * HKAI_EDGEDATA_UNIT; memcpy(p, e.cachedEdgeData, sz); p += sz; }

	L2Header* h = (L2Header*)buf;
	memset(h, 0, sizeof(L2Header));
	h->magic            = L2_CACHE_MAGIC;
	h->version          = L2_CACHE_VERSION;
	h->settingsHash     = L2SettingsHash();
	h->payloadLen       = (unsigned int)payload;
	h->faceCount        = e.faceCount;
	h->edgeCount        = e.edgeCount;
	h->vertexCount      = e.vertexCount;
	h->faceDataCount    = e.faceDataCount;
	h->edgeDataCount    = e.edgeDataCount;
	h->faceDataStriding = e.faceDataStriding;
	h->edgeDataStriding = e.edgeDataStriding;
	h->navMeshFlags     = (unsigned int)e.navMeshFlags;
	h->erosionRadius    = e.erosionRadius;
	h->userData         = e.userData;
	memcpy(h->aabb, e.aabb, 32);

	// The checksum covers everything after its own slot: the counts, the
	// stridings, the flags, the erosion radius, the user data and the AABB, and
	// then the payload. Those metadata fields are what the reader builds the
	// navmesh from, so they have to be checksummed too. The region is
	// contiguous here, so one pass over it does the job.
	h->payloadCrc = L2Crc32(buf + L2_CRC_REGION_START, total - L2_CRC_REGION_START);

	out->data = buf;
	out->size = total;
	return true;
}

bool WriteDiskCacheBlob(L2WriteBlob* blob)
{
	if (!blob || !blob->data || blob->size == 0)
	{ L2NoteWriteFailure(L2WR_BADBLOB, 0, NULL); FreeDiskCacheBlob(blob); return false; }

	char path[MAX_PATH];
	GetDiskCachePath(blob->key, path, sizeof(path));

	unsigned long osErr = 0;
	L2WriteOutcome outcome = L2WriteFileAtomic(nmDiskCacheDirBuf, path,
	                                           blob->data, blob->size, &osErr);
	if (outcome != L2WR_OK)
	{
		L2NoteWriteFailure(outcome, osErr, path);
		FreeDiskCacheBlob(blob);
		return false;
	}

	size_t written = blob->size;
	NavMeshCacheKey key = blob->key;
	FreeDiskCacheBlob(blob);

	InterlockedIncrement(&nmDiskWriteCount);

	long wIdx = InterlockedCompareExchange(&nmDiskWriteCount, 0, 0);
	if (wIdx <= L2_MISS_LOG_MAX)
	{
		long idx = InterlockedIncrement(&l2MissLogCount) - 1;
		if (idx < L2_MISS_LOG_MAX)
		{
			l2MissLog[idx].gridX = key.gridX;
			l2MissLog[idx].gridY = key.gridY;
			l2MissLog[idx].tileId = key.sectionTileId;
			l2MissLog[idx].jobType = key.jobType;
			l2MissLog[idx].aabbHash = key.aabbHash;
			l2MissLog[idx].buildingHash = key.buildingHash;
			l2MissLog[idx].thingsCount = -2;  // sentinel: write entry
		}
	}

	L2EnforceCap(written);
	return true;
}


// --------------------------------------------------------------------
// Reading
// --------------------------------------------------------------------

static void L2FreeEntryArrays(NavMeshCacheEntry& e)
{
	if (e.cachedFaces)     { fn_gameDelArr(e.cachedFaces);     e.cachedFaces = NULL; }
	if (e.cachedEdges)     { fn_gameDelArr(e.cachedEdges);     e.cachedEdges = NULL; }
	if (e.cachedVertices)  { fn_gameDelArr(e.cachedVertices);  e.cachedVertices = NULL; }
	if (e.cachedFaceData)  { fn_gameDelArr(e.cachedFaceData);  e.cachedFaceData = NULL; }
	if (e.cachedEdgeData)  { fn_gameDelArr(e.cachedEdgeData);  e.cachedEdgeData = NULL; }
	e.valid = false;
}

// Copies count * unit bytes out of the payload into a fresh game array and
// advances the cursor. A zero count leaves the pointer NULL and succeeds.
static bool L2TakeArray(const unsigned char* payload, size_t* cursor,
                        int count, int unit, void** dest)
{
	*dest = NULL;
	if (count <= 0) return true;
	size_t sz = (size_t)count * (size_t)unit;
	void* buf = fn_gameNewArr(sz);
	if (!buf) return false;
	memcpy(buf, payload + *cursor, sz);
	*cursor += sz;
	*dest = buf;
	return true;
}

bool ReadDiskCache(const NavMeshCacheKey& key, NavMeshCacheEntry& out)
{
	memset(&out, 0, sizeof(out));
	out.valid = false;

	// L2 off for the session (nm_cache_core.h). A plain miss,
	// not a rejection: the file may be perfectly good for the settings its key
	// names, it is this session's generation that no longer matches.
	if (L2Bypassed()) { InterlockedIncrement(&nmL2BypassReads); return false; }

	char path[MAX_PATH];
	GetDiskCachePath(key, path, sizeof(path));
	// No path means the lookup could not even be attempted: the cache directory
	// was never established, or this key's name did not fit. Counted, because a
	// read that never happens is otherwise indistinguishable from a plain miss.
	if (!path[0]) { L2Reject(L2REJ_PATHLONG); return false; }

	FILE* f = NULL;
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return false;   // no such file: a plain miss, not a rejection

	// File size first: the header's length field is checked against it.
	long fileSize = 0;
	if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
	{ fclose(f); L2Reject(L2REJ_IO); return false; }

	L2Header h;
	if (fread(&h, sizeof(h), 1, f) != 1)
	{ fclose(f); L2Reject(L2REJ_IO); return false; }

	if (h.magic != L2_CACHE_MAGIC)
	{ fclose(f); L2Reject(L2REJ_MAGIC); return false; }
	if (h.version != L2_CACHE_VERSION)
	{ fclose(f); L2Reject(L2REJ_VERSION); return false; }
	if (h.settingsHash != L2SettingsHash())
	{ fclose(f); L2Reject(L2REJ_SETTINGS); return false; }

	if (h.faceCount        < 0 || h.faceCount        > L2_MAX_FACES    ||
	    h.edgeCount        < 0 || h.edgeCount        > L2_MAX_EDGES    ||
	    h.vertexCount      < 0 || h.vertexCount      > L2_MAX_VERTICES ||
	    h.faceDataCount    < 0 || h.faceDataCount    > L2_MAX_FACEDATA ||
	    h.edgeDataCount    < 0 || h.edgeDataCount    > L2_MAX_EDGEDATA ||
	    h.faceDataStriding < 0 || h.faceDataStriding > L2_MAX_STRIDING ||
	    h.edgeDataStriding < 0 || h.edgeDataStriding > L2_MAX_STRIDING)
	{ fclose(f); L2Reject(L2REJ_BOUNDS); return false; }

	long long expected = L2PayloadLength(h.faceCount, h.edgeCount, h.vertexCount,
	                                     h.faceDataCount, h.edgeDataCount);
	if (expected != (long long)h.payloadLen ||
	    (long long)fileSize != (long long)sizeof(L2Header) + expected)
	{ fclose(f); L2Reject(L2REJ_LENGTH); return false; }

	unsigned char* payload = NULL;
	if (h.payloadLen > 0)
	{
		payload = (unsigned char*)malloc(h.payloadLen);
		if (!payload)
		{ fclose(f); L2Reject(L2REJ_ALLOC); return false; }
		if (fread(payload, h.payloadLen, 1, f) != 1)
		{ free(payload); fclose(f); L2Reject(L2REJ_IO); return false; }
	}
	fclose(f);

	// CRC before anything is built from the bytes. The region is the header
	// tail (the counts, stridings, flags, erosion radius, user data and AABB —
	// everything the entry below is built from) followed by the payload; those
	// sit in two buffers here, so the running form walks both.
	{
		unsigned int crc = L2Crc32Begin();
		crc = L2Crc32Update(crc, (const unsigned char*)&h + L2_CRC_REGION_START,
		                    sizeof(L2Header) - L2_CRC_REGION_START);
		crc = L2Crc32Update(crc, payload, (size_t)h.payloadLen);
		if (L2Crc32End(crc) != h.payloadCrc)
		{ if (payload) free(payload); L2Reject(L2REJ_CRC); return false; }
	}

	// All-or-nothing: any allocation failure discards the whole entry.
	size_t cursor = 0;
	bool ok = true;
	if (ok) ok = L2TakeArray(payload, &cursor, h.faceCount,     HKAI_FACE_SIZE,     &out.cachedFaces);
	if (ok) ok = L2TakeArray(payload, &cursor, h.edgeCount,     HKAI_EDGE_SIZE,     &out.cachedEdges);
	if (ok) ok = L2TakeArray(payload, &cursor, h.vertexCount,   HKAI_VERTEX_SIZE,   &out.cachedVertices);
	if (ok) ok = L2TakeArray(payload, &cursor, h.faceDataCount, HKAI_FACEDATA_UNIT, &out.cachedFaceData);
	if (ok) ok = L2TakeArray(payload, &cursor, h.edgeDataCount, HKAI_EDGEDATA_UNIT, &out.cachedEdgeData);

	if (payload) free(payload);

	if (!ok)
	{
		L2FreeEntryArrays(out);
		L2Reject(L2REJ_ALLOC);
		return false;
	}

	out.key              = key;
	out.faceCount        = h.faceCount;
	out.edgeCount        = h.edgeCount;
	out.vertexCount      = h.vertexCount;
	out.faceDataCount    = h.faceDataCount;
	out.edgeDataCount    = h.edgeDataCount;
	out.faceDataStriding = h.faceDataStriding;
	out.edgeDataStriding = h.edgeDataStriding;
	out.navMeshFlags     = (unsigned char)(h.navMeshFlags & 0xFF);
	memcpy(out.aabb, h.aabb, 32);
	out.erosionRadius    = h.erosionRadius;
	out.userData         = h.userData;
	out.valid            = true;
	return true;
}

