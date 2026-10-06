// nm_force_rebuild.cpp - The rebuild-navmesh key: the force-mark table, the claim and
// finish, the forced job's cache clear, the cache line's token, the key's detour, the
// queue's front, the panel hold and its frame tick.
// Claims and finishes run on the NavMesh threads and neither allocate nor log; the press,
// the tick, the reset and the install run on the main thread.
#include "navmesh/nm_workers.h"
#include "plugin/hook_manifest.h"
#include "navmesh/nm_workers_internal.h"
#include "navmesh/jobs/nm_queue_lock.h"
#include "navmesh/cache/nm_force_rebuild.h"
#include "navmesh/cache/nm_force_rebuild_policy.h"
#include "zone/transition_hook.h"
#include <intrin.h>
#include <sstream>
#include <iomanip>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace nm_force_rebuild_detail {
struct LiveMark { int cell; long word; void* zone; };
struct PressCell { int cell; long word; void* zone; int skip; int verdict; };
enum PressVerdict { PV_SKIP = 0, PV_VANILLA, PV_QUEUED, PV_ALREADY, PV_INFLIGHT, PV_KEPT };
struct PressWork { PressCell c[NM_REBUILD_MAX_CELLS]; int count; bool key; bool centreNull;
                   NmRebuildSelection sel; uintptr_t nmg; LONGLONG now; int pending; };
typedef void (*generateZone_t)(void* navMesh, void* zone);
typedef void (*addJobZone_t)(void* nmg, void* zone, unsigned int hash);
typedef unsigned int (*hashZone_t)(void* zone);
}
using namespace nm_force_rebuild_detail;

static const int MARK_CELLS = NM_REBUILD_GRID * NM_REBUILD_GRID;

// Index gx * 64 + gy. The stamp is written before its word, both Interlocked.
static volatile LONG     s_markWord[MARK_CELLS];
static volatile LONGLONG s_markQpc[MARK_CELLS];

static volatile LONG s_marked, s_claimed, s_stored, s_refused, s_dropped, s_expired, s_lost;
static volatile LONG s_l1Evicted, s_l2Deleted, s_l2Absent, s_l2Failed;

// The press and the hold. Main thread unless marked.
static const int LIVE_CAP = 16;
static const int QUEUE_WALK_CAP = 65536;
static long      s_seq = 0;
static LiveMark  s_live[LIVE_CAP];
static int       s_liveCount = 0;
static PressCell s_press[NM_REBUILD_MAX_CELLS];   // the last key press's pending cells
static int       s_pressCount = 0;
static long      s_pressBase[5];                  // stored, refused, dropped, expired, lost at the press
static volatile LONG     s_holdActive = 0;        // any thread reads
static volatile LONGLONG s_holdPressQpc = 0;      // any thread reads
static volatile LONG     s_dismissOwed = 0;       // any thread
static volatile LONG     s_pressShowOpen = 0;     // a press showed the panel and no dismissal reached the game since
static bool s_inCall = false, s_tagKey = false, s_shown = false;
static volatile LONG s_heldDismissals, s_replays;
static generateZone_t s_origGenerate = NULL;
static addJobZone_t   s_addJob = NULL;
static hashZone_t     s_hashZone = NULL;

// The zone's grid cell as a table index; -1 off the grid.
static int CellIndex(uintptr_t zone)
{
	const int gx = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X));
	const int gy = *(int*)(KLIB_MEMBER(4, zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y));
	if (gx < 0 || gx >= NM_REBUILD_GRID || gy < 0 || gy >= NM_REBUILD_GRID)
		return -1;
	return gx * NM_REBUILD_GRID + gy;
}

static double AgeSec(LONGLONG from, LONGLONG now)
{
	return QpcToMs(now - from) / 1000.0;
}

