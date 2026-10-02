// island_components.cpp - Component snapshots, builder and router support.
// Main thread builds, publishes and logs; snapshot readers run on any thread.
// No mod locks. Dormant hook appends use the game's reserve call.

#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "movement/island_overlay_internal.h"
#include "zone/grid.h"
#include <intrin.h>
#include <iomanip>

namespace islands_detail {

const int MAX_COMPS       = 64;               // components published per snapshot
const int MAX_MEMBERS     = ZONE_GRID_COUNT;  // every zone at most once

} // namespace islands_detail
using namespace islands_detail;
namespace island_components_detail {
// =========================================================================
// Published snapshot (double buffer + per-buffer seqlock)
// =========================================================================
// Main-thread PublishSnapshot writes the inactive buffer, with seq odd
// during the copy and even before exchanging the active index. Main and AI
// hooks read through SnapReadComps or SnapCopyMembers, with two attempts.
// ResetBuilder publishes an invalid buffer on a load or ZoneManager change.
// The component answer is a behavior input: a torn read is rejected and the
// hook falls back to the original answer. The component overlay is dormant.
struct IslandSnapshot {
	volatile LONG  seq;          // odd while being written
	volatile LONG  valid;        // 0 = no components (save load / no zm)
	uintptr_t      zm;           // ZoneManager the indices refer to
	unsigned int   gen;
	int            compCount;
	short          comp[ZONE_GRID_COUNT];        // component id per zone, -1 = none
	unsigned short memberStart[MAX_COMPS];
	unsigned short memberCount[MAX_COMPS];
	unsigned short members[MAX_MEMBERS];         // zone indices grouped by component
};
} // namespace island_components_detail
using namespace island_components_detail;
namespace islands_detail {
static IslandSnapshot g_snap[2];
static volatile LONG  g_activeSnap = 0;

// Main thread writes these builder scalars in IslandTick, IslandReset,
// IslandRequestRebuild, WalkSetB, Rebuild and ResetBuilder. HookCellSpan
// reads only g_builderZm on any thread with an aligned pointer-sized load
// for the far-span rule; the other scalars are main-thread-only.
// Plain main-thread stores publish these scalars separately; ResetBuilder
// resets its state and IslandReset clears g_builderZm. The any-thread span
// reader takes only the aligned pointer; no coherent scalar set is promised,
// and an unresolved zone index leaves the far-span rule unchanged.
uintptr_t     g_builderZm = 0;
unsigned int  g_snapGen   = 0;
unsigned int  g_setBSig   = 0;
bool          g_haveSig   = false;
bool          g_rebuildRequested = false;
double        g_lastEligibility  = -1.0;
int            g_setBAccessible = 0;
int            g_curCompCount = 0;
int            g_curModZones = 0;


// Seqlock read of comp[] for up to two zones. b may be 0.
bool SnapReadComps(uintptr_t a, uintptr_t b, int* ca, int* cb)
{
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG idx = g_activeSnap;
		const IslandSnapshot* s = &g_snap[idx & 1];
		LONG seq1 = s->seq;
		if (seq1 & 1) continue;
		_ReadWriteBarrier();
		LONG valid = s->valid;
		uintptr_t zm = s->zm;
		int ra = -1, rb = -1;
		if (valid)
		{
			int ia = ZoneIndexOf(zm, a);
			int ib = ZoneIndexOf(zm, b);
			if (ia >= 0) ra = s->comp[ia];
			if (ib >= 0) rb = s->comp[ib];
		}
		_ReadWriteBarrier();
		LONG seq2 = s->seq;
		if (seq1 != seq2) continue;
		*ca = ra;
		*cb = rb;
		return true;
	}
	return false;
}

