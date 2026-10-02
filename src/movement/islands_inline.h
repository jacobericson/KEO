// islands_inline.h - Shared inline island field and grid accessors.
// Any thread, with no locks, allocation or logging; callers own object lifetime.

#ifndef KEO_ISLANDS_INLINE_H
#define KEO_ISLANDS_INLINE_H

#include "base/config.h"

namespace islands_detail {

// Zone index (y + x*64) from a ZoneMap pointer, or -1 if it is not an entry of
// this ZoneManager's zone array. Pure arithmetic: safe on any thread.
inline int ZoneIndexOf(uintptr_t zm, uintptr_t z)
{
	if (!zm || !z) return -1;
	uintptr_t base = KLIB_MEMBER(2, zm, ZoneManager_worldMap, OFF_ZM_ZONE_BASE);
	if (z < base) return -1;
	uintptr_t off = z - base;
	if (off % ZONE_ENTRY_SIZE) return -1;
	uintptr_t idx = off / ZONE_ENTRY_SIZE;
	if (idx >= (uintptr_t)ZONE_GRID_COUNT) return -1;
	return (int)idx;
}

inline uintptr_t ZoneAt(uintptr_t zm, int idx)
{
	return KlibZoneEntry(zm, idx);
}

inline int ZoneLabel(uintptr_t z)     { return *(int*)(KLIB_MEMBER(2, z, ZoneMap_island, OFF_ZONE_ISLAND)); }
inline bool ZoneAccess(uintptr_t z)   { return *(unsigned char*)(KLIB_MEMBER(2, z, ZoneMap_stateT_mainThreadData__zoneIsLoaded, OFF_ZONE_IS_ACCESS)) != 0; }
inline bool ZoneLoadingF(uintptr_t z) { return *(unsigned char*)(KLIB_MEMBER(2, z, ZoneMap_stateT_mainThreadData__zoneBeingLoaded, OFF_ZONE_IS_LOADING)) != 0; }
inline int IdxGX(int idx) { return idx / 64; }
inline int IdxGY(int idx) { return idx % 64; }


} // namespace

#endif // KEO_ISLANDS_INLINE_H