int NmForceRebuildClaimLocked(const NmQueueLock&, uintptr_t zone, int jobType, long* wordOut)
{
	*wordOut = 0;
	if (!zone || jobType != 0)
		return -1;
	const int cell = CellIndex(zone);
	if (cell < 0)
		return -1;
	const LONG w = InterlockedCompareExchange(&s_markWord[cell], 0, 0);
	if (NmMarkStateOf(w) != NM_MARK_MARKED)
		return -1;   // nearly every claim: one read, no clock
	const double age = AgeSec(InterlockedCompareExchange64(&s_markQpc[cell], 0, 0), QpcNow());
	if (NmMarkClaimDecide(w, age, jobType) != NM_CLAIM_CONSUME)
		return -1;
	const LONG claimedWord = NmMarkWord(NmMarkSeqOf(w), NM_MARK_CLAIMED);
	if (InterlockedCompareExchange(&s_markWord[cell], claimedWord, w) != w)
		return -1;
	InterlockedIncrement(&s_claimed);
	*wordOut = claimedWord;
	return cell;
}

void NmForceRebuildFinish(int cell, long claimedWord, int outcome)
{
	if (cell < 0 || cell >= MARK_CELLS)
		return;
	InterlockedIncrement(outcome == NM_FORCE_STORED ? &s_stored : outcome == NM_FORCE_REFUSED ? &s_refused : &s_dropped);
	InterlockedCompareExchange(&s_markWord[cell], NmMarkWord(NmMarkSeqOf(claimedWord), NM_MARK_DONE), claimedWord);
}

static L2DeleteOutcome CountDelete(L2DeleteOutcome outcome)
{
	InterlockedIncrement(outcome == L2DEL_DELETED ? &s_l2Deleted : outcome == L2DEL_ABSENT ? &s_l2Absent : &s_l2Failed);
	return outcome;
}

bool NmForceRebuildClearKey(bool keyOk, const NavMeshCacheKey& key)
{
	if (!keyOk)
		return false;
	NmCacheLock cacheLock;
	const int evicted = EvictCacheEntriesForKey(cacheLock, key);
	cacheLock.Release();
	InterlockedExchangeAdd(&s_l1Evicted, evicted);
	return CountDelete(DeleteDiskCacheFile(key)) == L2DEL_FAILED;
}

void NmForceRebuildRetryDelete(const NavMeshCacheKey& key)
{
	CountDelete(DeleteDiskCacheFile(key));
}

void NmForceRebuildNoteEvicted(int n)
{
	if (n > 0)
		InterlockedExchangeAdd(&s_l1Evicted, n);
}

void NmForceRebuildFormatCacheToken(char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	const long claimed = InterlockedCompareExchange(&s_claimed, 0, 0);
	const long expired = InterlockedCompareExchange(&s_expired, 0, 0);
	const long lost = InterlockedCompareExchange(&s_lost, 0, 0);
	if (!claimed && !expired && !lost)
		return;
	_snprintf_s(out, outSize, _TRUNCATE, " forced=%ld/%ld/%ld/%ld/%ld/%ld forcedEvict=%ld/%ld/%ld/%ld",
	            claimed,
	            (long)InterlockedCompareExchange(&s_stored, 0, 0),
	            (long)InterlockedCompareExchange(&s_refused, 0, 0),
	            (long)InterlockedCompareExchange(&s_dropped, 0, 0),
	            expired, lost,
	            (long)InterlockedCompareExchange(&s_l1Evicted, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Deleted, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Absent, 0, 0),
	            (long)InterlockedCompareExchange(&s_l2Failed, 0, 0));
}

// ---------------------------------------------------------------------
// The press (main thread)
// ---------------------------------------------------------------------

static uintptr_t ZoneContent(void* zone)
{
	return zone ? *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)zone, ZoneMap_mapContent, OFF_ZONE_CONTENT)) : 0;
}

static uintptr_t ZoneTerrain(void* zone)
{
	return *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)zone, ZoneMap_terrainCollision, OFF_ZONE_TERRAIN_COLLISION));
}