// Seqlock copy of the member list of t's component into buf.
// Returns member count, 0 with *outComp = -1 when t is in no component,
// or -1 on seqlock failure / oversize (caller falls back to the original).
int SnapCopyMembers(uintptr_t t, int* outComp, uintptr_t* outZm,
                    unsigned short* buf, int maxCount)
{
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG idx = g_activeSnap;
		const IslandSnapshot* s = &g_snap[idx & 1];
		LONG seq1 = s->seq;
		if (seq1 & 1) continue;
		_ReadWriteBarrier();
		LONG valid = s->valid;
		uintptr_t zm = s->zm;
		int comp = -1;
		int n = 0;
		bool oversize = false;
		if (valid)
		{
			int it = ZoneIndexOf(zm, t);
			if (it >= 0) comp = s->comp[it];
			if (comp >= 0 && comp < MAX_COMPS)
			{
				int start = s->memberStart[comp];
				n = s->memberCount[comp];
				if (n > maxCount || start + n > MAX_MEMBERS) { oversize = true; n = 0; }
				else memcpy(buf, &s->members[start], n * sizeof(unsigned short));
			}
			else if (comp >= MAX_COMPS)
				comp = -1;
		}
		_ReadWriteBarrier();
		LONG seq2 = s->seq;
		if (seq1 != seq2) continue;
		if (oversize) return -1;
		*outComp = comp;
		*outZm = zm;
		return n;
	}
	return -1;
}

// Append a ZoneMap* to a lektor<ZoneMap*> exactly as ZoneManager::getIsland does.
bool AppendLektor(uintptr_t lek, uintptr_t z)
{
	unsigned int count = *(unsigned int*)(KLIB_MEMBER(2, lek, ZoneLektor_count, OFF_LEKTOR_COUNT));
	unsigned int cap   = *(unsigned int*)(KLIB_MEMBER(2, lek, ZoneLektor_maxSize, OFF_LEKTOR_CAPACITY));
	if (count >= cap)
	{
		if (!game::g_gameFn.fn_lektorReserve) return false;
		game::g_gameFn.fn_lektorReserve((void*)lek, 2 * cap);   // 0 -> 10 inside the game
		cap = *(unsigned int*)(KLIB_MEMBER(2, lek, ZoneLektor_maxSize, OFF_LEKTOR_CAPACITY));
		if (count >= cap) return false;
	}
	uintptr_t* data = *(uintptr_t**)(KLIB_MEMBER(2, lek, ZoneLektor_stuff, OFF_LEKTOR_DATA));
	if (!data) return false;
	data[count] = z;
	*(unsigned int*)(KLIB_MEMBER(2, lek, ZoneLektor_count, OFF_LEKTOR_COUNT)) = count + 1;
	return true;
}


// =========================================================================
// Builder state (main thread only)
// =========================================================================


// Last Set B walk (for unexpl + the vanilla router list)
static unsigned short g_setBIdx[ZONE_GRID_COUNT];
static int            g_setBCount = 0;
static unsigned char  g_inSetB[ZONE_GRID_COUNT];

// Current published result (main-thread mirror for comparison + diagnostics)
static short          g_curComp[ZONE_GRID_COUNT];
static unsigned short g_curMemberStart[MAX_COMPS];
static unsigned short g_curMemberCount[MAX_COMPS];
static unsigned short g_curMembers[MAX_MEMBERS];
static unsigned char  g_curIsMod[ZONE_GRID_COUNT];
static bool           g_curValid = false;
static int            g_compOverflow = 0;
static bool           g_inputsLogged = false;

// Scratch for a rebuild
static short          g_ufParent[ZONE_GRID_COUNT];
static unsigned char  g_included[ZONE_GRID_COUNT];
static unsigned char  g_isMod[ZONE_GRID_COUNT];
static short          g_newComp[ZONE_GRID_COUNT];
static unsigned short g_newMemberStart[MAX_COMPS];
static unsigned short g_newMemberCount[MAX_COMPS];
static unsigned short g_newMembers[MAX_MEMBERS];
static short          g_rootComp[ZONE_GRID_COUNT];

static int UfFind(int i)
{
	while (g_ufParent[i] != i)
	{
		g_ufParent[i] = g_ufParent[g_ufParent[i]];
		i = g_ufParent[i];
	}
	return i;
}

static void UfUnion(int a, int b)
{
	int ra = UfFind(a), rb = UfFind(b);
	if (ra == rb) return;
	if (ra < rb) g_ufParent[rb] = ra; else g_ufParent[ra] = rb;
}

