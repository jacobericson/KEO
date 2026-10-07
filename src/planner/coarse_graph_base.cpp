// coarse_graph_base.cpp - The whole-map base, built on the mod's own thread from the shipped tiles
// or loaded from its disk cache; the save-path snapshot at a save load and the save-tile replace;
// and the planner's frame step.
//
// The builder thread makes no game call and takes no game lock: it reads tile files whole (each
// closed before it is parsed), publishes exterior blocks into the store and hands interior blocks
// to the main thread, which alone writes the interior uid table. Its one lock is the request slot's
// critical section, shared with the frame step.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/coarse_graph_base.h"
#include "planner/coarse_graph.h"
#include "planner/coarse_graph_cache.h"
#include "planner/coarse_graph_live.h"
#include "planner/planner_config.h"
#include "planner/plan_store.h"
#include "planner/planner_tick.h"
#include "planner/planner_water.h"
#include "planner/planner_acid.h"
#include "planner/planner_prearrival.h"
#include "game/game.h"
#include "base/core.h"
#include "zone/readiness/readiness_bindings.h"
#include <windows.h>
#include <stdio.h>
#include <exception>
#include <sstream>
#include <string>
#include <vector>
#include "base/klib_include.h"
#include <kenshi/SaveFileSystem.h>
#include "base/klib_include_end.h"