// A full list retires its oldest entry: a mark still MARKED is cleared and
// counted expired. A cell already listed takes the new word in place.
static void AddLive(int cell, long word, void* zone)
{
	for (int i = 0; i < s_liveCount; ++i)
	{
		if (s_live[i].cell == cell)
		{
			s_live[i].word = word;
			s_live[i].zone = zone;
			return;
		}
	}
	if (s_liveCount >= LIVE_CAP)
	{
		const LiveMark& old = s_live[0];
		const LONG cur = InterlockedCompareExchange(&s_markWord[old.cell], 0, 0);
		if (NmMarkSeqOf(cur) == NmMarkSeqOf(old.word) && NmMarkStateOf(cur) == NM_MARK_MARKED
		    && InterlockedCompareExchange(&s_markWord[old.cell], NmMarkWord(NmMarkSeqOf(cur), NM_MARK_NONE), cur) == cur)
			InterlockedIncrement(&s_expired);
		for (int i = 1; i < s_liveCount; ++i)
			s_live[i - 1] = s_live[i];
		--s_liveCount;
	}
	s_live[s_liveCount].cell = cell;
	s_live[s_liveCount].word = word;
	s_live[s_liveCount].zone = zone;
	++s_liveCount;
}

// Marks one cell for this press, or keeps the mark a job is working on. The
// stamp is written before the word, so a claim that reads the new word reads
// its stamp. A lost exchange (a claim or a finish moved the word) decides once
// more on the word it left.
static long MarkCell(int cell, void* zone, LONGLONG now, bool* kept)
{
	*kept = false;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const LONG old = InterlockedCompareExchange(&s_markWord[cell], 0, 0);
		const double age = AgeSec(InterlockedCompareExchange64(&s_markQpc[cell], 0, 0), now);
		if (NmMarkPressDecide(old, age) == NM_PRESS_KEEP)
		{
			*kept = true;
			return old;
		}
		s_seq = NmMarkNextSeq(s_seq);
		const LONG word = NmMarkWord(s_seq, NM_MARK_MARKED);
		InterlockedExchange64(&s_markQpc[cell], now);
		if (InterlockedCompareExchange(&s_markWord[cell], word, old) == old)
		{
			AddLive(cell, word, zone);
			InterlockedIncrement(&s_marked);
			return word;
		}
	}
	*kept = true;
	return InterlockedCompareExchange(&s_markWord[cell], 0, 0);
}

// The key's own call: its return address in this frame, or, when another
// plugin's detour sits in front of this one, a few frames down.
static bool CalledByTheKey(void* immediate)
{
	unsigned __int64 offs[8];
	offs[0] = (unsigned __int64)((uintptr_t)immediate - gameBase);
	if (NmRebuildIsKeyCaller(offs, 1, RVA_PROCESS_KEYS_GENERATE_RET))
		return true;
	void* frames[8];
	const USHORT n = RtlCaptureStackBackTrace(0, 8, frames, NULL);
	for (USHORT i = 0; i < n; ++i)
		offs[i] = (unsigned __int64)((uintptr_t)frames[i] - gameBase);
	return NmRebuildIsKeyCaller(offs, (int)n, RVA_PROCESS_KEYS_GENERATE_RET);
}

static void AddPressCell(PressWork* p, int gx, int gy, void* zone)
{
	PressCell& c = p->c[p->count++];
	c.cell = (gx >= 0 && gx < NM_REBUILD_GRID && gy >= 0 && gy < NM_REBUILD_GRID) ? gx * NM_REBUILD_GRID + gy : -1;
	c.word = 0;
	c.zone = zone;
	c.verdict = PV_SKIP;
	const bool have = zone != NULL && c.cell >= 0;
	c.skip = NmRebuildEligible(have, have && IsZoneAccessible(zone), have && IsZoneLoading(zone),
	                           have && ZoneContent(zone) != 0, have && ZoneTerrain(zone) != 0);
	if (c.skip != NM_SKIP_NONE)
		return;
	bool kept = false;
	c.word = MarkCell(c.cell, zone, p->now, &kept);
	c.verdict = kept ? PV_KEPT : PV_SKIP;   // settled by QueueMissing
	p->pending++;
}

