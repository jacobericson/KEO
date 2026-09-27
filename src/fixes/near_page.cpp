#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/near_page.h"
#include <windows.h>

unsigned char* AllocateNearPages(unsigned __int64 nearAddr, size_t bytes)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	unsigned __int64 gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
	const unsigned __int64 kReach = 0x7F000000ULL;   // margin under 2 GB
	unsigned __int64 origin = (nearAddr / gran) * gran;

	for (unsigned __int64 step = gran; step < kReach; step += gran)
	{
		unsigned __int64 candidates[2];
		candidates[0] = origin + step;
		candidates[1] = (origin > step) ? (origin - step) : 0;

		for (int i = 0; i < 2; ++i)
		{
			if (candidates[i] == 0)
				continue;
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery((LPCVOID)(uintptr_t)candidates[i], &mbi, sizeof(mbi)))
				continue;
			if (mbi.State != MEM_FREE || mbi.RegionSize < bytes)
				continue;
			void* got = VirtualAlloc((LPVOID)(uintptr_t)candidates[i],
				bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (got)
				return (unsigned char*)got;
		}
	}
	return NULL;
}