// Walk Set B (main thread). Fills g_setBIdx / g_inSetB and the signature.
void WalkSetB(uintptr_t zm)
{
	g_setBCount = 0;
	g_setBAccessible = 0;
	memset(g_inSetB, 0, sizeof(g_inSetB));

	uintptr_t setB = KLIB_MEMBER(2, zm, ZoneManager_activeZones, OFF_ZM_SET_B);
	unsigned long long size = *(unsigned long long*)(KLIB_MEMBER(2, setB, ZoneSetTable_size_, OFF_SET_SIZE));
	unsigned int sum = 0;
	if (size > 0 && size <= (unsigned long long)ZONE_GRID_COUNT * 4)
	{
		unsigned long long bucketCount = *(unsigned long long*)(KLIB_MEMBER(2, setB, ZoneSetTable_bucket_count_, OFF_SET_BUCKET_COUNT));
		uintptr_t buckets = *(uintptr_t*)(KLIB_MEMBER(2, setB, ZoneSetTable_buckets_, OFF_SET_BUCKETS));
		if (buckets && bucketCount > 0 && bucketCount < (1ull << 24))
		{
			uintptr_t node = *(uintptr_t*)(buckets + 8 * bucketCount);
			int iter = 0;
			int maxIter = (int)size + 16;
			while (node && iter++ < maxIter)
			{
				uintptr_t z = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_value_base_, OFF_SET_NODE_VALUE);
				int idx = ZoneIndexOf(zm, z);
				if (idx >= 0 && !g_inSetB[idx] && g_setBCount < ZONE_GRID_COUNT)
				{
					g_inSetB[idx] = 1;
					g_setBIdx[g_setBCount++] = (unsigned short)idx;
					sum += (unsigned int)idx;
					if (ZoneAccess(z)) g_setBAccessible++;
				}
				node = *(uintptr_t*)KLIB_MEMBER(2, node, ZoneSetNode_next_, 0);
			}
		}
	}
	g_setBSig = (unsigned int)size * 0x9E3779B1u
	          ^ sum * 0x85EBCA6Bu
	          ^ (unsigned int)g_setBAccessible * 0xC2B2AE35u;
}

static void PublishSnapshot(bool valid, uintptr_t zm)
{
	LONG cur = g_activeSnap;
	IslandSnapshot* w = &g_snap[(cur + 1) & 1];
	InterlockedIncrement(&w->seq);           // odd: writing
	_ReadWriteBarrier();
	w->valid = valid ? 1 : 0;
	w->zm = zm;
	w->gen = g_snapGen;
	w->compCount = valid ? g_curCompCount : 0;
	if (valid)
	{
		memcpy(w->comp, g_curComp, sizeof(w->comp));
		memcpy(w->memberStart, g_curMemberStart, sizeof(w->memberStart));
		memcpy(w->memberCount, g_curMemberCount, sizeof(w->memberCount));
		memcpy(w->members, g_curMembers, sizeof(w->members));
	}
	else
	{
		for (int i = 0; i < ZONE_GRID_COUNT; ++i) w->comp[i] = -1;
	}
	_ReadWriteBarrier();
	InterlockedIncrement(&w->seq);           // even: consistent
	InterlockedExchange(&g_activeSnap, (cur + 1) & 1);
}

static void DumpComponents();
static void LogInputsOnce(uintptr_t zm);

