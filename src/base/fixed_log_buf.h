#ifndef KENSHI_ZONE_OPT_BASE_FIXED_LOG_BUF_H
#define KENSHI_ZONE_OPT_BASE_FIXED_LOG_BUF_H

#include <stddef.h>

// A fixed-size line builder for code where a CRT stream and an allocation are both
// unwanted: detours on the navmesh, content-stream and search threads, and the crash and
// record writers. Header-only; nothing here touches game state. A character is appended while
// one byte stays free, so a line cut at the capacity keeps its first cap - 1 bytes.

template <size_t N>
struct FixedLogBufN
{
	char   b[N];
	size_t n;
};

typedef FixedLogBufN<352> FixedLogBuf;

// Caller-owned storage: b points at cap bytes.
struct FlbExternal
{
	char*  b;
	size_t cap;
	size_t n;
};

template <size_t N>
inline size_t FlbCap(const FixedLogBufN<N>*) { return N; }

inline size_t FlbCap(const FlbExternal* o) { return o->cap; }

template <class B>
inline void FlbInit(B* o) { o->n = 0; }

template <class B>
inline void FlbChar(B* o, char c)
{
	if (o->n + 1 < FlbCap(o))
		o->b[o->n++] = c;
}

template <class B>
inline void FlbStr(B* o, const char* s)
{
	while (s && *s)
		FlbChar(o, *s++);
}

template <class B>
inline void FlbDec(B* o, __int64 v)
{
	if (v < 0) { FlbChar(o, '-'); v = -v; }
	char t[24];
	int n = 0;
	do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 24);
	while (n)
		FlbChar(o, t[--n]);
}

template <class B>
inline void FlbDecU(B* o, unsigned __int64 v)
{
	char tmp[24];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < 24)
	{
		tmp[n++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (n > 0)
		FlbChar(o, tmp[--n]);
}

// "0x" and the fewest uppercase digits ("0x0" for zero).
template <class B>
inline void FlbHex(B* o, unsigned __int64 v)
{
	FlbStr(o, "0x");
	char t[20];
	int n = 0;
	do { int d = (int)(v & 15); t[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; }
	while (v && n < 20);
	while (n)
		FlbChar(o, t[--n]);
}

// Exactly digits uppercase digits, clamped to 1..16, with no prefix.
template <class B>
inline void FlbHexDigits(B* o, unsigned __int64 v, int digits)
{
	if (digits < 1)  digits = 1;
	if (digits > 16) digits = 16;
	for (int i = digits - 1; i >= 0; --i)
		FlbChar(o, "0123456789ABCDEF"[(size_t)((v >> (i * 4)) & 0xF)]);
}

// Terminates the line and hands back the C string; the caller logs it. The
// terminator goes through FlbChar's rule too, so a line that already holds
// cap - 1 bytes is left unterminated: its last byte keeps whatever it held.
template <class B>
inline const char* FlbDone(B* o) { FlbChar(o, '\0'); return o->b; }

#endif // KENSHI_ZONE_OPT_BASE_FIXED_LOG_BUF_H
