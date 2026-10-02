#ifndef KEO_FIXES_GUARD_REPORT_H
#define KEO_FIXES_GUARD_REPORT_H

// The guards' shared reporting: a heartbeat line built from a table of named counters, the
// heartbeat timer and the fire-line cap. Any thread; no lock, no CRT stream, no allocation.
// Every guard keeps its own lead and token names, so its lines read as they did.

#include "base/fixed_log_buf.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <limits.h>

enum GuardFieldKind
{
	GF_COUNT,     // " name=<value>"
	GF_COUNT_OF   // " name=<value>/<of>"
};

// One field of a heartbeat line. A table of these is POD with address constants, so a
// file-scope table is statically initialised.
struct GuardCounter
{
	const char*    name;
	GuardFieldKind kind;
	volatile LONG* value;
	LONG           of;
};

template <class B>
inline void GuardHeartbeatBegin(B* o, const char* lead)
{
	FlbInit(o);
	FlbStr(o, lead);
}

// A field whose guard is not live prints '?' for its value; the "/<of>" suffix is printed
// either way.
template <class B>
inline void GuardFields(B* o, const GuardCounter* rows, int n, bool live)
{
	for (int i = 0; i < n; ++i)
	{
		FlbChar(o, ' ');
		FlbStr(o, rows[i].name);
		FlbChar(o, '=');
		if (live)
			FlbDec(o, InterlockedCompareExchange(rows[i].value, 0, 0));
		else
			FlbChar(o, '?');
		if (rows[i].kind == GF_COUNT_OF)
		{
			FlbChar(o, '/');
			FlbDec(o, rows[i].of);
		}
	}
}

// True for exactly one caller once *nextBeat is reached, and moves it seconds ahead. With no
// usable frequency the next beat is never: the guard beats once and stops.
inline bool GuardBeatDue(volatile LONGLONG* nextBeat, LONGLONG qpf, int seconds)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	LONGLONG due = *nextBeat;
	if (now.QuadPart < due)
		return false;
	LONGLONG next = qpf > 0 ? now.QuadPart + qpf * seconds : _I64_MAX;
	return InterlockedCompareExchange64(nextBeat, next, due) == due;
}

// The clock is read on the first call and then once every 1024, for sites that run per node
// or per face.
inline bool GuardBeatSample(LONG calls)
{
	return calls == 1 || (calls & 0x3FF) == 0;
}

// Saturating: once the cap is reached the counter stops moving, so the number in the
// heartbeat stays the number of lines actually written.
inline bool GuardFireClaim(volatile LONG* lines, LONG max)
{
	for (;;)
	{
		LONG cur = InterlockedCompareExchange(lines, 0, 0);
		if (cur >= max)
			return false;
		if (InterlockedCompareExchange(lines, cur + 1, cur) == cur)
			return true;
	}
}

#endif // KEO_FIXES_GUARD_REPORT_H