// The cells of one call: the zone the game passed, then, on the key's call,
// the neighbours the camera centre is near. Each eligible cell is marked.
static void BeginPress(void* navMesh, void* zone, void* immediate, PressWork* p)
{
	memset(p, 0, sizeof(*p));
	p->now = QpcNow();
	p->nmg = navMesh ? *(uintptr_t*)(KLIB_MEMBER(4, (uintptr_t)navMesh, NavMesh_generator, OFF_MGR_NAVMESH_GEN)) : 0;
	p->key = CalledByTheKey(immediate);
	p->centreNull = !zone || !ZoneContent(zone);
	const int gx = zone ? *(int*)(KLIB_MEMBER(4, (uintptr_t)zone, ZoneMap_coordinates_x, OFF_ZONE_COORDS_X)) : -1;
	const int gy = zone ? *(int*)(KLIB_MEMBER(4, (uintptr_t)zone, ZoneMap_coordinates_y, OFF_ZONE_COORDS_Y)) : -1;
	AddPressCell(p, gx, gy, zone);
	float x = 0.0f, z = 0.0f;
	if (!p->key || !g_cachedZoneMgr || !zone || !GetCameraFocusXZ(*(uintptr_t*)GameAddr(RVA_GLOBAL_PLAYER), &x, &z))
		return;
	const float sx = *(const float*)GameAddr(RVA_SUBMAP_CELL_SIZE_X);
	const float sz = *(const float*)GameAddr(RVA_SUBMAP_CELL_SIZE_Z);
	NmRebuildSelect(x, z, sx, sz, gx, gy, &p->sel);
	for (int i = 0; i < p->sel.count && p->count < NM_REBUILD_MAX_CELLS; ++i)
		AddPressCell(p, p->sel.n[i].gx, p->sel.n[i].gy, GetZoneEntry(g_cachedZoneMgr, p->sel.n[i].gx, p->sel.n[i].gy));
}

// One walk of the queue under its lock (no call, allocation or log inside),
// released before addJob, which takes the same lock itself. A cell whose job a
// claim already took is in flight; a queued type-0 job is left; anything else
// gets the game's own exterior job. hasJob is not used: it walks unlocked.
static void QueueMissing(PressWork* p)
{
	if (!p->nmg || !s_addJob || !s_hashZone || p->pending <= 0)
		return;
	bool queued[NM_REBUILD_MAX_CELLS] = { false };
	NmQueueLock q(p->nmg);
	uintptr_t node = *(uintptr_t*)(KLIB_MEMBER(4, p->nmg, NavMeshGenerator_queue_front, 136));
	for (int walked = 0; node && walked < QUEUE_WALK_CAP; ++walked)
	{
		const int type = *(int*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_flags, 88)) & 7;
		const uintptr_t nodeZone = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_zone, 0));
		if (type == 0)
			for (int i = 0; i < p->count; ++i)
				if (p->c[i].skip == NM_SKIP_NONE && (uintptr_t)p->c[i].zone == nodeZone)
					queued[i] = true;
		node = *(uintptr_t*)(KLIB_MEMBER(4, node, NavMeshGenerator__Task_next, 96));
	}
	q.Release();

	for (int i = 0; i < p->count; ++i)
	{
		PressCell& c = p->c[i];
		if (c.skip != NM_SKIP_NONE)
			continue;
		const LONG cur = InterlockedCompareExchange(&s_markWord[c.cell], 0, 0);
		if (NmMarkStateOf(cur) == NM_MARK_CLAIMED && NmMarkSeqOf(cur) == NmMarkSeqOf(c.word))
			c.verdict = c.verdict == PV_KEPT ? PV_KEPT : PV_INFLIGHT;
		else if (queued[i])
			c.verdict = (i == 0 && s_shown) ? PV_VANILLA : PV_ALREADY;
		else
		{
			s_addJob((void*)p->nmg, c.zone, s_hashZone(c.zone));
			c.verdict = PV_QUEUED;
		}
	}
}

// Before the game's call: the press's cells, the counters to report against,
// then the hold, so no dismissal slips between the game's show and the hold.
static void ArmHold(PressWork* p)
{
	s_pressCount = 0;
	for (int i = 0; i < p->count; ++i)
	{
		if (p->c[i].skip != NM_SKIP_NONE)
			continue;
		s_press[s_pressCount++] = p->c[i];
	}
	s_pressBase[0] = InterlockedCompareExchange(&s_stored, 0, 0);
	s_pressBase[1] = InterlockedCompareExchange(&s_refused, 0, 0);
	s_pressBase[2] = InterlockedCompareExchange(&s_dropped, 0, 0);
	s_pressBase[3] = InterlockedCompareExchange(&s_expired, 0, 0);
	s_pressBase[4] = InterlockedCompareExchange(&s_lost, 0, 0);
	InterlockedExchange64(&s_holdPressQpc, p->now);
	InterlockedExchange(&s_holdActive, 1);
}