namespace planner {

namespace coarse_graph_base_detail {

const int    SAVE_LAYERS_MAX = 8;
const size_t CACHE_MAX_BYTES = (size_t)512 << 20;
const double PATHS_POLL_MS   = 250.0;

// One save replace: the store generation it belongs to and the save's folder chain, absolute,
// first layer first.
struct SaveRequest
{
	unsigned storeGen;
	int      layers;
	int      layersCut;     // a ninth and later layer, or one whose path does not fit MAX_PATH
	wchar_t  path[SAVE_LAYERS_MAX][MAX_PATH];
};

struct Win32File { HANDLE h; };

struct TileName { int gx, gy; std::wstring name; };

// LoadTileFile's result: whether the tile extracted, and whether its parse was a tail stop.
enum TileLoad { TL_OK = 0, TL_OK_TAIL, TL_FAILED, TL_FAILED_TAIL };

// One pass's totals, for its log line.
struct PassTotals
{
	int tiles, failed, tailStops, clusters, arcs, borders, arcsTrunc, maxNodes, replaced, handedOff, outranked;
	int interiorsDropped, bordersSkipped;
	int waterNodes;   // nodes with a non-zero water byte
};

} // namespace coarse_graph_base_detail
using namespace coarse_graph_base_detail;

// The request slot: written by the frame step, taken by the builder, both under s_requestCS
// (initialised by the store's start step; taken under no other lock).
static CRITICAL_SECTION s_requestCS;
static HANDLE           s_requestEvent = NULL;
static SaveRequest      s_request;
static bool             s_requestPending = false;
static volatile LONG    s_builderRunning = 0;
static volatile LONG    s_tilesPublished = 0;
static volatile LONG    s_tilesFound = 0;

// The frame step's state; main thread only.
static void*            s_lastZoneMgr = NULL;
static bool             s_wasLoading = false;
static bool             s_snapshotArmed = false;
static unsigned         s_lastPathsCount = 0;
static LONGLONG         s_lastPollQpc = 0;
static SaveRequest      s_staging;

// ---- File reads (builder thread) -------------------------------------------------------------

static bool Win32Open(void* ctx, const wchar_t* path)
{
	Win32File* f = (Win32File*)ctx;
	f->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	return f->h != INVALID_HANDLE_VALUE;
}

static bool Win32Size(void* ctx, unsigned __int64* out)
{
	LARGE_INTEGER li;
	if (!GetFileSizeEx(((Win32File*)ctx)->h, &li) || li.QuadPart < 0)
		return false;
	*out = (unsigned __int64)li.QuadPart;
	return true;
}

static bool Win32Read(void* ctx, void* buf, size_t n)
{
	HANDLE h = ((Win32File*)ctx)->h;
	unsigned char* p = (unsigned char*)buf;
	while (n > 0)
	{
		DWORD want = n > (size_t)(1u << 30) ? (DWORD)(1u << 30) : (DWORD)n;
		DWORD got = 0;
		if (!ReadFile(h, p, want, &got, NULL) || got == 0)
			return false;
		p += got;
		n -= got;
	}
	return true;
}

static void Win32Close(void* ctx)
{
	Win32File* f = (Win32File*)ctx;
	if (f->h != INVALID_HANDLE_VALUE)
		CloseHandle(f->h);
	f->h = INVALID_HANDLE_VALUE;
}

static bool ReadWholeFile(const wchar_t* path, size_t maxBytes, std::vector<unsigned char>* out)
{
	Win32File f = { INVALID_HANDLE_VALUE };
	CgFileOps ops = { &f, Win32Open, Win32Size, Win32Read, Win32Close };
	return CgReadWhole(ops, path, maxBytes, out);
}

// One tile file: read whole and closed, then parsed and extracted. Builder thread.
static int LoadTileFile(const wchar_t* path, int gx, int gy, TileGraph* g)
{
	std::vector<unsigned char> bytes;
	if (!ReadWholeFile(path, TF_MAX_FILE_BYTES, &bytes) || bytes.empty())
		return TL_FAILED;
	TfDoc doc;
	TfResult parsed = TfParse(&bytes[0], bytes.size(), &doc);
	bool tail = parsed == TF_TAIL_STOP;
	if (parsed != TF_OK && !tail)
		return TL_FAILED;
	if (TgExtract(doc, gx, gy, g) != TG_OK)
		return tail ? TL_FAILED_TAIL : TL_FAILED;
	return tail ? TL_OK_TAIL : TL_OK;
}

// Every tile<x>.<y>.hkt in dir (which ends in a separator) with 0 <= x, y < 64. findError, when not
// NULL, receives the find's error code when it found nothing at all, else 0.
static std::vector<TileName> ListTiles(const std::wstring& dir, DWORD* findError)
{
	std::vector<TileName> out;
	WIN32_FIND_DATAW fd;
	std::wstring pattern = dir + L"tile*.hkt";
	HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
	if (findError)
		*findError = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
	{
		int gx = -1, gy = -1;
		wchar_t canon[64];
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || swscanf_s(fd.cFileName, L"tile%d.%d.hkt", &gx, &gy) != 2
		    || gx < 0 || gx >= 64 || gy < 0 || gy >= 64)
			continue;
		swprintf_s(canon, L"tile%d.%d.hkt", gx, gy);
		if (_wcsicmp(canon, fd.cFileName) != 0)
			continue;
		TileName t;
		t.gx = gx;
		t.gy = gy;
		t.name = fd.cFileName;
		out.push_back(t);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

// Builds each section's block: an exterior one published into its tile's entry, an interior one
// handed to the main thread. Builder thread. False when a block could not be allocated.
static bool PublishTile(const TileGraph& g, int gx, int gy, int source, unsigned gen, PassTotals* t)
{
	int dir = CgExteriorIndex(gx, gy);
	t->interiorsDropped += g.interiorsDropped;
	t->bordersSkipped += g.bordersSkipped;
	for (size_t s = 0; s < g.sections.size(); ++s)
	{
		CgBlock* b = CgBlockFromTile(g, (int)s, source, gen);
		if (!b)
			return false;
		t->clusters += b->nodeCount;
		for (int k = 0; k < b->nodeCount; ++k)
			t->waterNodes += b->nodes[k].water != 0 ? 1 : 0;
		t->arcs += b->arcCount;
		t->borders += b->borderCount;
		t->arcsTrunc += b->arcsTrunc;
		if (b->nodeCount > t->maxNodes)
			t->maxNodes = b->nodeCount;
		if (g.sections[s].kind == TGS_INTERIOR)
		{
			CgHandOffInterior(b);
			t->handedOff++;
			continue;
		}
		CgPublishResult r = source == CG_BASE ? CgPublishBase(dir, b) : CgPublishOver(dir, b);
		if (r == CGP_OK)
			t->replaced++;
		else if (r == CGP_OUTRANKED)
			t->outranked++;
	}
	return true;
}

static void CountLoad(int r, PassTotals* t)
{
	if (r == TL_OK_TAIL || r == TL_FAILED_TAIL)
		t->tailStops++;
	if (r == TL_FAILED || r == TL_FAILED_TAIL)
		t->failed++;
}

static long UnsearchableSoFar()
{
	CgStats s;
	CgStatsGet(&s);
	return s.unsearchable;
}

// ---- The data root (main thread, then the builder) ----------------------------------------------

// The folder holding the game's data folder, ending in a separator, and which candidate gave it.
// The main thread writes both once, before it starts the builder thread, which reads them only
// after: the thread start orders the writes before the reads, so neither takes a lock.
static wchar_t     s_dataRoot[MAX_PATH];
static const char* s_dataRootSource = "none";

// Takes root (ending in a separator) as the data root when it holds the shipped navtiles folder.
// Main thread.
static bool TakeDataRoot(const wchar_t* root, const char* source)
{
	wchar_t dir[MAX_PATH];
	if (_snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%lsdata\\newland\\land\\navtiles\\", root) < 0)
		return false;
	DWORD a = GetFileAttributesW(dir);
	if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return false;
	memcpy(s_dataRoot, root, sizeof(s_dataRoot[0]) * (wcslen(root) + 1));
	s_dataRootSource = source;
	return true;
}

// The first candidate that holds the navtiles folder: the working directory (the game's own data
// paths are relative to it), the exe's folder, then the plugin folder's grandparent. None leaves
// the root empty. Main thread, before the builder thread starts.
static void ResolveDataRoot()
{
	wchar_t cand[MAX_PATH];
	DWORD n = GetCurrentDirectoryW(MAX_PATH, cand);
	if (n > 0 && n < MAX_PATH - 1)
	{
		if (cand[n - 1] != L'\\' && cand[n - 1] != L'/')
		{
			cand[n] = L'\\';
			cand[n + 1] = 0;
		}
		if (TakeDataRoot(cand, "cwd"))
			return;
	}
	n = GetModuleFileNameW(NULL, cand, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
	{
		DWORD cut = n;
		while (cut > 0 && cand[cut - 1] != L'\\' && cand[cut - 1] != L'/')
			--cut;
		cand[cut] = 0;
		if (cut > 0 && TakeDataRoot(cand, "exe"))
			return;
	}
	std::string dll = GetDLLDirectory();
	wchar_t wide[MAX_PATH];
	if (!dll.empty() && MultiByteToWideChar(CP_ACP, 0, dll.c_str(), -1, wide, MAX_PATH)
	    && _snwprintf_s(cand, MAX_PATH, _TRUNCATE, L"%ls..\\..\\", wide) >= 0)
		TakeDataRoot(cand, "dll");
}

// The root line, once per session before the base line. Builder thread.
static void LogDataRoot(const std::wstring& root, size_t listed, DWORD findError)
{
	char narrow[MAX_PATH * 2];
	if (root.empty())
		_snprintf_s(narrow, sizeof(narrow), _TRUNCATE, "-");
	else if (!WideCharToMultiByte(CP_ACP, 0, root.c_str(), -1, narrow, (int)sizeof(narrow), NULL, NULL))
		_snprintf_s(narrow, sizeof(narrow), _TRUNCATE, "?");
	char line[MAX_PATH * 2 + 96];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "Planner base: root=%s source=%s listed=%d err=%lu",
	            narrow, s_dataRootSource, (int)listed, (unsigned long)findError);
	LogMsgDeferrable(line);
}

// ---- The base (builder thread) -----------------------------------------------------------------

static std::wstring CacheFolder()
{
	std::string dll = GetDLLDirectory();
	wchar_t wide[MAX_PATH];
	if (dll.empty() || !MultiByteToWideChar(CP_ACP, 0, dll.c_str(), -1, wide, MAX_PATH))
		return std::wstring();
	return std::wstring(wide) + L"planner_cache\\";
}

// The cache written whole to a temporary file, then moved over the old one.
static bool WriteCache(const std::wstring& path, const std::vector<CgCacheIndexEntry>& entries,
                       const std::vector<unsigned char>& payload)
{
	std::vector<unsigned char> file;
	CgCacheBuild(entries, payload, &file);
	std::wstring tmp = path + L".tmp";
	FILE* f = NULL;
	if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f)
		return false;
	bool ok = fwrite(&file[0], 1, file.size(), f) == file.size();
	ok = fflush(f) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok)
	{
		DeleteFileW(tmp.c_str());
		return false;
	}
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static void BuildBase()
{
	LONGLONG t0 = QpcNow();
	std::wstring game(s_dataRoot);
	DWORD findError = 0;
	std::vector<TileName> names = game.empty() ? std::vector<TileName>()
	                                           : ListTiles(game + L"data\\newland\\land\\navtiles\\", &findError);
	InterlockedExchange(&s_tilesFound, (LONG)names.size());
	LogDataRoot(game, names.size(), findError);
	std::wstring cacheDir = CacheFolder();
	std::wstring cachePath = cacheDir + L"coarse_base.bin";
	if (!cacheDir.empty())
		CreateDirectoryW(cacheDir.c_str(), NULL);
	std::vector<unsigned char> old;
	bool valid = !cacheDir.empty() && ReadWholeFile(cachePath.c_str(), CACHE_MAX_BYTES, &old) && !old.empty()
	          && CgCacheValidate(&old[0], old.size()) == CGC_OK;
	const CgCacheHeader* oldHead = valid ? (const CgCacheHeader*)&old[0] : NULL;

	PassTotals t;
	memset(&t, 0, sizeof(t));
	int hits = 0;
	bool changed = !valid;
	std::vector<CgCacheIndexEntry> entries;
	std::vector<unsigned char> payload;
	for (size_t i = 0; i < names.size(); ++i)
	{
		const TileName& tn = names[i];
		std::wstring path = game + L"data\\newland\\land\\navtiles\\" + tn.name;
		WIN32_FILE_ATTRIBUTE_DATA fa;
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeHigh != 0)
		{
			t.failed++;
			continue;
		}
		CgCacheIndexEntry e;
		memset(&e, 0, sizeof(e));
		e.gx = (short)tn.gx;
		e.gy = (short)tn.gy;
		e.fileSize = fa.nFileSizeLow;
		e.mtime = ((unsigned __int64)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
		e.payloadOffset = (unsigned)payload.size();
		TileGraph g;
		const CgCacheIndexEntry* hit = valid ? CgCacheFind(&old[0], old.size(), tn.gx, tn.gy, e.fileSize, e.mtime) : NULL;
		const unsigned char* rec = hit ? &old[oldHead->payloadOffset + hit->payloadOffset] : NULL;
		if (hit && CgCacheDecodeTile(rec, hit->payloadBytes, &g))
		{
			hits++;
			if (CgCacheTileFlags(rec, hit->payloadBytes) & 1u)
				t.tailStops++;
			payload.insert(payload.end(), rec, rec + hit->payloadBytes);
		}
		else
		{
			int r = LoadTileFile(path.c_str(), tn.gx, tn.gy, &g);
			CountLoad(r, &t);
			if (r == TL_FAILED || r == TL_FAILED_TAIL)
				continue;
			CgCacheEncodeTile(g, &payload, r == TL_OK_TAIL ? 1u : 0u);
			changed = true;
		}
		e.payloadBytes = (unsigned)payload.size() - e.payloadOffset;
		e.sectionCount = (unsigned)g.sections.size();
		entries.push_back(e);
		if (!PublishTile(g, tn.gx, tn.gy, CG_BASE, 0, &t))
		{
			t.failed++;
			continue;
		}
		t.tiles++;
		InterlockedIncrement(&s_tilesPublished);
	}
	if (valid && oldHead->tileCount != (unsigned)entries.size())
		changed = true;
	const char* write = "skip";   // an empty listing never replaces the cache
	if (changed && names.size() > 0)
		write = !cacheDir.empty() && WriteCache(cachePath, entries, payload) ? "ok" : "fail";

	char line[448];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner base: tiles=%d/%d failed=%d tailStops=%d clusters=%d arcs=%d borders=%d arcsTrunc=%d ms=%d"
	            " cacheHits=%d cacheWrite=%s maxNodes=%d interiorsDropped=%d bordersSkipped=%d waterNodes=%d"
	            " unsearchable=%ld",
	            t.tiles, (int)names.size(), t.failed, t.tailStops, t.clusters, t.arcs, t.borders, t.arcsTrunc,
	            (int)QpcToMs(QpcNow() - t0), hits, write, t.maxNodes, t.interiorsDropped, t.bordersSkipped,
	            t.waterNodes, UnsearchableSoFar());
	LogMsgDeferrable(line);
}

// ---- The save replace (builder thread) -----------------------------------------------------------

static void ReplaceSaveTiles(const SaveRequest& req)
{
	LONGLONG t0 = QpcNow();
	PassTotals t;
	memset(&t, 0, sizeof(t));
	unsigned seen[CG_EXTERIOR_SLOTS / 32];
	memset(seen, 0, sizeof(seen));
	for (int layer = req.layers - 1; layer >= 0 && CgStoreGen() == req.storeGen; --layer)
	{
		std::wstring dir = std::wstring(req.path[layer]) + L"\\zone\\";
		std::vector<TileName> names = ListTiles(dir, NULL);
		for (size_t i = 0; i < names.size() && CgStoreGen() == req.storeGen; ++i)
		{
			int bit = CgExteriorIndex(names[i].gx, names[i].gy);
			if (seen[bit >> 5] & (1u << (bit & 31)))
				continue;                            // a later layer holds this tile
			seen[bit >> 5] |= 1u << (bit & 31);
			t.tiles++;
			TileGraph g;
			int r = LoadTileFile((dir + names[i].name).c_str(), names[i].gx, names[i].gy, &g);
			CountLoad(r, &t);
			if (r == TL_FAILED || r == TL_FAILED_TAIL)
				continue;
			if (!PublishTile(g, names[i].gx, names[i].gy, CG_SAVE, req.storeGen, &t))
				t.failed++;
		}
	}
	char line[448];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner base: save gen=%u layers=%d layersCut=%d tiles=%d failed=%d replaced=%d handedOff=%d"
	            " outranked=%d arcsTrunc=%d ms=%d tailStops=%d interiorsDropped=%d bordersSkipped=%d waterNodes=%d"
	            " unsearchable=%ld",
	            req.storeGen, req.layers, req.layersCut, t.tiles, t.failed, t.replaced, t.handedOff, t.outranked,
	            t.arcsTrunc, (int)QpcToMs(QpcNow() - t0), t.tailStops, t.interiorsDropped, t.bordersSkipped,
	            t.waterNodes, UnsearchableSoFar());
	LogMsgDeferrable(line);
}

