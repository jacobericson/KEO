// planner_tick.cpp - The route planner's main-thread half: the order capture, the store adapter the
// coarse search walks, the locator, the loaded-set snapshot and the tick that validates, drops and
// re-plans each slot and feeds the route's tiles to the preload queue.
// Main thread only. The one game lock is the section manager's world lock (+0x200), taken
// try-shared (never blocking) by the locator and the snapshot and released on every path; no mod
// lock is taken. The search scratch is allocated once, at arm.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "planner/planner_tick.h"
#include "planner/plan_build.h"
#include "planner/plan_policy.h"
#include "planner/plan_store.h"
#include "planner/coarse_graph.h"
#include "planner/coarse_graph_live.h"
#include "planner/coarse_search.h"
#include "planner/planner_config.h"
#include "game/game.h"
#include "base/core.h"
#include "zone/readiness/readiness_bindings.h"
#include "zone/preload/preload.h"
#include "movement/islands.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <new>

namespace planner {

static_assert(CG_NODE_ARCS_MAX == COARSE_ARCS_MAX, "a node reports at most as many arcs as the search takes");
static_assert(PDW_COUNT == PLAN_DROP_REASONS, "one drop count per drop reason");
static_assert(PRW_COUNT == PLAN_REPLAN_REASONS, "one re-plan count per re-plan reason");

namespace planner_tick_detail {

const double TICK_SECONDS       = 0.25;
const int    REPLANS_PER_TICK   = 4;
const int    SNAP_SLOTS         = 1024;   // a packed face key's 10 collection-slot bits
const int    MEMO_ENTRIES       = 8;
const int    NODES_PER_SECTION  = 1 << CG_NODE_BITS;
const int    PLAYER_SCAN_MAX    = 200;
const int    LINE_TILES_MAX     = 16;
const float  FACE_RAY_LENGTH    = 500.0f; // Havok units, both ways along the vertical
const float  WORLD_TO_HAVOK     = 0.1f;

typedef unsigned int (*getFaceKeyVec4_t)(void* sectionMgr, const float* havokPos, float rayLength);

// A located point: its store node and section, and whether the engine's own face lookup found it.
struct Located { unsigned key; int dir; int uid; int node; int exact; };

// One search's legs and figures, kept for the other characters of the same order.
struct Built
{
	unsigned __int64 memoKey;
	int      found;
	int      legCount, truncated;
	float    cost;
	int      expanded;
	double   ms;
	PlanLeg  legs[PLAN_MAX_LEGS];
};

// A live player character and its movement, for the tick's validation.
struct PlayerChar { uintptr_t character, cm; };

} // namespace planner_tick_detail
using namespace planner_tick_detail;

// Written by the arm, then read by the main thread alone.
static CoarseScratch* s_scratch     = NULL;
static int            s_legSpan     = 2;
static int            s_aheadTiles  = 3;
static int            s_waitSeconds = 10;

// The loaded-set snapshot: the exterior cells as a 4,096-bit set, the interior uids and their
// directory indices; s_loadedGen moves when the set changes. Main thread.
static unsigned       s_loadedExt[CG_EXTERIOR_SLOTS / 32];
static int            s_interiorUids[SNAP_SLOTS];
static int            s_interiorDirs[SNAP_SLOTS];
static int            s_interiorCount = 0;
static unsigned       s_loadedGen     = 0;
static unsigned       s_walkExt[CG_EXTERIOR_SLOTS / 32];
static int            s_walkUids[SNAP_SLOTS];

// The tick's own state. Main thread.
static double         s_lastTick      = 0.0;
static unsigned       s_storeGen      = 0;
static int            s_orderSeq      = 0;

// Per-call scratch of the order capture and the re-plan. Main thread.
static Built          s_memo[MEMO_ENTRIES];
static int            s_memoCount     = 0;
static CoarseRoute    s_route;
static PlanRouteStep  s_steps[COARSE_ROUTE_MAX];
static PlanNodeBox    s_boxes[NODES_PER_SECTION];
static PlanWrite      s_write;

static float DistXz(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dz = a[2] - b[2];
	return sqrtf(dx * dx + dz * dz);
}

static void CharPos(uintptr_t character, float out[3])
{
	out[0] = GetCharPosX(character);
	out[1] = *(float*)(KLIB_MEMBER(1, character, RootObjectBase_pos_y, OFF_CHAR_POS_Y));
	out[2] = GetCharPosZ(character);
}

static uintptr_t MovementOf(uintptr_t character)
{
	return character ? *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT)) : 0;
}