static const char* VerdictName(const PressCell& c)
{
	switch (c.verdict)
	{
	case PV_VANILLA:  return "vanilla";
	case PV_QUEUED:   return "queued";
	case PV_ALREADY:  return "alreadyQueued";
	case PV_INFLIGHT: return "inFlight";
	case PV_KEPT:     return "kept";
	default:          return NULL;
	}
}

static void LogPress(const PressWork* p)
{
	std::ostringstream ss;
	const PressCell& centre = p->c[0];
	ss << "NavMesh rebuild: centre=(" << (centre.cell >= 0 ? centre.cell / NM_REBUILD_GRID : -1) << ","
	   << (centre.cell >= 0 ? centre.cell % NM_REBUILD_GRID : -1) << ") key=" << (p->key ? 1 : 0);
	if (p->key)
	{
		if (p->sel.matched)
			ss << std::fixed << std::setprecision(2) << " frac=(" << p->sel.fracX << "," << p->sel.fracZ << ")";
		else
			ss << " frac=none";
	}
	ss << " cells=" << p->count;
	int queued = 0, already = 0, inFlight = 0, skipped = 0;
	for (int i = 0; i < p->count; ++i)
	{
		const PressCell& c = p->c[i];
		const int gx = c.cell >= 0 ? c.cell / NM_REBUILD_GRID : -1;
		const int gy = c.cell >= 0 ? c.cell % NM_REBUILD_GRID : -1;
		ss << " (" << gx << "," << gy << ")=";
		const char* name = c.skip == NM_SKIP_NONE ? VerdictName(c) : NULL;
		if (name)
			ss << name;
		else
			ss << "skip:" << NmRebuildSkipName(c.skip);
		if (c.skip != NM_SKIP_NONE)                          ++skipped;
		else if (c.verdict == PV_QUEUED)                     ++queued;
		else if (c.verdict == PV_ALREADY || c.verdict == PV_VANILLA) ++already;
		else if (c.verdict == PV_INFLIGHT || c.verdict == PV_KEPT)   ++inFlight;
	}
	ss << " queued=" << queued << " alreadyQueued=" << already << " inFlight=" << inFlight
	   << " skipped=" << skipped << " hold=" << ((p->key && p->pending > 0) ? "on" : "off");
	LogMsg(ss.str());
}

static void hook_navMeshGenerate(void* navMesh, void* zone)
{
	void* immediate = _ReturnAddress();
	if (!IsMainThread() || !navmesh::g_navmeshCfg.cachingEnabled)
	{
		s_origGenerate(navMesh, zone);
		return;
	}
	PressWork p;
	BeginPress(navMesh, zone, immediate, &p);
	if (p.key && p.pending > 0)
		ArmHold(&p);
	s_inCall = true;
	s_tagKey = p.key;
	s_shown = false;
	if (!p.centreNull)
		s_origGenerate(navMesh, zone);   // vanilla's message, exterior and interior jobs
	QueueMissing(&p);
	if (p.pending > 0)
		CallPrioritizeNavMeshQueue();
	if (p.key && p.pending > 0 && !s_shown)
		hook_showLoadingMessage(GameAddr(RVA_GLOBAL_GUI), true);
	s_inCall = false;
	s_tagKey = false;
	LogPress(&p);
}

bool NmForceRebuildInKeyCall()
{
	return s_inCall && s_tagKey && IsMainThread();
}

void NmForceRebuildNoteShow()
{
	if (!s_inCall || !IsMainThread())
		return;
	s_shown = true;
	if (s_tagKey && InterlockedCompareExchange(&s_holdActive, 0, 0))
		InterlockedExchange(&s_pressShowOpen, 1);
}

void NmForceRebuildNoteDismissed()
{
	InterlockedExchange(&s_pressShowOpen, 0);
}

// ---------------------------------------------------------------------
// The queue's front and the hold
// ---------------------------------------------------------------------

