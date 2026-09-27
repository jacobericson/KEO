#include "diag/fatal_class.h"

namespace fatal_class_detail {

const unsigned long CODE_ACCESS_VIOLATION   = 0xC0000005UL;
const unsigned long CODE_IN_PAGE_ERROR      = 0xC0000006UL;
const unsigned long CODE_DIVIDE_BY_ZERO     = 0xC0000094UL;
const unsigned long CODE_STACK_OVERFLOW     = 0xC00000FDUL;
const unsigned long CODE_CPP_EXCEPTION      = 0xE06D7363UL;
const unsigned long CODE_FAIL_FAST          = 0xC0000409UL;  // security check / abort
const unsigned long CODE_HEAP_CORRUPTION    = 0xC0000374UL;
const unsigned long CODE_INVALID_PARAMETER  = 0xC0000417UL;  // CRT _invoke_watson exit code
const unsigned long CODE_APP_EXIT           = 0x40000015UL;  // CRT abort message
const unsigned long CODE_BREAKPOINT         = 0x80000003UL;
const unsigned long CODE_SINGLE_STEP        = 0x80000004UL;

// The descriptor prefixes worth stripping. Longest first, so ".PEAV" is not
// mistaken for a ".?AV" that failed to match.
struct Prefix { const char* text; size_t len; };
const Prefix PREFIXES[] = {
	{ ".PEAV", 5 }, { ".PEAU", 5 },
	{ ".?AV",  4 }, { ".?AU",  4 }, { ".?AW", 4 }
};
const int PREFIX_COUNT = 5;
const int MAX_PARTS = 8;

bool StartsWith(const char* s, const char* prefix, size_t len)
{
	for (size_t i = 0; i < len; ++i)
	{
		if (s[i] == '\0' || s[i] != prefix[i])
			return false;
	}
	return true;
}

size_t CopyVerbatim(const char* raw, char* out, size_t cap)
{
	size_t n = 0;
	if (cap == 0)
		return 0;
	while (raw && *raw && n + 1 < cap)
		out[n++] = *raw++;
	out[n] = '\0';
	return n;
}

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

void AppendHex16(char* out, size_t cap, size_t* n, unsigned __int64 v)
{
	static const char* kHex = "0123456789ABCDEF";
	for (int i = 15; i >= 0; --i)
		AppendChar(out, cap, n, kHex[(size_t)((v >> (i * 4)) & 0xF)]);
}

void AppendDec(char* out, size_t cap, size_t* n, unsigned __int64 v)
{
	char tmp[24];
	int t = 0;
	if (v == 0)
		tmp[t++] = '0';
	while (v > 0 && t < 24)
	{
		tmp[t++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (t > 0)
		AppendChar(out, cap, n, tmp[--t]);
}

} // namespace
using namespace fatal_class_detail;

size_t FatalFormatAccess(unsigned long code, unsigned long numberParameters,
                         unsigned __int64 info0, unsigned __int64 info1,
                         char* out, size_t cap)
{
	if (!out || cap == 0)
		return 0;

	const bool carries = (code == CODE_ACCESS_VIOLATION || code == CODE_IN_PAGE_ERROR)
	                     && numberParameters >= 2;

	size_t n = 0;
	AppendStr(out, cap, &n, "access=");
	if (carries)
	{
		AppendStr(out, cap, &n, "0x");
		AppendHex16(out, cap, &n, info1);
	}
	else
		AppendChar(out, cap, &n, '-');

	AppendStr(out, cap, &n, " rw=");
	if (carries)
	{
		AppendDec(out, cap, &n, info0);
		if (info0 == 0)      AppendStr(out, cap, &n, "/read");
		else if (info0 == 1) AppendStr(out, cap, &n, "/write");
		else if (info0 == 8) AppendStr(out, cap, &n, "/exec");
	}
	else
		AppendChar(out, cap, &n, '-');

	out[n] = '\0';
	return n;
}

const char* FatalKindToken(int kind)
{
	switch (kind)
	{
	case FATAL_KIND_AV:       return "AV";
	case FATAL_KIND_DIV0:     return "DIV0";
	case FATAL_KIND_STACK:    return "STACK";
	case FATAL_KIND_CPPEX:    return "CPPEX";
	case FATAL_KIND_FASTFAIL: return "ABORT";
	case FATAL_KIND_BREAK:    return "BREAK";
	default:                  return "OTHER";
	}
}

int FatalKindForCode(unsigned long code)
{
	switch (code)
	{
	case CODE_ACCESS_VIOLATION: return FATAL_KIND_AV;
	case CODE_DIVIDE_BY_ZERO:   return FATAL_KIND_DIV0;
	case CODE_STACK_OVERFLOW:   return FATAL_KIND_STACK;
	case CODE_CPP_EXCEPTION:    return FATAL_KIND_CPPEX;
	case CODE_FAIL_FAST:
	case CODE_HEAP_CORRUPTION:
	case CODE_INVALID_PARAMETER:
	case CODE_APP_EXIT:         return FATAL_KIND_FASTFAIL;
	case CODE_BREAKPOINT:
	case CODE_SINGLE_STEP:      return FATAL_KIND_BREAK;
	default:                    return FATAL_KIND_OTHER;
	}
}

const long   CPP_RECORD_BURST = 4;
const long   CPP_RECORD_CAP = 64;
const double CPP_RECORD_SPACING_SEC = 2.0;

bool CppRecordAdmit(long recordsWritten, double nowSec, double lastWriteSec)
{
	if (recordsWritten >= CPP_RECORD_CAP)
		return false;
	if (recordsWritten < CPP_RECORD_BURST)
		return true;
	if (lastWriteSec < 0.0)
		return true;
	return nowSec - lastWriteSec >= CPP_RECORD_SPACING_SEC;
}

size_t FatalDemangleTypeName(const char* raw, char* out, size_t cap)
{
	if (cap == 0)
		return 0;
	out[0] = '\0';
	if (!raw || !*raw)
		return 0;

	const char* p = 0;
	for (int i = 0; i < PREFIX_COUNT && !p; ++i)
	{
		if (StartsWith(raw, PREFIXES[i].text, PREFIXES[i].len))
			p = raw + PREFIXES[i].len;
	}
	if (!p)
		return CopyVerbatim(raw, out, cap);

	// Mangled scopes run innermost-first and end at the empty component of
	// the trailing "@@"; printing them outermost-first is what a reader of
	// the log expects.
	const char* starts[MAX_PARTS];
	size_t      lens[MAX_PARTS];
	int         count = 0;
	while (*p && count < MAX_PARTS)
	{
		const char* begin = p;
		while (*p && *p != '@')
			++p;
		size_t len = (size_t)(p - begin);
		if (len == 0)
			break;                 // the "@@" terminator
		starts[count] = begin;
		lens[count] = len;
		++count;
		if (*p == '@')
			++p;
	}
	if (count == 0)
		return CopyVerbatim(raw, out, cap);

	size_t n = 0;
	for (int i = count - 1; i >= 0; --i)
	{
		if (n > 0)
		{
			if (n + 2 < cap) { out[n++] = ':'; out[n++] = ':'; }
			else break;
		}
		for (size_t k = 0; k < lens[i] && n + 1 < cap; ++k)
			out[n++] = starts[i][k];
	}
	out[n] = '\0';
	return n;
}
