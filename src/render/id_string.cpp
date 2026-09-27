#include "render/id_string.h"
#include <string.h>

// No Windows or game calls, so the host tests link it directly.

static const unsigned int IDSTRING_SEED = 0x3A8EFA67;

static unsigned int Rotl32(unsigned int x, int r)
{
	return (x << r) | (x >> (32 - r));
}

unsigned int MurmurHash3_x86_32(const void* key, size_t len, unsigned int seed)
{
	const unsigned char* data = (const unsigned char*)key;
	const size_t nblocks = len / 4;
	const unsigned int c1 = 0xCC9E2D51;
	const unsigned int c2 = 0x1B873593;
	unsigned int h1 = seed;

	for (size_t i = 0; i < nblocks; ++i)
	{
		unsigned int k1;
		memcpy(&k1, data + i * 4, 4);
		k1 *= c1;
		k1 = Rotl32(k1, 15);
		k1 *= c2;
		h1 ^= k1;
		h1 = Rotl32(h1, 13);
		h1 = h1 * 5 + 0xE6546B64;
	}

	const unsigned char* tail = data + nblocks * 4;
	unsigned int k1 = 0;
	switch (len & 3)
	{
	case 3: k1 ^= (unsigned int)tail[2] << 16;
	case 2: k1 ^= (unsigned int)tail[1] << 8;
	case 1: k1 ^= tail[0];
	        k1 *= c1;
	        k1 = Rotl32(k1, 15);
	        k1 *= c2;
	        h1 ^= k1;
	}

	h1 ^= (unsigned int)len;
	h1 ^= h1 >> 16;
	h1 *= 0x85EBCA6B;
	h1 ^= h1 >> 13;
	h1 *= 0xC2B2AE35;
	h1 ^= h1 >> 16;
	return h1;
}

unsigned int IdStringHash(const char* name)
{
	return MurmurHash3_x86_32(name, strlen(name), IDSTRING_SEED);
}