// Full rebuild (main thread). Publishes only when the result changes.
//
// g_isMod (and so every mod-seeded component below) is permanently empty:
// the mod never publishes a zone as accessible outside Set A/B any more (a private cell
// is +176 only; an adopted one is a real Set B member from the moment it
// can become accessible), so nothing can ever satisfy the old eligibility
// test (marked, accessible, not in Set B). The union-find and component
// derivation below still run -- they cost a linear pass over the grid, no
// more -- but seed zero components every time, so the component answer never
// applies: both hooks return the vanilla answer except where the far-span
// rule (hook_isInIsland) or the edge-ring filter (hook_getIsland) changes it.
void Rebuild(uintptr_t zm, double now)
{
	memset(g_included, 0, sizeof(g_included));
	memset(g_isMod, 0, sizeof(g_isMod));
	// Permanently 0 along with g_isMod (see above); flows into g_curModZones,
	// the mod= token on the Islands: and Island snapshot lines, and the *
	// per-member marker in DumpComponents, all of which read constant now.
	int modZones = 0;
	g_lastEligibility = now;

	// --- accessible Set B zones ---
	for (int i = 0; i < g_setBCount; ++i)
	{
		int idx = g_setBIdx[i];
		if (ZoneAccess(ZoneAt(zm, idx)))
			g_included[idx] = 1;
	}

	// --- union-find over neighbors[4] ---
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx) g_ufParent[idx] = (short)idx;
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx)
	{
		if (!g_included[idx]) continue;
		uintptr_t z = ZoneAt(zm, idx);
		for (int k = 0; k < ZONE_NEIGHBOR_COUNT; ++k)
		{
			uintptr_t nb = *(uintptr_t*)(KLIB_ZONE_NEIGHBOR(2, z, k));
			int ni = ZoneIndexOf(zm, nb);
			if (ni < 0 || !g_included[ni]) continue;
			UfUnion(idx, ni);
		}
	}

	// --- components with at least one mod zone ---
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx) { g_rootComp[idx] = -1; g_newComp[idx] = -1; }
	int compCount = 0;
	int overflow = 0;
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx)
	{
		if (!g_isMod[idx]) continue;
		int r = UfFind(idx);
		if (g_rootComp[r] >= 0) continue;
		if (compCount >= MAX_COMPS) { overflow++; continue; }
		g_rootComp[r] = (short)compCount++;
	}
	for (int c = 0; c < compCount; ++c) g_newMemberCount[c] = 0;
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx)
	{
		if (!g_included[idx]) continue;
		int c = g_rootComp[UfFind(idx)];
		if (c < 0) continue;
		g_newComp[idx] = (short)c;
		g_newMemberCount[c]++;
	}
	int pos = 0;
	for (int c = 0; c < compCount; ++c)
	{
		g_newMemberStart[c] = (unsigned short)pos;
		pos += g_newMemberCount[c];
		g_newMemberCount[c] = 0;   // reused as a fill cursor below
	}
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx)
	{
		int c = g_newComp[idx];
		if (c < 0) continue;
		g_newMembers[g_newMemberStart[c] + g_newMemberCount[c]++] = (unsigned short)idx;
	}

	// --- publish on change ---
	bool changed = !g_curValid
	            || compCount != g_curCompCount
	            || memcmp(g_newComp, g_curComp, sizeof(g_newComp)) != 0
	            || memcmp(g_isMod, g_curIsMod, sizeof(g_isMod)) != 0;
	if (!changed && compCount > 0)
	{
		if (memcmp(g_newMemberStart, g_curMemberStart, compCount * sizeof(unsigned short)) != 0
		    || memcmp(g_newMemberCount, g_curMemberCount, compCount * sizeof(unsigned short)) != 0
		    || memcmp(g_newMembers, g_curMembers, pos * sizeof(unsigned short)) != 0)
			changed = true;
	}
	g_compOverflow = overflow;
	g_curModZones = modZones;
	if (!changed) return;

	memcpy(g_curComp, g_newComp, sizeof(g_curComp));
	memcpy(g_curIsMod, g_isMod, sizeof(g_curIsMod));
	memcpy(g_curMemberStart, g_newMemberStart, sizeof(g_curMemberStart));
	memcpy(g_curMemberCount, g_newMemberCount, sizeof(g_curMemberCount));
	memcpy(g_curMembers, g_newMembers, sizeof(g_curMembers));
	g_curCompCount = compCount;
	g_curValid = true;
	g_snapGen++;
	PublishSnapshot(true, zm);
	DumpComponents();
	if (!g_inputsLogged && pos > 0)
		LogInputsOnce(zm);
}

void ResetBuilder()
{
	g_haveSig = false;
	g_rebuildRequested = false;
	g_lastEligibility = -1.0;
	g_setBCount = 0;
	g_setBAccessible = 0;
	memset(g_inSetB, 0, sizeof(g_inSetB));
	for (int i = 0; i < ZONE_GRID_COUNT; ++i) g_curComp[i] = -1;
	memset(g_curIsMod, 0, sizeof(g_curIsMod));
	g_curCompCount = 0;
	g_curValid = false;
	g_curModZones = 0;
	g_compOverflow = 0;
	g_snapGen++;
	PublishSnapshot(false, 0);
}

// Component id of a zone in the CURRENT (main-thread) result, with liveness.
int CurComp(uintptr_t zm, uintptr_t z)
{
	if (!g_curValid || zm != g_builderZm) return -1;
	int idx = ZoneIndexOf(zm, z);
	if (idx < 0) return -1;
	if (!ZoneAccess(z)) return -1;
	return g_curComp[idx];
}