bool NmForceRebuildWantsFront(uintptr_t zone, int jobType, __int64 nowQpc)
{
	if (!zone || jobType != 0)
		return false;
	const int cell = CellIndex(zone);
	if (cell < 0)
		return false;
	const LONG word = InterlockedCompareExchange(&s_markWord[cell], 0, 0);
	if (NmMarkStateOf(word) != NM_MARK_MARKED)
		return false;
	return NmMarkWantsFront(word, AgeSec(InterlockedCompareExchange64(&s_markQpc[cell], 0, 0), nowQpc), jobType);
}

bool NmForceRebuildHoldsDismissal()
{
	if (!InterlockedCompareExchange(&s_holdActive, 0, 0))
		return false;
	const double since = AgeSec(InterlockedCompareExchange64(&s_holdPressQpc, 0, 0), QpcNow());
	if (!NmHoldSuppresses(true, IsMainThread(), since))
		return false;
	InterlockedExchange(&s_dismissOwed, 1);
	// Released in between: let this one through; the release replays one too, and a second hide is harmless.
	if (!InterlockedCompareExchange(&s_holdActive, 0, 0))
		return false;
	InterlockedIncrement(&s_heldDismissals);
	return true;
}

// Expired marks are cleared, and so are marks whose cell lost its content (a
// job the game popped itself never reaches a claim). Entries of another press,
// and finished ones, leave the list.
static void SweepLiveMarks(LONGLONG now)
{
	int kept = 0;
	for (int i = 0; i < s_liveCount; ++i)
	{
		const LiveMark m = s_live[i];
		const LONG cur = InterlockedCompareExchange(&s_markWord[m.cell], 0, 0);
		if (NmMarkSeqOf(cur) != NmMarkSeqOf(m.word))
			continue;
		const int state = NmMarkStateOf(cur);
		if (state == NM_MARK_MARKED)
		{
			const LONG none = NmMarkWord(NmMarkSeqOf(cur), NM_MARK_NONE);
			if (AgeSec(InterlockedCompareExchange64(&s_markQpc[m.cell], 0, 0), now) > NM_REBUILD_MARK_TTL_SEC)
			{
				if (InterlockedCompareExchange(&s_markWord[m.cell], none, cur) == cur)
					InterlockedIncrement(&s_expired);
				continue;
			}
			if (!ZoneContent(m.zone))
			{
				if (InterlockedCompareExchange(&s_markWord[m.cell], none, cur) == cur)
					InterlockedIncrement(&s_lost);
				continue;
			}
		}
		else if (state != NM_MARK_CLAIMED)
			continue;
		s_live[kept++] = m;
	}
	s_liveCount = kept;
}

static int CountUnfinished()
{
	int n = 0;
	for (int i = 0; i < s_pressCount; ++i)
		if (!NmMarkPressFinished(s_press[i].word, InterlockedCompareExchange(&s_markWord[s_press[i].cell], 0, 0)))
			++n;
	return n;
}

static void LogHoldEnd(NmHoldVerdict v, double since)
{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	if (v == NM_HOLD_DONE)
	{
		ss << "NavMesh rebuild done: " << since << " s cells=" << s_pressCount
		   << " stored=" << (InterlockedCompareExchange(&s_stored, 0, 0) - s_pressBase[0])
		   << " refused=" << (InterlockedCompareExchange(&s_refused, 0, 0) - s_pressBase[1])
		   << " dropped=" << (InterlockedCompareExchange(&s_dropped, 0, 0) - s_pressBase[2])
		   << " expired=" << (InterlockedCompareExchange(&s_expired, 0, 0) - s_pressBase[3])
		   << " lost=" << (InterlockedCompareExchange(&s_lost, 0, 0) - s_pressBase[4])
		   << " owed=" << (InterlockedCompareExchange(&s_dismissOwed, 0, 0) ? 1 : 0)
		   << " held=" << InterlockedCompareExchange(&s_heldDismissals, 0, 0)
		   << " replays=" << InterlockedCompareExchange(&s_replays, 0, 0)
		   << " marked=" << InterlockedCompareExchange(&s_marked, 0, 0);
	}
	else
	{
		ss << "NavMesh rebuild hold capped: " << since << " s, unfinished";
		for (int i = 0; i < s_pressCount; ++i)
		{
			const LONG cur = InterlockedCompareExchange(&s_markWord[s_press[i].cell], 0, 0);
			if (NmMarkPressFinished(s_press[i].word, cur))
				continue;
			ss << " (" << s_press[i].cell / NM_REBUILD_GRID << "," << s_press[i].cell % NM_REBUILD_GRID << ")="
			   << (NmMarkStateOf(cur) == NM_MARK_CLAIMED ? "claimed" : "marked");
		}
		ss << "; the panel is released";
	}
	s_pressCount = 0;
	LogMsg(ss.str());
}