static bool TakeRequest(SaveRequest* out)
{
	EnterCriticalSection(&s_requestCS);
	bool have = s_requestPending;
	if (have)
	{
		*out = s_request;
		s_requestPending = false;
	}
	LeaveCriticalSection(&s_requestCS);
	return have;
}

// The builder: the base, then one save replace per request whose generation is still current.
static DWORD WINAPI BuilderMain(void*)
{
	try
	{
		BuildBase();
	}
	catch (const std::exception&)
	{
		LogMsgDeferrable("Planner base: the build stopped on an allocation failure");
	}
	for (;;)
	{
		WaitForSingleObject(s_requestEvent, INFINITE);
		SaveRequest req;
		if (!TakeRequest(&req))
			continue;
		if (req.storeGen != CgStoreGen())
		{
			char line[128];
			_snprintf_s(line, sizeof(line), _TRUNCATE, "Planner base: save gen=%u dropped, the store is at gen=%u",
			            req.storeGen, CgStoreGen());
			LogMsgDeferrable(line);
			continue;
		}
		try
		{
			ReplaceSaveTiles(req);
		}
		catch (const std::exception&)
		{
			LogMsgDeferrable("Planner base: a save replace stopped on an allocation failure");
		}
	}
}

// ---- The main thread ---------------------------------------------------------------------------

