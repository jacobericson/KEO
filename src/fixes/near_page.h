#ifndef KENSHI_ZONE_OPT_FIXES_NEAR_PAGE_H
#define KENSHI_ZONE_OPT_FIXES_NEAR_PAGE_H

#include <stddef.h>

// Commits `bytes` of PAGE_READWRITE memory within rel32 reach of `nearAddr`,
// for a mid-function patch's stub: the mod DLL is too far from the exe for a
// rel32 branch to reach. Steps outward from `nearAddr` in allocation-
// granularity increments and takes the first free block VirtualAlloc accepts.
// Returns NULL when nothing within reach is free. The caller owns the block.
unsigned char* AllocateNearPages(unsigned __int64 nearAddr, size_t bytes);

#endif // KENSHI_ZONE_OPT_FIXES_NEAR_PAGE_H