bool NmForceRebuildTick(void* zoneMgr, bool saveLoading)
{
	const LONGLONG now = QpcNow();
	if (s_liveCount > 0)
		SweepLiveMarks(now);
	if (InterlockedCompareExchange(&s_holdActive, 0, 0))
	{
		const double since = AgeSec(InterlockedCompareExchange64(&s_holdPressQpc, 0, 0), now);
		const NmHoldVerdict v = NmHoldDecide(true, CountUnfinished(), since);
		if (v == NM_HOLD_PENDING)
			return false;
		InterlockedExchange(&s_holdActive, 0);
		// A press never leaves the panel up: a dismissal the hold swallowed, or
		// the press's own show with no dismissal since, is issued below.
		const bool shownOpen = InterlockedExchange(&s_pressShowOpen, 0) != 0;
		if (NmHoldReleaseOwes(v, InterlockedCompareExchange(&s_dismissOwed, 0, 0) != 0, shownOpen))
			InterlockedExchange(&s_dismissOwed, 1);
		LogHoldEnd(v, since);   // "NavMesh rebuild done:" or "NavMesh rebuild hold capped:"
	}
	const bool owed = InterlockedCompareExchange(&s_dismissOwed, 0, 0) != 0;
	if (!NmHoldReplayNow(owed, zoneMgr != NULL, zoneMgr ? GetZoneState(zoneMgr) : -1, saveLoading))
		return false;
	InterlockedExchange(&s_dismissOwed, 0);
	InterlockedIncrement(&s_replays);
	return true;
}

void NmForceRebuildOnWorldReset()
{
	for (int i = 0; i < MARK_CELLS; ++i)
		InterlockedExchange(&s_markWord[i], 0);
	s_liveCount = 0;
	s_pressCount = 0;
	InterlockedExchange(&s_holdActive, 0);
	InterlockedExchange(&s_dismissOwed, 0);
	InterlockedExchange(&s_pressShowOpen, 0);
}

// ---------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------

static const unsigned char kAddJobPrologue[16]   = { 0x48,0x8B,0xC4,0x55,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x8D,0x68,0xA1,0x48 };
static const unsigned char kHashZonePrologue[16] = { 0x40,0x55,0x48,0x83,0xEC,0x20,0x48,0x8B,0x01,0x48,0x89,0x7C,0x24,0x40,0x33,0xFF };

void InstallNavMeshRebuildKey(int* installed, int*)
{
	if (!HookRowWanted(HOOK_NAVMESH_GENERATE_ZONEMAP))
	{
		LogMsg("NavMesh rebuild key: rebuildKey=off(caching)");
		return;
	}
	const char* why = NULL;
	if (!VerifyPrologue(RVA_NMG_ADD_JOB_ZONE, kAddJobPrologue, "addJob(ZoneMap*,uint)"))
		why = "addJob";
	else if (!VerifyPrologue(RVA_NAVMESH_HASH_ZONE, kHashZonePrologue, "hashZone"))
		why = "hashZone";
	else
	{
		s_addJob = (addJobZone_t)GameAddr(RVA_NMG_ADD_JOB_ZONE);
		s_hashZone = (hashZone_t)GameAddr(RVA_NAVMESH_HASH_ZONE);
		why = HookInstall(HOOK_NAVMESH_GENERATE_ZONEMAP, hook_navMeshGenerate, &s_origGenerate, installed, true);
	}
	if (why)
		LogMsg(std::string("NavMesh rebuild key: rebuildKey=refused(") + why + "); the key keeps the game's own behaviour");
	else
		LogMsg("NavMesh rebuild key: rebuildKey=on");
}