void PlannerBaseStartStep(int* installed, int* total)
{
	if (g_plannerCfg.mode == PLANNER_OFF) return;
	(void)installed;
	(void)total;
	if (!CgStoreCreate())
	{
		LogError("Planner: store allocation failed; the planner is off for this session");
		g_plannerCfg.mode = PLANNER_OFF;
		return;
	}
	PlanStoreArm(g_plannerCfg.mode);
	if (!PlannerTickArm())
	{
		LogError("Planner: search scratch allocation failed; the planner is off for this session");
		g_plannerCfg.mode = PLANNER_OFF;
		PlanStoreArm(PLANNER_OFF);
	}
	// pre= follows mode=: the pre-arrival install runs before this step, so a planner turned off above reads
	// off there too (its detour then finds no plan).
	std::ostringstream arm;
	arm << "Planner arm: mode=" << PlannerModeName(g_plannerCfg.mode) << " legSpan=" << g_plannerCfg.legSpan
	    << " aheadTiles=" << g_plannerCfg.aheadTiles << " waitSeconds=" << g_plannerCfg.waitSeconds
	    << " baseBuild=" << g_plannerCfg.baseBuild << " water=" << PlannerWaterModeToken()
	    << " waterBind=" << PlannerWaterBindToken() << " waterEngine=" << PlannerWaterEngineToken()
	    << " acidBind=" << PlannerWaterAcidToken() << " acidCost=" << g_plannerCfg.acidCost
	    << " pre=" << (g_plannerCfg.mode == PLANNER_OFF ? std::string("off") : PlannerPreArrivalArmToken());
	LogMsg(arm.str());
	InitializeCriticalSection(&s_requestCS);
	if (g_plannerCfg.mode == PLANNER_OFF || !g_plannerCfg.baseBuild)
		return;
	ResolveDataRoot();
	s_requestEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE thread = s_requestEvent ? CreateThread(NULL, 0, BuilderMain, NULL, CREATE_SUSPENDED, NULL) : NULL;
	if (!thread)
	{
		LogError("Planner: the base builder thread did not start; the base stays empty");
		return;
	}
	SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
	ResumeThread(thread);
	CloseHandle(thread);
	InterlockedExchange(&s_builderRunning, 1);
}