// ---- The store adapter ---------------------------------------------------------------------------

// A neighbour section's current block, by uid; NULL when it has no index or no readable block.
static const CgBlock* NeighbourOf(void* ctx, int uid, int* dirIndexOut)
{
	(void)ctx;
	int dir = CgIndexOfUid(uid);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return NULL;
	*dirIndexOut = dir;
	return v.block;
}

static const CgBlock* BlockOfKey(unsigned key, int* idx)
{
	CgView v;
	if (!CgRead(CgNodeDir(key), &v))
		return NULL;
	*idx = CgNodeIndex(key);
	return (*idx >= 0 && *idx < v.block->nodeCount) ? v.block : NULL;
}

// The node's intra arcs, then its resolved cross arcs, the first max of them.
static int AdapterArcs(void* ctx, unsigned key, CoarseArc* out, int max)
{
	(void)ctx;
	int idx;
	const CgBlock* b = BlockOfKey(key, &idx);
	if (!b)
		return -1;
	int dir = CgNodeDir(key);
	const CgNode& n = b->nodes[idx];
	int count = 0;
	for (int i = 0; i < n.arcCount && count < max; ++i)
	{
		const CgArc& a = b->arcs[n.firstArc + i];
		out[count].to = CgNodeKey(dir, a.to);
		out[count].cost = a.cost;
		++count;
	}
	if (count < max)
	{
		CgResolved res[CG_NODE_ARCS_MAX];
		int r = CgCrossArcs(b, idx, NeighbourOf, NULL, res, max - count);
		for (int i = 0; i < r; ++i)
		{
			out[count].to = CgNodeKey(res[i].dirIndex, res[i].node);
			out[count].cost = res[i].cost;
			++count;
		}
	}
	return count;
}

static bool AdapterPosition(void* ctx, unsigned key, float out[3])
{
	(void)ctx;
	int idx;
	const CgBlock* b = BlockOfKey(key, &idx);
	if (!b)
		return false;
	memcpy(out, b->nodes[idx].centre, sizeof(float) * 3);
	return true;
}

// ---- The loaded-set snapshot ---------------------------------------------------------------------

static uintptr_t SectionManager()
{
	return *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
}

static const void* LiveCollection(uintptr_t sm)
{
	uintptr_t world = *(uintptr_t*)(KLIB_MEMBER(4, sm, NavMesh_world, OFF_RDY_SM_WORLD));
	return world ? *(const void**)(world + OFF_RDY_WORLD_COLLECTION) : NULL;
}

// The world's instances walked under one try-shared take of the world lock (the uid decode reads
// mod memory only); a busy lock keeps the last snapshot. Interior uids get their directory indices
// after the release.
static void TakeSnapshot()
{
	uintptr_t sm = SectionManager();
	if (!sm || !fn_boostUnlockShared)
		return;
	volatile LONG* lock = (volatile LONG*)(KLIB_MEMBER(4, sm, NavMesh_changeMutex, OFF_RDY_SM_WORLD_LOCK));
	if (!BoostTryLockShared(lock))
		return;
	memset(s_walkExt, 0, sizeof(s_walkExt));
	int interiors = 0;
	long pastSlots = 0;
	const void* coll = LiveCollection(sm);
	int count = CgCollectionCount(coll);
	for (int i = 0; i < count; ++i)
	{
		const void* mi = CgCollectionMeshInstance(coll, i);
		if (!mi || *(const int*)((const unsigned char*)mi + OFF_RDY_NMI_RUNTIME_INDEX) < 0)
			continue;
		if (i >= SNAP_SLOTS)
		{
			++pastSlots;
			continue;
		}
		int uid = CgMeshInstanceUid(mi);
		int dir = CgIndexOfUid(uid);
		if (dir >= 0 && dir < CG_EXTERIOR_SLOTS)
			s_walkExt[dir >> 5] |= 1u << (dir & 31);
		else
			s_walkUids[interiors++] = uid;
	}
	fn_boostUnlockShared((void*)lock);
	if (pastSlots)
		InterlockedExchangeAdd(&PlannerCountersGet()->locFail, pastSlots);
	bool changed = interiors != s_interiorCount
	            || memcmp(s_walkExt, s_loadedExt, sizeof(s_loadedExt)) != 0
	            || memcmp(s_walkUids, s_interiorUids, sizeof(int) * interiors) != 0;
	// An interior's directory index can appear after its instance does (the store maps a fresh uid
	// when its first copy lands), so the indices are re-read on every snapshot.
	for (int i = 0; i < interiors; ++i)
	{
		int dir = CgIndexOfUid(s_walkUids[i]);
		if (changed || dir != s_interiorDirs[i])
		{
			changed = true;
			s_interiorDirs[i] = dir;
		}
	}
	if (!changed)
		return;
	memcpy(s_loadedExt, s_walkExt, sizeof(s_loadedExt));
	memcpy(s_interiorUids, s_walkUids, sizeof(int) * interiors);
	s_interiorCount = interiors;
	++s_loadedGen;
}

