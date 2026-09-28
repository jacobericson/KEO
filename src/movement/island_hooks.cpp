// island_hooks.cpp - Paired island routing hooks and install state.
// Any caller thread; active mod filtering takes no locks and does not allocate or log.
// Snapshot reads try at most twice; dormant appends use the game's reserve call.
// Both hooks arm together, and the caller address is captured in the hook itself.

#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "movement/island_edge_ring.h"
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace islands_detail {

const int HOOK_COPY_MAX   = 512;              // getIsland stack copy bound
const int ISIN_FAR_CELLS = 2;         // cell span at which a direct path spans unloaded ground
volatile long g_farSpanArmed   = 0;

} // namespace
using namespace islands_detail;

// =========================================================================
// Hooks (any thread)
// =========================================================================

void IslandSetHooksInstalled(bool installed)
{
	InterlockedExchange(&g_hooksInstalled, installed ? 1 : 0);
	InterlockedExchange(&g_farSpanArmed, (installed && movement::g_movementCfg.cfg_islandFarSpan > 0) ? 1 : 0);
	IslandEdgeRingArm(installed);
}

bool IslandFarSpanArmed()
{
	return InterlockedCompareExchange(&g_farSpanArmed, 0, 0) != 0;
}

bool IslandHooksLive()
{
	return movement::g_movementCfg.islandFixEnabled && InterlockedCompareExchange(&g_hooksInstalled, 0, 0) != 0;
}

// Cell span between two zone entries of the zone array the builder last
// published, or -1 when either is not an entry of it (or nothing is published
// yet). Any thread: pointer arithmetic on a plain aligned load.
static int HookCellSpan(uintptr_t a, uintptr_t b)
{
	uintptr_t zm = g_builderZm;
	int ia = ZoneIndexOf(zm, a), ib = ZoneIndexOf(zm, b);
	if (ia < 0 || ib < 0) return -1;
	return IslandCellSpan(IdxGX(ia), IdxGY(ia), IdxGX(ib), IdxGY(ib));
}

// Classify one positive answer. Any thread: Interlocked counters only, no
// lock, no allocation and no logging. An unresolvable span is counted as
// span-unknown rather than dropped.
static void IsInIslandCensus(uintptr_t a, int span)
{
	if (span < 0)
	{
		InterlockedIncrement(&g_isInSpanUnk);
		return;
	}
	for (;;)
	{
		long seen = InterlockedCompareExchange(&g_isInMaxSpan, 0, 0);
		if (span <= seen) break;
		if (InterlockedCompareExchange(&g_isInMaxSpan, span, seen) == seen) break;
	}
	if (span < ISIN_FAR_CELLS) return;

	InterlockedIncrement(&g_isInFarTrue);
	if (ZoneLabel(a) == 0) InterlockedIncrement(&g_isInTrueZero);
	else                   InterlockedIncrement(&g_isInTrueLabel);
}

bool hook_isInIsland(void* zoneA, void* zoneB)
{
	InterlockedIncrement(&g_isInCalls);
	uintptr_t a = (uintptr_t)zoneA;
	uintptr_t b = (uintptr_t)zoneB;

	bool vanilla = orig_isInIsland
		? orig_isInIsland(zoneA, zoneB)
		: (b != 0 && a != 0 && ZoneLabel(a) == ZoneLabel(b));

	InterlockedIncrement(vanilla ? &g_isInVanTrue : &g_isInVanFalse);

	// 1. Vanilla NULL rule.
	if (!a || !b)
		return vanilla;

	if (vanilla)
	{
		int span = HookCellSpan(a, b);
		IsInIslandCensus(a, span);

		// Far-span rule: a direct path across several cells exhausts the
		// search budget, while the false branch walks there leg by leg.
		// Ahead of step 2, which returns every labelled match unchanged.
		if (InterlockedCompareExchange(&g_farSpanArmed, 0, 0))
		{
			int la0 = ZoneLabel(a);
			if (span < 0 && la0 > 0)
				InterlockedIncrement(&g_isInRuleUnk);
			else if (IslandFarSpanFlips(true, la0, span, movement::g_movementCfg.cfg_islandFarSpan))
			{
				InterlockedIncrement(&g_isInRuleFlip);
				InterlockedIncrement(&g_isInFlipSpan[IslandSpanBucket(span)]);
				return false;
			}
		}
	}

	// 2. Vanilla positive match: each vanilla island lies inside one component,
	//    so this can never contradict the overlay.
	int la = ZoneLabel(a);
	if (la > 0 && la == ZoneLabel(b))
		return vanilla;

	// 3. Component answer when either zone lives in one.
	int ca, cb;
	if (!SnapReadComps(a, b, &ca, &cb))
	{
		InterlockedIncrement(&g_isInSeqFail);
		return vanilla;
	}
	if (ca >= 0 && !ZoneAccess(a)) ca = -1;
	if (cb >= 0 && !ZoneAccess(b)) cb = -1;
	if (ca < 0 && cb < 0)
		return vanilla;    // 4. outside every component: vanilla (incl. 0 == 0)

	bool answer = (ca >= 0 && ca == cb);
	if (answer != vanilla)
		InterlockedIncrement(&g_isInFlips);
	if (!IslandHooksLive())
		return vanilla;
	return answer;
}

void* hook_getIsland(void* zoneMgr, void* zone, void* lektorOut)
{
	// Captured before anything else can push a frame: island_edge_ring.cpp
	// reads this to tell getZoneEdge's own call site from every other
	// caller, on every return path below.
	void* ra = _ReturnAddress();

	InterlockedIncrement(&g_getIslCalls);
	uintptr_t t = (uintptr_t)zone;
	uintptr_t out = (uintptr_t)lektorOut;
	uintptr_t zm = (uintptr_t)zoneMgr;
	void* result;

	if (!t || !out)
	{
		result = orig_getIsland(zoneMgr, zone, lektorOut);
	}
	else
	{
		unsigned short members[HOOK_COPY_MAX];
		int ct = -1;
		uintptr_t snapZm = 0;
		int n = SnapCopyMembers(t, &ct, &snapZm, members, HOOK_COPY_MAX);
		if (n < 0)
		{
			InterlockedIncrement(&g_getIslFallback);
			result = orig_getIsland(zoneMgr, zone, lektorOut);
		}
		else if (ct < 0 || snapZm != zm || !ZoneAccess(t))
		{
			result = orig_getIsland(zoneMgr, zone, lektorOut);
		}
		else
		{
			int tl = ZoneLabel(t);
			bool live = IslandHooksLive();
			result = NULL;

			// t.island <= 0: the original would return every Set B zone labelled
			// 0 (including zones that just entered Set B), which can capture the
			// router's ray. Answer from the component only.
			if (tl > 0 || !live)
				result = orig_getIsland(zoneMgr, zone, lektorOut);

			long appended = 0;
			for (int i = 0; i < n; ++i)
			{
				uintptr_t z = ZoneAt(zm, members[i]);
				if (!ZoneAccess(z)) continue;
				if (tl > 0 && ZoneLabel(z) == tl) continue;   // the original already appended it
				if (live && !AppendLektor(out, z)) break;
				appended++;
			}
			if (appended)
				InterlockedExchangeAdd(&g_getIslAppended, appended);
		}
	}

	// Runs on every return path above, including the early orig_getIsland
	// fallbacks: whichever branch filled the lektor, a getZoneEdge caller
	// still needs its list ring-filtered.
	IslandEdgeRingApply(ra, zm, t, out);
	return result;
}