// The save folder chain, copied under a try-shared take of the save system's current mutex (never
// a blocking take), then made absolute and posted to the builder outside it. Main thread. False
// while the lock is busy or the singleton is absent: the frame step tries again next frame.
static bool SnapshotSavePaths(unsigned gen)
{
	uintptr_t sfs = *(uintptr_t*)GameAddr(RVA_SAVE_FILE_SYSTEM);
	if (!sfs || !fn_boostUnlockShared)
		return false;
	volatile LONG* lock = (volatile LONG*)KLIB_MEMBER(2, sfs, SaveFileSystem_currentMutex, 0x1A0);
	if (!BoostTryLockShared(lock))
		return false;
	const lektor<std::string>* paths = (const lektor<std::string>*)KLIB_MEMBER(2, sfs, SaveFileSystem_paths, 0xF0);
	char narrow[SAVE_LAYERS_MAX][MAX_PATH];
	int copied = 0, cut = 0;
	unsigned count = paths->stuff ? paths->count : 0;
	for (unsigned i = 0; i < count; ++i)
	{
		const std::string& s = paths->stuff[i];
		if (copied == SAVE_LAYERS_MAX || s.empty() || s.size() >= MAX_PATH)
		{
			++cut;
			continue;
		}
		memcpy(narrow[copied], s.c_str(), s.size() + 1);
		++copied;
	}
	fn_boostUnlockShared((void*)lock);
	s_lastPathsCount = count;

	SaveRequest& r = s_staging;
	r.storeGen = gen;
	r.layers = 0;
	r.layersCut = cut;
	for (int i = 0; i < copied; ++i)
	{
		wchar_t wide[MAX_PATH];
		DWORD n = MultiByteToWideChar(CP_ACP, 0, narrow[i], -1, wide, MAX_PATH) ? GetFullPathNameW(wide, MAX_PATH, r.path[r.layers], NULL) : 0;
		if (n == 0 || n >= MAX_PATH)
			r.layersCut++;
		else
			r.layers++;
	}
	if (r.layers > 0 && s_builderRunning)
	{
		EnterCriticalSection(&s_requestCS);
		s_request = r;
		s_requestPending = true;
		LeaveCriticalSection(&s_requestCS);
		SetEvent(s_requestEvent);
	}
	return true;
}

