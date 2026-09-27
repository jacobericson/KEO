#ifndef KENSHI_ZONE_OPT_BASE_CLOCK_H
#define KENSHI_ZONE_OPT_BASE_CLOCK_H

// QueryPerformanceCounter time. startPlugin sets qpcFrequency and
// pluginStartTime before any hook is installed and nothing changes them after,
// so every function here is lock-free and safe on any thread. A duration is a
// later read minus an earlier one, so it is never negative, except where a
// caller subtracts two unordered stamps (QpcToMs keeps the sign).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

extern LARGE_INTEGER qpcFrequency;
extern LARGE_INTEGER pluginStartTime;

inline LONGLONG QpcNow()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

// Microseconds in a tick count; 0 for a count at or below zero.
inline LONGLONG QpcToUs(LONGLONG ticks)
{
	if (qpcFrequency.QuadPart <= 0 || ticks <= 0)
		return 0;
	return (ticks * 1000000LL) / qpcFrequency.QuadPart;
}

// Milliseconds in a tick count, sign kept.
inline double QpcToMs(LONGLONG ticks)
{
	if (qpcFrequency.QuadPart <= 0)
		return 0.0;
	return (double)ticks * 1000.0 / (double)qpcFrequency.QuadPart;
}

// Microseconds between two reads, clamped to what a 32-bit slot holds
// (about 35 minutes).
inline long QpcDeltaUs(LONGLONG from, LONGLONG to)
{
	if (to <= from || qpcFrequency.QuadPart <= 0)
		return 0;
	LONGLONG us = (to - from) * 1000000 / qpcFrequency.QuadPart;
	return us > 0x7FFFFFFF ? 0x7FFFFFFF : (long)us;
}

inline LONGLONG QpcFromUs(LONGLONG us)
{
	return (qpcFrequency.QuadPart > 0) ? us * qpcFrequency.QuadPart / 1000000 : 0;
}

inline double ElapsedSec()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - pluginStartTime.QuadPart) / (double)qpcFrequency.QuadPart;
}

inline double QPCToMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end)
{
	return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)qpcFrequency.QuadPart;
}

#endif