static bool IsDirLoaded(int dir)
{
	if (dir < 0)
		return false;
	if (dir < CG_EXTERIOR_SLOTS)
		return (s_loadedExt[dir >> 5] >> (dir & 31) & 1u) != 0;
	for (int i = 0; i < s_interiorCount; ++i)
		if (s_interiorDirs[i] == dir)
			return true;
	return false;
}

static unsigned LoadedMaskOf(const PlanLeg* legs, int n)
{
	unsigned mask = 0;
	for (int i = 0; i < n && i < PLAN_MAX_LEGS; ++i)
		if (IsDirLoaded(legs[i].farSection))
			mask |= 1u << i;
	return mask;
}

// ---- The locator ---------------------------------------------------------------------------------

static bool StoreNode(int uid, int dir, int node, int exact, Located* out)
{
	CgView v;
	if (dir < 0 || node < 0 || !CgRead(dir, &v) || node >= v.block->nodeCount)
		return false;
	out->key = CgNodeKey(dir, node);
	out->dir = dir;
	out->uid = uid;
	out->node = node;
	out->exact = exact;
	return true;
}

// The engine's own face lookup under a try-shared take of the world lock: the packed key's slot
// names the mesh instance, whose face gives the cluster and whose uid gives the section.
static bool LocateExact(const float p[3], Located* out)
{
	uintptr_t sm = SectionManager();
	if (!sm || !fn_boostUnlockShared)
		return false;
	const float* shift = *(const float* const*)(KLIB_MEMBER(4, sm, NavMesh_worldShift, 0x1D8));
	if (!shift)
		return false;
	volatile LONG* lock = (volatile LONG*)(KLIB_MEMBER(4, sm, NavMesh_changeMutex, OFF_RDY_SM_WORLD_LOCK));
	if (!BoostTryLockShared(lock))
		return false;
	int uid = 0, cluster = -1;
	const void* coll = LiveCollection(sm);
	if (coll)
	{
		__declspec(align(16)) float h[4];
		h[0] = p[0] * WORLD_TO_HAVOK + shift[0];
		h[1] = p[1] * WORLD_TO_HAVOK + shift[1];
		h[2] = p[2] * WORLD_TO_HAVOK + shift[2];
		h[3] = 0.0f;
		getFaceKeyVec4_t getFaceKey = (getFaceKeyVec4_t)GameAddr(RVA_NAVMESH_GET_FACE_KEY_VEC4);
		unsigned key = getFaceKey((void*)sm, h, FACE_RAY_LENGTH);
		if (key != 0xFFFFFFFFu)
		{
			const void* mi = CgCollectionMeshInstance(coll, (int)(key >> 22));
			if (mi)
			{
				cluster = CgInstanceFaceCluster(mi, key & 0x3FFFFFu);
				uid = CgMeshInstanceUid(mi);
			}
		}
	}
	fn_boostUnlockShared((void*)lock);
	return cluster >= 0 && StoreNode(uid, CgIndexOfUid(uid), cluster, 1, out);
}

// The footprint pick over one block's nodes; -1 when none qualifies.
static int PickInBlock(const CgBlock* b, const float p[3], bool allowFallback)
{
	int n = b->nodeCount < NODES_PER_SECTION ? b->nodeCount : NODES_PER_SECTION;
	for (int i = 0; i < n; ++i)
	{
		memcpy(s_boxes[i].boxMin, b->nodes[i].boxMin, sizeof(float) * 3);
		memcpy(s_boxes[i].boxMax, b->nodes[i].boxMax, sizeof(float) * 3);
		memcpy(s_boxes[i].centre, b->nodes[i].centre, sizeof(float) * 3);
	}
	return PlanPickFootprint(s_boxes, n, p, allowFallback);
}