void PlannerOnFrame(void* zoneMgr, bool saveLoading)
{
	if (!CgStoreReady()) return;
	(void)saveLoading;   // the ZM+8 byte is read here directly, for its rising edge
	bool loading = zoneMgr && *(const unsigned char*)KLIB_MEMBER(2, zoneMgr, ZoneManager_justLoadedAGame, 0x8) != 0;
	if (zoneMgr != s_lastZoneMgr || (loading && !s_wasLoading))
	{
		CgStoreNewWorld();
		PlannerWaterReset();
		PlannerAcidNewWorld();
		s_snapshotArmed = true;
	}
	s_lastZoneMgr = zoneMgr;
	s_wasLoading = loading;
	if (!loading)
		PlannerWaterAcidFrame(ElapsedSec());

	LONGLONG now = QpcNow();
	if (zoneMgr && QpcToMs(now - s_lastPollQpc) >= PATHS_POLL_MS)
	{
		s_lastPollQpc = now;
		uintptr_t sfs = *(uintptr_t*)GameAddr(RVA_SAVE_FILE_SYSTEM);
		unsigned count = sfs ? ((const lektor<std::string>*)KLIB_MEMBER(2, sfs, SaveFileSystem_paths, 0xF0))->count : 0;
		if (count != s_lastPathsCount)
			s_snapshotArmed = true;
	}
	if (s_snapshotArmed && SnapshotSavePaths(CgStoreGen()))
		s_snapshotArmed = false;

	CgDrainHandOff();
	CgPromoteLive(8);
	CgLiveReport(ElapsedSec());
	CgDrainRetired();
}

void PlannerBaseProgress(int* tiles, int* total)
{
	*tiles = (int)InterlockedCompareExchange(&s_tilesPublished, 0, 0);
	*total = (int)InterlockedCompareExchange(&s_tilesFound, 0, 0);
}

} // namespace planner
