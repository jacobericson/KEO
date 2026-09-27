#pragma once
#include <stddef.h>
#include <stdint.h>

// Explicit aliases only. No instruction decoding: shared-hook E9 detours
// at an implementation entry must never change its identity.
struct KlibAddressEntry
{
	const char* name;
	uintptr_t legacyRva;
	uintptr_t implementationRva;
	uintptr_t resolved;
};
inline bool KlibAddressesMatch(uintptr_t base, const KlibAddressEntry* entries,
                              size_t count, size_t* bad)
{
	if (!base) { if (bad) *bad = 0; return false; }
	for (size_t i = 0; i < count; ++i)
	{
		if (!entries[i].resolved || entries[i].resolved != base + entries[i].implementationRva)
		{
			if (bad) *bad = i;
			return false;
		}
	}
	return true;
}
inline uintptr_t KlibFindAddress(const KlibAddressEntry* entries, size_t count, uintptr_t rva)
{
	for (size_t i = 0; i < count; ++i)
		if (entries[i].legacyRva == rva || entries[i].implementationRva == rva)
			return entries[i].resolved;
	return 0;
}