// The interiors whose widened boxes hold the point first (the nearest centre among them), then the
// point's cell's exterior with the fallback.
static bool LocateFootprint(const float p[3], Located* out)
{
	int bestDir = -1, bestNode = -1, bestUid = 0;
	float bestD = 0.0f;
	for (int dir = CG_EXTERIOR_SLOTS; dir < CG_DIR_SLOTS; ++dir)
	{
		CgView v;
		if (!CgRead(dir, &v) || !v.block->uid)
			continue;
		int node = PickInBlock(v.block, p, false);
		if (node < 0)
			continue;
		const float* c = v.block->nodes[node].centre;
		float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
		float d = dx * dx + dy * dy + dz * dz;
		if (bestDir < 0 || d < bestD)
		{
			bestDir = dir;
			bestNode = node;
			bestUid = v.block->uid;
			bestD = d;
		}
	}
	if (bestDir >= 0)
		return StoreNode(bestUid, bestDir, bestNode, 0, out);
	int cx, cy;
	PlanCellOf(p[0], p[2], &cx, &cy);
	int dir = CgExteriorIndex(cx, cy);
	CgView v;
	if (dir < 0 || !CgRead(dir, &v))
		return false;
	int node = PickInBlock(v.block, p, true);
	return node >= 0 && StoreNode(v.block->uid, dir, node, 0, out);
}

// A busy lock or a missed face adds to locFail and falls back to the footprints.
static bool Locate(const float p[3], Located* out)
{
	if (LocateExact(p, out))
		return true;
	InterlockedIncrement(&PlannerCountersGet()->locFail);
	return LocateFootprint(p, out);
}

// ---- Search and legs -----------------------------------------------------------------------------

// The cross arc from step i's node to step i + 1's: its portal and edge; the next node's centre
// when the arc is no longer resolvable.
static void ResolveCrossing(unsigned from, unsigned to, PlanRouteStep* step)
{
	int idx;
	const CgBlock* b = BlockOfKey(from, &idx);
	if (b)
	{
		CgResolved res[CG_NODE_ARCS_MAX];
		int r = CgCrossArcs(b, idx, NeighbourOf, NULL, res, CG_NODE_ARCS_MAX);
		for (int i = 0; i < r; ++i)
		{
			if (CgNodeKey(res[i].dirIndex, res[i].node) != to)
				continue;
			memcpy(step->portal, res[i].portal, sizeof(step->portal));
			memcpy(step->edgeA, res[i].edgeA, sizeof(step->edgeA));
			memcpy(step->edgeB, res[i].edgeB, sizeof(step->edgeB));
			return;
		}
	}
	AdapterPosition(NULL, to, step->portal);
	memcpy(step->edgeA, step->portal, sizeof(step->edgeA));
	memcpy(step->edgeB, step->portal, sizeof(step->edgeB));
}

static void BuildFromRoute(const CoarseRoute& route, const float dest[3], int destSection, Built* out)
{
	int n = route.count;
	for (int i = 0; i < n; ++i)
	{
		PlanRouteStep& s = s_steps[i];
		memset(&s, 0, sizeof(s));
		s.dirIndex = CgNodeDir(route.nodes[i]);
		s.node = CgNodeIndex(route.nodes[i]);
		s.crossesNext = i + 1 < n && CgNodeDir(route.nodes[i + 1]) != s.dirIndex;
		if (s.crossesNext)
			ResolveCrossing(route.nodes[i], route.nodes[i + 1], &s);
	}
	out->legCount = PlanBuildLegs(s_steps, n, dest, destSection, out->legs, &out->truncated);
}

