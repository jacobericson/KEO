#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "diag/physx_pool_sets.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace physx_pool_sets_detail
{

struct CallerRange
{
	unsigned __int64 begin;
	unsigned __int64 end;      // exclusive
	int              role;
};

// The four direct callers of loadPhysXResource, as executable RVAs. Sizes are
// the function extents, so a return address anywhere in the body attributes
// correctly even though every call goes through a j_ thunk.
const CallerRange kCallers[] =
{
	{ 0x7E49E0, 0x7E49E0 + 0x26A, PXP_ROLE_PHYS },
	{ 0x7E4C50, 0x7E4C50 + 0x39D, PXP_ROLE_TRIG },
	{ 0x3C7870, 0x3C7870 + 0xCA0, PXP_ROLE_NAV  },
	{ 0x7E5000, 0x7E5000 + 0x3D6, PXP_ROLE_XML  }
};
const int kCallerCount = (int)(sizeof(kCallers) / sizeof(kCallers[0]));

size_t AppendLiteral(char* out, size_t cap, const char* text)
{
	if (!out || cap == 0)
		return 0;
	size_t n = strlen(text);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, text, n);
	out[n] = 0;
	return n;
}

} // namespace
using namespace physx_pool_sets_detail;


const char* PhysXPoolRoleName(int role)
{
	switch (role)
	{
	case PXP_ROLE_PHYS:      return "phys";
	case PXP_ROLE_TRIG:      return "trig";
	case PXP_ROLE_NAV:       return "nav";
	case PXP_ROLE_XML:       return "xml";
	case PXP_ROLE_EXE_OTHER: return "other";
	case PXP_ROLE_FOREIGN:   return "foreign";
	default:                 return "?";
	}
}

int PhysXPoolClassifyCaller(unsigned __int64 retRva, bool insideExe)
{
	if (!insideExe)
		return PXP_ROLE_FOREIGN;
	for (int i = 0; i < kCallerCount; ++i)
	{
		if (retRva >= kCallers[i].begin && retRva < kCallers[i].end)
			return kCallers[i].role;
	}
	return PXP_ROLE_EXE_OTHER;
}

size_t PhysXPoolFormatRoleMask(char* out, size_t cap, unsigned long mask)
{
	if (!out || cap == 0)
		return 0;
	out[0] = 0;
	size_t pos = 0;
	for (int i = 0; i < PXP_ROLE_COUNT; ++i)
	{
		if ((mask & (1UL << i)) == 0)
			continue;
		const char* tag = PhysXPoolRoleName(i);
		size_t need = strlen(tag) + (pos ? 1u : 0u);
		if (pos + need + 1 > cap)
			break;
		if (pos)
			out[pos++] = '|';
		memcpy(out + pos, tag, strlen(tag));
		pos += strlen(tag);
		out[pos] = 0;
	}
	if (pos == 0)
		return AppendLiteral(out, cap, "-");
	return pos;
}


void PhysXPoolSetReset(PhysXPoolSet* set)
{
	if (!set)
		return;
	memset((void*)set->slot, 0, sizeof(set->slot));
	set->distinct = 0;
	set->overflow = 0;
	set->samples  = 0;
}

int PhysXPoolSetInsert(PhysXPoolSet* set, unsigned __int64 key)
{
	if (!set)
		return PXP_INSERT_FULL;

	InterlockedIncrement((volatile LONG*)&set->samples);

	// 0 marks an empty slot, so a key that hashes to 0 is nudged rather than
	// dropped; the collision this can create is one extra distinct value at
	// worst, which cannot turn an unbounded spread into a small one.
	if (key == 0)
		key = 1;

	const unsigned __int64 mask = (unsigned __int64)(PXP_SET_SLOTS - 1);
	unsigned __int64 index = key & mask;

	for (int probe = 0; probe < PXP_SET_SLOTS; ++probe)
	{
		volatile __int64* cell = &set->slot[index];
		__int64 seen = *cell;
		if (seen == (__int64)key)
			return PXP_INSERT_PRESENT;
		if (seen == 0)
		{
			__int64 prior = InterlockedCompareExchange64(
				(volatile LONGLONG*)cell, (LONGLONG)key, 0);
			if (prior == 0)
			{
				InterlockedIncrement((volatile LONG*)&set->distinct);
				return PXP_INSERT_NEW;
			}
			if (prior == (__int64)key)
				return PXP_INSERT_PRESENT;
		}
		index = (index + 1) & mask;
	}

	InterlockedIncrement((volatile LONG*)&set->overflow);
	return PXP_INSERT_FULL;
}

unsigned __int64 PhysXPoolHashBytes(const void* bytes, size_t count, unsigned __int64 seed)
{
	const unsigned char* p = (const unsigned char*)bytes;
	unsigned __int64 h = seed;
	if (!p)
		return h;
	for (size_t i = 0; i < count; ++i)
	{
		h ^= (unsigned __int64)p[i];
		h *= 0x100000001B3ULL;
	}
	return h;
}

unsigned __int64 PhysXPoolHashU32(unsigned int value, unsigned __int64 seed)
{
	return PhysXPoolHashBytes(&value, sizeof(value), seed);
}

unsigned __int64 PhysXPoolHashScale(float x, float y, float z, unsigned __int64 seed)
{
	float v[3];
	v[0] = x; v[1] = y; v[2] = z;
	return PhysXPoolHashBytes(v, sizeof(v), seed);
}


size_t PhysXPoolFormatSet(char* out, size_t cap, const PhysXPoolSet* set)
{
	if (!out || cap == 0)
		return 0;
	if (!set || set->samples == 0)
		return AppendLiteral(out, cap, "?");

	char buf[32];
	if (set->overflow > 0)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%ld+", (long)set->distinct);
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%ld", (long)set->distinct);
	return AppendLiteral(out, cap, buf);
}

size_t PhysXPoolFormatCount(char* out, size_t cap, long value, bool sampled)
{
	if (!out || cap == 0)
		return 0;
	if (!sampled)
		return AppendLiteral(out, cap, "?");
	char buf[32];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%ld", value);
	return AppendLiteral(out, cap, buf);
}