// The island list the router receives for zone t in the CURRENT configuration:
// vanilla (Set B zones with t's label) when the hooks pass through, the overlay
// answer when they are live. Mirrors hook_getIsland exactly.
int RouterList(uintptr_t zm, uintptr_t t, unsigned short* out, int maxOut)
{
	int n = 0;
	int tl = ZoneLabel(t);
	bool live = IslandHooksLive();
	int ct = live ? CurComp(zm, t) : -1;

	if (!(live && ct >= 0 && tl <= 0))
	{
		for (int i = 0; i < g_setBCount && n < maxOut; ++i)
		{
			int idx = g_setBIdx[i];
			if (ZoneLabel(ZoneAt(zm, idx)) == tl)
				out[n++] = (unsigned short)idx;
		}
	}
	if (live && ct >= 0)
	{
		int start = g_curMemberStart[ct];
		int cnt = g_curMemberCount[ct];
		for (int i = 0; i < cnt && n < maxOut; ++i)
		{
			int idx = g_curMembers[start + i];
			uintptr_t z = ZoneAt(zm, idx);
			if (!ZoneAccess(z)) continue;
			if (tl > 0 && ZoneLabel(z) == tl) continue;
			out[n++] = (unsigned short)idx;
		}
	}
	return n;
}

static void DumpComponents()
{
#ifdef KEO_DEBUG
	{
		std::ostringstream ss;
		ss << "Island snapshot gen=" << g_snapGen
		   << " comps=" << g_curCompCount << " mod=" << g_curModZones
		   << " setB=" << g_setBCount << "/" << g_setBAccessible << "acc";
		if (g_compOverflow) ss << " compOverflow=" << g_compOverflow;
		LogDebug(ss.str());
	}
	for (int c = 0; c < g_curCompCount; ++c)
	{
		std::ostringstream ss;
		ss << "Island comp " << c << " (" << g_curMemberCount[c] << "):";
		int shown = 0;
		for (int i = 0; i < g_curMemberCount[c]; ++i)
		{
			int idx = g_curMembers[g_curMemberStart[c] + i];
			if (shown++ >= 40) { ss << " ..."; break; }
			ss << " (" << IdxGX(idx) << "," << IdxGY(idx) << ")";
			if (g_curIsMod[idx]) ss << "*";
			else ss << "l" << ZoneLabel(ZoneAt(g_builderZm, idx));
		}
		LogDebug(ss.str());
	}
#endif
}

// The router's bounds minimum and navmesh+468 must match
// centre - zoneStep/2 and zoneStep for the zones we route through.
static void LogInputsOnce(uintptr_t zm)
{
	g_inputsLogged = true;
	uintptr_t navmesh = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	float size = navmesh ? *(float*)(KLIB_MEMBER(2, navmesh, NavMesh_cellSize, OFF_NAVMESH_ZONE_SIZE)) : 0.0f;
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "Island router inputs: navmesh+468=" << size
	   << " zoneStep=" << zoneStepX;
	int shown = 0;
	for (int c = 0; c < g_curCompCount && shown < 3; ++c)
	{
		for (int i = 0; i < g_curMemberCount[c] && shown < 3; ++i)
		{
			int idx = g_curMembers[g_curMemberStart[c] + i];
			uintptr_t z = ZoneAt(zm, idx);
			float minX = *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mCenter_x, OFF_ZONE_AABB_CENTER))     - *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mHalfSize_x, OFF_ZONE_AABB_HALF));
			float minZ = *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mCenter_z, OFF_ZONE_AABB_CENTER + 8)) - *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mHalfSize_z, OFF_ZONE_AABB_HALF + 8));
			ss << " | (" << IdxGX(idx) << "," << IdxGY(idx) << ") min=(" << minX << "," << minZ
			   << ") expect=(" << (GetZoneCenterX((void*)z) - zoneStepX * 0.5f) << ","
			   << (GetZoneCenterZ((void*)z) - zoneStepZ * 0.5f) << ")";
			shown++;
		}
	}
	LogMsg(ss.str());
}

// Accessible, content-bearing zones outside Set B: since the mod can no
// longer publish a zone as accessible itself, every one of these is
// genuinely unexplained rather than one the mod's own mark would have
// accounted for.
int CountUnexplained(uintptr_t zm)
{
	int n = 0;
	for (int idx = 0; idx < ZONE_GRID_COUNT; ++idx)
	{
		uintptr_t z = ZoneAt(zm, idx);
		if (!ZoneAccess(z)) continue;
		if (!*(void**)(KLIB_MEMBER(2, z, ZoneMap_mapContent, OFF_ZONE_CONTENT))) continue;
		if (g_inSetB[idx]) continue;
		n++;
	}
	return n;
}


} // namespace
using namespace islands_detail;