// One coarse search from start to goal, memoised within an order by the node pair.
static const Built* SearchAndBuild(const Located& start, const Located& goal, const float dest[3])
{
	unsigned __int64 memoKey = PlanMemoKey(start.key, goal.key);
	for (int i = 0; i < s_memoCount; ++i)
		if (s_memo[i].memoKey == memoKey)
			return &s_memo[i];
	Built* b = &s_memo[s_memoCount < MEMO_ENTRIES ? s_memoCount++ : MEMO_ENTRIES - 1];
	memset(b, 0, sizeof(*b));
	b->memoKey = memoKey;
	CoarseGraphOps ops;
	ops.ctx = NULL;
	ops.arcs = AdapterArcs;
	ops.position = AdapterPosition;
	LONGLONG t0 = QpcNow();
	CoarseResult r = CoarseSearch(ops, start.key, goal.key, COARSE_SCRATCH_MAX, s_scratch, &s_route);
	b->ms = QpcToMs(QpcNow() - t0);
	b->found = r == CS_FOUND;
	b->expanded = s_route.expanded;
	b->cost = b->found ? s_route.cost : 0.0f;
	if (b->found)
		BuildFromRoute(s_route, dest, goal.dir, b);
	return b;
}

// Writes the character's plan and feeds its route's next tiles; -1 when the store is full.
static int WritePlan(uintptr_t cm, const float pos[3], const Located& goal, const float dest[3],
                     const Built& b, double now, int* verdictOut, int keepSends)
{
	PlanWrite& w = s_write;
	memset(&w, 0, sizeof(w));
	w.cm = cm;
	w.keepSends = keepSends;
	w.legCount = b.found ? b.legCount : 0;
	memcpy(w.legs, b.legs, sizeof(PlanLeg) * w.legCount);
	w.routeTruncated = b.truncated;
	w.goalByFootprint = !goal.exact;
	w.loadedMask = LoadedMaskOf(w.legs, w.legCount);
	int sx, sy, gx, gy;
	PlanCellOf(pos[0], pos[2], &sx, &sy);
	PlanCellOf(dest[0], dest[2], &gx, &gy);
	int span = PlanCellSpan(sx, sy, gx, gy);
	w.verdict = PlanDecideVerdict(b.found != 0, w.legCount, w.loadedMask, span, s_legSpan);
	int first = PlanLegTarget(w.legs, w.legCount, w.loadedMask, 0, sx, sy, s_legSpan);
	w.firstLeg = first < 0 ? 0 : first;
	memcpy(w.finalDest, dest, sizeof(w.finalDest));
	w.now = now;
	*verdictOut = w.verdict;
	int slot = PlanStoreWrite(w);
	if (slot < 0)
		return -1;
	PlanStoreMain(slot)->loadedGenAtPlan = s_loadedGen;
	PlannerCounters* c = PlannerCountersGet();
	InterlockedIncrement(&c->plans);
	InterlockedIncrement(w.verdict == PV_DIRECT ? &c->direct : (w.verdict == PV_LEGGED ? &c->legged : &c->noRoute));
	InterlockedExchangeAdd(&c->legs, w.legCount);
	int xy[2 * 8];
	int cells = PlanFeedCells(w.legs, w.legCount, w.firstLeg, s_aheadTiles, CG_EXTERIOR_SLOTS, xy);
	for (int i = 0; i < cells; ++i)
		EnqueueCharacterZone(xy[2 * i], xy[2 * i + 1]);
	return slot;
}

// Drops the character's plan, counting it by reason, and a legged plan the edge branch never consulted.
static void DropPlan(uintptr_t cm, PlanDropWhy why)
{
	PlanView v;
	int slot = PlanStoreFind(cm);
	int verdict = (slot >= 0 && PlanStoreRead(slot, &v)) ? v.verdict : PV_NONE;
	int consulted = PlanStoreDrop(cm);
	if (consulted < 0)
		return;
	InterlockedIncrement(&PlannerCountersGet()->drops);
	if (why > PDW_NONE && why < PDW_COUNT)
		InterlockedIncrement(&PlannerCountersGet()->dropsBy[why]);
	if (verdict == PV_LEGGED && consulted == 0)
		InterlockedIncrement(&PlannerCountersGet()->notConsulted);
}

// ---- The order capture ---------------------------------------------------------------------------

static const char* VerdictName(int v)
{
	return v == PV_DIRECT ? "direct" : (v == PV_LEGGED ? "legged" : "noRoute");
}

// The distinct exterior cells of the legs, in route order, as gx.gy pairs.
static void FormatTiles(const PlanLeg* legs, int n, char* out, size_t size)
{
	out[0] = '\0';
	size_t len = 0;
	int shown = 0, lastX = -1, lastY = -1;
	for (int i = 0; i < n && shown < LINE_TILES_MAX; ++i)
	{
		if (legs[i].farSection >= CG_EXTERIOR_SLOTS || (legs[i].cellX == lastX && legs[i].cellY == lastY))
			continue;
		lastX = legs[i].cellX;
		lastY = legs[i].cellY;
		int w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%d.%d", shown ? "," : "", lastX, lastY);
		if (w < 0)
			break;
		len += (size_t)w;
		++shown;
	}
}

