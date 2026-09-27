#include "diag/module_bases.h"

// No CRT stream, no allocation: this formatter is meant to be safe to call
// from the crash path, where both are off limits.

namespace module_bases_detail
{
	void AppendChar(char* out, size_t cap, size_t* n, char c)
	{
		if (*n + 1 < cap)
			out[(*n)++] = c;
	}

	void AppendStr(char* out, size_t cap, size_t* n, const char* s)
	{
		while (s && *s)
			AppendChar(out, cap, n, *s++);
	}

	void AppendHex(char* out, size_t cap, size_t* n, unsigned __int64 v)
	{
		static const char* kHex = "0123456789ABCDEF";
		char tmp[16];
		int tn = 0;
		if (v == 0)
			tmp[tn++] = '0';
		while (v > 0 && tn < 16)
		{
			tmp[tn++] = kHex[(size_t)(v & 0xF)];
			v >>= 4;
		}
		while (tn > 0)
			AppendChar(out, cap, n, tmp[--tn]);
	}

	size_t HexDigits(unsigned __int64 v)
	{
		size_t d = 1;
		while (v >>= 4)
			++d;
		return d;
	}

	size_t StrLen(const char* s)
	{
		size_t n = 0;
		while (s && s[n])
			++n;
		return n;
	}

	// " name=0xBASE+0xSIZE", the exact length AppendChar/AppendStr/AppendHex
	// would write for this entry -- computed up front so a not-quite-fitting
	// entry can be dropped whole, never emitted with its tail clipped off.
	size_t EntryLen(const ModuleBaseEntry& e)
	{
		return 1 + StrLen(e.name) + 3 + HexDigits(e.base) + 3 + HexDigits(e.size);
	}
}
using namespace module_bases_detail;

size_t FormatModuleBases(char* out, size_t cap, const ModuleBaseEntry* mods, int count,
                          int* outTruncated)
{
	if (outTruncated)
		*outTruncated = 0;
	if (!out || cap == 0)
	{
		if (outTruncated)
			*outTruncated = count;
		return 0;
	}

	size_t n = 0;
	AppendStr(out, cap, &n, "Module bases:");

	int printed = 0;
	for (int i = 0; i < count; ++i)
	{
		// Reserve one byte for the final NUL: only write this entry if it
		// (and the NUL after it) fully fits, so no entry is ever emitted
		// with its tail silently clipped off.
		if (n + EntryLen(mods[i]) + 1 > cap)
		{
			if (outTruncated)
				*outTruncated = count - printed;
			out[n < cap ? n : cap - 1] = '\0';
			return n;
		}

		AppendChar(out, cap, &n, ' ');
		AppendStr(out, cap, &n, mods[i].name);
		AppendChar(out, cap, &n, '=');
		AppendStr(out, cap, &n, "0x");
		AppendHex(out, cap, &n, mods[i].base);
		AppendChar(out, cap, &n, '+');
		AppendStr(out, cap, &n, "0x");
		AppendHex(out, cap, &n, mods[i].size);
		++printed;
	}

	if (outTruncated)
		*outTruncated = count - printed;
	out[n < cap ? n : cap - 1] = '\0';
	return n;
}

int SelectModuleBases(const ModuleBaseEntry* all, int allCount,
                       const unsigned __int64* priorityBases, int priorityCount,
                       ModuleBaseEntry* out, int cap)
{
	if (!out || cap <= 0)
		return 0;

	int n = 0;

	// Priority entries claim their slots first, in priority order, so they
	// are never the ones dropped when allCount > cap.
	for (int p = 0; p < priorityCount && n < cap; ++p)
	{
		if (!priorityBases[p])
			continue;
		for (int i = 0; i < allCount; ++i)
		{
			if (all[i].base == priorityBases[p])
			{
				out[n++] = all[i];
				break;
			}
		}
	}

	// Everything else fills whatever room is left, in enumeration order,
	// skipping bases already copied above.
	for (int i = 0; i < allCount && n < cap; ++i)
	{
		bool already = false;
		for (int j = 0; j < n; ++j)
		{
			if (out[j].base == all[i].base)
			{
				already = true;
				break;
			}
		}
		if (!already)
			out[n++] = all[i];
	}

	return n;
}

int ResolveModuleForAddress(const ModuleBaseEntry* mods, int count,
                             unsigned __int64 addr, unsigned __int64* outOffset)
{
	for (int i = 0; i < count; ++i)
	{
		if (mods[i].size == 0)
			continue;
		if (addr >= mods[i].base && addr < mods[i].base + mods[i].size)
		{
			if (outOffset)
				*outOffset = addr - mods[i].base;
			return i;
		}
	}
	return -1;
}

size_t FormatAddrMod(char* out, size_t cap, const ModuleBaseEntry* mods, int count,
                      bool haveList, unsigned __int64 addr, unsigned __int64 imageBase)
{
	if (!out || cap == 0)
		return 0;

	size_t n = 0;
	unsigned __int64 off = 0;
	int idx = haveList ? ResolveModuleForAddress(mods, count, addr, &off) : -1;
	if (idx >= 0)
	{
		AppendStr(out, cap, &n, mods[idx].name);
		AppendChar(out, cap, &n, '+');
		AppendStr(out, cap, &n, "0x");
		AppendHex(out, cap, &n, off);
	}
	else if (imageBase != 0)
	{
		AppendStr(out, cap, &n, "image@0x");
		AppendHex(out, cap, &n, imageBase);
		AppendChar(out, cap, &n, '+');
		AppendStr(out, cap, &n, "0x");
		AppendHex(out, cap, &n, addr - imageBase);
	}
	else if (haveList)
	{
		AppendStr(out, cap, &n, "none");
	}
	else
	{
		AppendChar(out, cap, &n, '?');
	}
	out[n < cap ? n : cap - 1] = '\0';
	return n;
}
