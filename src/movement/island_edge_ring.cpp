#include "movement/island_edge_ring.h"


#include "movement/islands_internal.h"
#include "movement/island_edge_ring_policy.h"
#include "game/game.h"

namespace island_edge_ring_detail {

// getZoneEdge's ray enters the adjacent ring cell 1 unit past the crossed
// face: radius 0 would park the character in its own cell forever, so 1 is
// the smallest radius that makes progress -- and only once every neighbour
// at that radius is actually present in the list (IslandRingComplete).
const int EDGE_RING_RADIUS = 1;

volatile long g_armed    = 0;

volatile long g_raEdge   = 0;
volatile long g_raSmell  = 0;
volatile long g_raOther  = 0;
volatile long g_filt     = 0;   // calls where the ring removed >= 1 entry
volatile long g_rm       = 0;   // cells removed, summed over those calls
volatile long g_partial  = 0;   // the ring is missing an in-grid neighbour -> left untouched
volatile long g_skip     = 0;   // armed edge-ra call with no resolvable start cell
volatile long g_big      = 0;   // armed edge-ra call whose list exceeds RING_COPY_MAX

} // namespace
using namespace island_edge_ring_detail;

void IslandEdgeRingArm(bool installed)
{
	InterlockedExchange(&g_armed, installed ? 1 : 0);
}

bool IslandEdgeRingArmed()
{
	return InterlockedCompareExchange(&g_armed, 0, 0) != 0;
}

void IslandEdgeRingApply(void* ra, uintptr_t zm, uintptr_t t, uintptr_t lektorOut)
{
	bool isEdge  = ra == GameAddr(RVA_GETISLAND_RET_EDGE);
	bool isSmell = !isEdge && ra == GameAddr(RVA_GETISLAND_RET_SMELL);
	if (isEdge)       InterlockedIncrement(&g_raEdge);
	else if (isSmell) InterlockedIncrement(&g_raSmell);
	else              InterlockedIncrement(&g_raOther);

	// Only getZoneEdge's own call site is filtered, only while armed, and
	// only when the hook actually has a lektor to work on.
	if (!isEdge || !IslandEdgeRingArmed() || !lektorOut)
		return;

	int s = IslandZoneIndexOf(zm, t);
	if (s < 0)
	{
		InterlockedIncrement(&g_skip);
		return;
	}

	unsigned int n = *(unsigned int*)(KLIB_MEMBER(2, lektorOut, ZoneLektor_count, OFF_LEKTOR_COUNT));
	if (n == 0)
		return;   // nothing to filter
	if (!IslandRingFitsCopyBound((int)n))
	{
		InterlockedIncrement(&g_big);
		return;
	}

	uintptr_t* data = *(uintptr_t**)(KLIB_MEMBER(2, lektorOut, ZoneLektor_stuff, OFF_LEKTOR_DATA));
	if (!data)
		return;

	int idxBuf[RING_COPY_MAX];
	for (unsigned int i = 0; i < n; ++i)
		idxBuf[i] = IslandZoneIndexOf(zm, data[i]);

	// Filter only when the start cell and every one of its in-grid
	// neighbours are present: getZoneEdge's ray is then always left a listed
	// face to cross, wherever the target sits. An incomplete ring is left
	// untouched -- filtering it can strand the character on its own start
	// face, the same park a radius of 0 would cause.
	if (!IslandRingComplete(idxBuf, (int)n, s, EDGE_RING_RADIUS))
	{
		InterlockedIncrement(&g_partial);
		return;
	}

	int posBuf[RING_COPY_MAX];
	int kept = IslandRingCompact(idxBuf, (int)n, s, EDGE_RING_RADIUS, posBuf);
	// The start cell is always one of the required neighbours, so a complete
	// ring always keeps at least it: kept > 0 here.
	if ((unsigned int)kept == n)
		return;   // nothing would be removed

	InterlockedIncrement(&g_filt);
	InterlockedExchangeAdd(&g_rm, (long)(n - (unsigned int)kept));

	if (!IslandRingShouldWrite(islandEdgeRingEnabled, kept))
		return;   // false: computed and counted, list left untouched

	// posBuf[j] >= j always (IslandRingCompact walks idx[] in order), so this
	// forward copy is safe in place: it only shrinks the lektor's own count,
	// never its capacity or stuff pointer, and needs no fn_lektorReserve.
	for (int j = 0; j < kept; ++j)
		data[j] = data[posBuf[j]];
	*(unsigned int*)(KLIB_MEMBER(2, lektorOut, ZoneLektor_count, OFF_LEKTOR_COUNT)) = (unsigned int)kept;
}

void IslandEdgeRingAppendSummary(std::ostringstream& ss)
{
	ss << " ring=" << IslandEdgeRingModeStr()
	   << " ra=edge" << InterlockedCompareExchange(&g_raEdge, 0, 0)
	   << "/smell" << InterlockedCompareExchange(&g_raSmell, 0, 0)
	   << "/other" << InterlockedCompareExchange(&g_raOther, 0, 0)
	   << " filt=" << InterlockedCompareExchange(&g_filt, 0, 0)
	   << " rm=" << InterlockedCompareExchange(&g_rm, 0, 0)
	   << " partial=" << InterlockedCompareExchange(&g_partial, 0, 0)
	   << " skip=" << InterlockedCompareExchange(&g_skip, 0, 0)
	   << " big=" << InterlockedCompareExchange(&g_big, 0, 0);
}


const char* IslandEdgeRingModeStr()
{
	return islandEdgeRingEnabled ? "true" : "false";
}