static void ReportOrderPlan(int order, int k, const Located& from, const Located& to, int verdict,
                            const Built& b, int span, bool indoors, uintptr_t cm)
{
	char tiles[160];
	FormatTiles(b.legs, b.found ? b.legCount : 0, tiles, sizeof(tiles));
	float road = *(const float*)(KLIB_MEMBER(3, cm, CharMovement_roadWeight, 0x100));
	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Planner plan: order=%d char=%d from=%x:%d to=%x:%d verdict=%s legs=%d tiles=%s span=%d"
	            " cost=%.0f expanded=%d ms=%.1f loc=%s indoors=%d road=%.3f",
	            order, k, (unsigned)from.uid, from.node, (unsigned)to.uid, to.node, VerdictName(verdict),
	            b.found ? b.legCount : 0, tiles, span, b.cost, b.expanded, b.ms,
	            to.exact ? "exact" : "footprint", indoors ? 1 : 0, road);
	PlannerReportPlan(line);
}

static void DropOrderPlans(const uintptr_t* chars, int n, volatile LONG* counter)
{
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm)
			continue;
		if (counter)
			InterlockedIncrement(counter);
		DropPlan(cm, PDW_UNLOCATED);
	}
}

// Whether the character's current plan already answers this order: inside its first second and
// toward the same point. Main thread, the store's one writer; reads the slot without a lock.
static bool IsRepeatOrder(uintptr_t cm, const float dest[3], double now)
{
	PlanView v;
	int slot = PlanStoreFind(cm);
	if (slot < 0 || !PlanStoreRead(slot, &v) || v.cm != cm)
		return false;
	return PlanRepeatDue(v.finalDest, dest, now - PlanStoreMain(slot)->planTime);
}

// The engine's move branch applies the destination at once whatever the order's two flags carry
// (a plain click sends the add flag set; the flags matter only to the engine's other orders), so
// every captured move order is planned.
void PlannerNoteOrder(const uintptr_t* chars, int n, const float* location, void* destIndoors, bool shift, bool addDontClear)
{
	if (PlanStoreMode() == PLANNER_OFF) return;
	(void)shift;
	(void)addDontClear;
	if (!location)
	{
		InterlockedIncrement(&PlannerCountersGet()->noLocation);
		return;
	}
	if (n <= 0)
		return;
	int order = ++s_orderSeq;
	TakeSnapshot();
	float dest[3] = { location[0], location[1], location[2] };
	Located goal;
	if (!Locate(dest, &goal))
	{
		DropOrderPlans(chars, n, &PlannerCountersGet()->goalUnlocated);
		return;
	}
	s_memoCount = 0;
	double now = ElapsedSec();
	for (int k = 0; k < n; ++k)
	{
		uintptr_t cm = MovementOf(chars[k]);
		if (!cm)
			continue;
		if (IsRepeatOrder(cm, dest, now))
		{
			InterlockedIncrement(&PlannerCountersGet()->repeats);
			continue;
		}
		float pos[3];
		CharPos(chars[k], pos);
		Located start;
		if (!Locate(pos, &start))
		{
			InterlockedIncrement(&PlannerCountersGet()->startUnlocated);
			DropPlan(cm, PDW_UNLOCATED);
			continue;
		}
		const Built* b = SearchAndBuild(start, goal, dest);
		int verdict = PV_NONE;
		if (WritePlan(cm, pos, goal, dest, *b, now, &verdict, 0) < 0)
			continue;
		int sx, sy, gx, gy;
		PlanCellOf(pos[0], pos[2], &sx, &sy);
		PlanCellOf(dest[0], dest[2], &gx, &gy);
		ReportOrderPlan(order, k, start, goal, verdict, *b, PlanCellSpan(sx, sy, gx, gy), destIndoors != NULL, cm);
	}
}

void PlannerDrop(uintptr_t character)
{
	if (PlanStoreMode() == PLANNER_OFF) return;
	DropPlan(MovementOf(character), PDW_ORDER);
}

