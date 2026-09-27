#ifndef KENSHI_ZONE_OPT_BASE_HASH_H
#define KENSHI_ZONE_OPT_BASE_HASH_H

#include <stddef.h>

// 32-bit FNV-1a. The navmesh cache's keys are built from it (the L2 file name
// and the settings hash in the L2 header), so its values never change.

const unsigned int FNV1A32_OFFSET = 2166136261u;

inline unsigned int Fnv1a32Mix(unsigned int h, unsigned char byte)
{
	h ^= byte;
	h *= 16777619u;
	return h;
}

inline unsigned int Fnv1a32Bytes(unsigned int h, const void* data, size_t len)
{
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < len; ++i)
		h = Fnv1a32Mix(h, p[i]);
	return h;
}

inline unsigned int Fnv1a32(const void* data, size_t len)
{
	return Fnv1a32Bytes(FNV1A32_OFFSET, data, len);
}

#endif