bool PlannerRouteReplacesAhead(uintptr_t character)
{
	if (PlanStoreMode() == PLANNER_OFF) return false;
	uintptr_t cm = MovementOf(character);
	int slot = PlanStoreFind(cm);
	PlanView v;
	if (slot < 0 || !PlanStoreRead(slot, &v) || v.cm != cm)
		return false;
	return PlanReplacesAhead(PlanStoreMode(), v.verdict);
}

bool PlannerTickArm()
{
	s_scratch = new (std::nothrow) CoarseScratch;
	if (!s_scratch)
		return false;
	if (!CoarseScratchInit(s_scratch, COARSE_SCRATCH_MAX))
	{
		delete s_scratch;
		s_scratch = NULL;
		return false;
	}
	s_legSpan = g_plannerCfg.legSpan;
	s_aheadTiles = g_plannerCfg.aheadTiles < 0 ? 0 : (g_plannerCfg.aheadTiles > 8 ? 8 : g_plannerCfg.aheadTiles);   // the feed buffers hold 8 cells
	s_waitSeconds = g_plannerCfg.waitSeconds;
	return true;
}

// ---- The tick ------------------------------------------------------------------------------------

// The live player characters with their movements; at most PLAYER_SCAN_MAX.
static int ReadPlayers(PlayerChar* out)
{
	uintptr_t pi = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!pi)
		return 0;
	unsigned count = GetPlayerCharCount(pi);
	uintptr_t* stuff = GetPlayerCharStuff(pi);
	if (!stuff || count > (unsigned)PLAYER_SCAN_MAX)
		return 0;
	int n = 0;
	for (unsigned i = 0; i < count; ++i)
	{
		uintptr_t cm = MovementOf(stuff[i]);
		if (cm)
		{
			out[n].character = stuff[i];
			out[n].cm = cm;
			++n;
		}
	}
	return n;
}

// Why the slot's plan ends now, from the character's state and the plan's age.
static PlanDropWhy SlotDropDue(const PlanView& v, uintptr_t character, const float pos[3], double now)
{
	const PlanMainState* m = PlanStoreMain(v.slot);
	double planAge = now - m->planTime;
	if (!character)
		return PlanDropDue(false, false, 0.0f, v.finalDest, v.finalDest, planAge, false);
	float moveDest[3];
	moveDest[0] = *(float*)(KLIB_MEMBER(3, v.cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
	moveDest[1] = 0.0f;
	moveDest[2] = *(float*)(KLIB_MEMBER(3, v.cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));
	return PlanDropDue(true, IslandK7IsUnconcious(character), DistXz(pos, v.finalDest), moveDest, v.finalDest, planAge,
	                   PlanIsModSend(moveDest, v.resend, v.resendCount, m->holdDest, m->haveHold, now - m->holdTime));
}

// The slot's wait and completion clocks: a wait begins when the waiting word rises; a portal leg
// is complete when the character stopped at its path's end outside the portal's reach.
static void UpdateClocks(int slot, const PlanView& v, double now, float distToPortal, bool portalLeg)
{
	PlanMainState* m = PlanStoreMain(slot);
	if (v.waiting && m->waitSince == 0.0)
	{
		m->waitSince = now;
		InterlockedIncrement(&PlannerCountersGet()->waits);
	}
	else if (!v.waiting)
		m->waitSince = 0.0;
	uintptr_t hc = *(uintptr_t*)(KLIB_MEMBER(3, v.cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
	bool complete = hc && PlanLegComplete(portalLeg ? 1 : 0, distToPortal,
	                                      *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE)),
	                                      *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_characterState, OFF_HC_ARRIVAL)));
	if (!complete)
		m->completeSince = 0.0;
	else if (m->completeSince == 0.0)
		m->completeSince = now;
}

// A re-plan from the character's current position toward the plan's destination. A goal-loaded
// re-plan whose goal is again found only by its footprint clears the slot's footprint flag, so
// that trigger fires once per plan rather than on every tick.
static bool Replan(const PlanView& v, uintptr_t character, double now, PlanReplanWhy why)
{
	float pos[3];
	CharPos(character, pos);
	Located start, goal;
	if (!Locate(v.finalDest, &goal) || !Locate(pos, &start))
		return false;
	s_memoCount = 0;
	const Built* b = SearchAndBuild(start, goal, v.finalDest);
	int verdict = PV_NONE;
	int slot = WritePlan(v.cm, pos, goal, v.finalDest, *b, now, &verdict, 1);
	if (slot < 0)
		return false;
	if (why == PRW_GOAL_LOADED && !goal.exact)
		PlanStoreMain(slot)->goalByFootprint = 0;
	return true;
}

static PlanReplanWhy SlotReplanDue(int slot, const PlanView& v, double now, float distToPortal)
{
	const PlanMainState* m = PlanStoreMain(slot);
	PlanReplanIn in;
	memset(&in, 0, sizeof(in));
	in.now = now;
	in.planTime = m->planTime;
	in.waitSince = m->waitSince;
	in.completeSince = m->completeSince;
	in.waitSeconds = s_waitSeconds;
	in.loadedChangedSincePlan = m->loadedGenAtPlan != s_loadedGen;
	in.awaitedLoaded = v.legIndex < PLAN_MAX_LEGS && ((v.loadedMask >> v.legIndex) & 1u) != 0;
	in.rungs = v.rungs;
	in.goalByFootprint = m->goalByFootprint;
	in.goalLoaded = v.legCount > 0 && IsDirLoaded(v.legs[v.legCount - 1].farSection);
	in.legIndex = v.legIndex;
	in.legCount = v.legCount;
	in.routeTruncated = v.routeTruncated;
	in.distToPortal = distToPortal;
	return PlanReplanDue(in);
}

// One armed slot: validation and drop, the loaded mask, the feed on an advance, the clocks and
// the re-plan. Returns whether it attempted a re-plan (the tick's per-tick budget counts attempts).
static bool TickSlot(int slot, const PlayerChar* players, int nPlayers, double now, bool mayReplan)
{
	PlanView v;
	if (!PlanStoreRead(slot, &v))
		return false;
	uintptr_t character = 0;
	for (int i = 0; i < nPlayers; ++i)
		if (players[i].cm == v.cm) { character = players[i].character; break; }
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	if (character)
		CharPos(character, pos);
	PlanDropWhy dropWhy = SlotDropDue(v, character, pos, now);
	if (dropWhy != PDW_NONE)
	{
		DropPlan(v.cm, dropWhy);
		return false;
	}
	unsigned mask = LoadedMaskOf(v.legs, v.legCount);
	if (mask != v.loadedMask)
	{
		PlanStoreSetLoaded(slot, mask);
		v.loadedMask = mask;
	}
	if (PlanStoreTakeArrival(slot))
	{
		int xy[2 * 8];
		int cells = PlanFeedCells(v.legs, v.legCount, v.legIndex, s_aheadTiles, CG_EXTERIOR_SLOTS, xy);
		for (int i = 0; i < cells; ++i)
			EnqueueCharacterZone(xy[2 * i], xy[2 * i + 1]);
	}
	bool onLeg = v.legIndex >= 0 && v.legIndex < v.legCount;
	float distToPortal = onLeg ? DistXz(pos, v.legs[v.legIndex].point) : 0.0f;
	UpdateClocks(slot, v, now, distToPortal, onLeg && !v.legs[v.legIndex].isDestination);
	PlanReplanWhy why = mayReplan ? SlotReplanDue(slot, v, now, distToPortal) : PRW_NONE;
	if (why == PRW_NONE)
		return false;
	if (Replan(v, character, now, why))
	{
		InterlockedIncrement(&PlannerCountersGet()->replans);
		InterlockedIncrement(&PlannerCountersGet()->replansBy[why]);
	}
	return true;
}

void PlannerTick(void* zoneMgr, double now)
{
	if (PlanStoreMode() == PLANNER_OFF) return;
	(void)zoneMgr;
	if (now - s_lastTick < TICK_SECONDS)
		return;
	s_lastTick = now;
	unsigned gen = CgStoreGen();
	if (gen != s_storeGen)
	{
		PlanStoreReset();
		s_storeGen = gen;
	}
	TakeSnapshot();
	PlayerChar players[PLAYER_SCAN_MAX];
	int nPlayers = ReadPlayers(players);
	int replans = 0;
	for (int slot = 0; slot < PLAN_SLOTS; ++slot)
	{
		if (!PlanStoreKey(slot))
			continue;
		if (TickSlot(slot, players, nPlayers, now, replans < REPLANS_PER_TICK))
			++replans;
	}
	PlannerReportTick(now);
}

} // namespace planner
