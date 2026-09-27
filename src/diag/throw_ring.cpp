#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "diag/throw_ring.h"

namespace throw_ring_detail {

char          g_slot[THROW_RING_SLOTS][THROW_RING_CHARS] = { { 0 } };
volatile LONG g_len[THROW_RING_SLOTS] = { 0 };
volatile LONG g_pushed = 0;
volatile LONG g_drained = 0;
volatile LONG g_lost = 0;

void Append(char* out, size_t cap, size_t& n, const char* s)
{
	while (s && *s && n + 1 < cap)
		out[n++] = *s++;
}

void AppendDec(char* out, size_t cap, size_t& n, unsigned __int64 v)
{
	char tmp[24];
	int d = 0;
	if (v == 0)
		tmp[d++] = '0';
	while (v > 0 && d < 24)
	{
		tmp[d++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (d > 0 && n + 1 < cap)
		out[n++] = tmp[--d];
}

void AppendHex(char* out, size_t cap, size_t& n, unsigned __int64 v, int digits)
{
	static const char HEX[] = "0123456789ABCDEF";
	for (int i = digits - 1; i >= 0 && n + 1 < cap; --i)
		out[n++] = HEX[(size_t)((v >> (i * 4)) & 0xF)];
}

} // namespace
using namespace throw_ring_detail;

size_t ThrowRecordFormat(char* out, size_t cap, const ThrowRecordFields& f)
{
	if (!out || cap == 0)
		return 0;
	size_t n = 0;
	Append(out, cap, n, "THROW #");
	AppendDec(out, cap, n, (unsigned __int64)(unsigned long)f.seq);
	Append(out, cap, n, ": kind=");
	Append(out, cap, n, f.kind && f.kind[0] ? f.kind : "?");
	Append(out, cap, n, " code=0x");
	AppendHex(out, cap, n, (unsigned __int64)f.code, 8);
	Append(out, cap, n, " addr=0x");
	AppendHex(out, cap, n, f.addr, 16);
	Append(out, cap, n, " rva=0x");
	AppendHex(out, cap, n, f.rva, 8);
	Append(out, cap, n, " t=");
	AppendDec(out, cap, n, (unsigned __int64)(unsigned long)(f.atSec > 0 ? f.atSec : 0));
	Append(out, cap, n, "s tid=");
	AppendDec(out, cap, n, (unsigned __int64)f.tid);
	// Its own word for the teardown state: afterStop= is reserved for a real
	// fault, and a caught throw during shutdown is not one.
	if (f.afterStop)
		Append(out, cap, n, " phase=afterStop");
	Append(out, cap, n, " type=");
	Append(out, cap, n, f.type && f.type[0] ? f.type : "?");
	Append(out, cap, n, "\r\n");
	out[n] = '\0';
	return n;
}

void ThrowRingPush(const char* text)
{
	if (!text || !text[0])
		return;

	LONG n = InterlockedIncrement(&g_pushed);
	size_t idx = (size_t)((n - 1) % THROW_RING_SLOTS);

	// What this push is about to overwrite, if nothing has drained it.
	if (n - InterlockedCompareExchange(&g_drained, 0, 0) > THROW_RING_SLOTS)
		InterlockedIncrement(&g_lost);

	char* dst = g_slot[idx];
	size_t i = 0;
	while (i + 1 < THROW_RING_CHARS && text[i])
	{
		dst[i] = text[i];
		++i;
	}
	dst[i] = '\0';
	InterlockedExchange(&g_len[idx], (LONG)i);
}

long ThrowRingPushed() { return InterlockedCompareExchange(&g_pushed, 0, 0); }
long ThrowRingLost()   { return InterlockedCompareExchange(&g_lost, 0, 0); }

long ThrowRingPending()
{
	LONG pushed  = InterlockedCompareExchange(&g_pushed, 0, 0);
	LONG drained = InterlockedCompareExchange(&g_drained, 0, 0);
	LONG pending = pushed - drained;
	if (pending < 0) pending = 0;
	if (pending > THROW_RING_SLOTS) pending = THROW_RING_SLOTS;
	return pending;
}

long ThrowRingDrain(ThrowRingSink sink, void* ctx)
{
	if (!sink)
		return 0;

	LONG pushed  = InterlockedCompareExchange(&g_pushed, 0, 0);
	LONG drained = InterlockedCompareExchange(&g_drained, 0, 0);
	LONG first   = drained;
	if (pushed - first > THROW_RING_SLOTS)
		first = pushed - THROW_RING_SLOTS;

	long delivered = 0;
	for (LONG i = first; i < pushed; ++i)
	{
		size_t idx = (size_t)(i % THROW_RING_SLOTS);
		LONG len = InterlockedCompareExchange(&g_len[idx], 0, 0);
		if (len <= 0)
			continue;
		sink(ctx, g_slot[idx], (size_t)len);
		++delivered;
	}
	InterlockedExchange(&g_drained, pushed);
	return delivered;
}

void ThrowRingResetForTest()
{
	for (size_t i = 0; i < THROW_RING_SLOTS; ++i)
	{
		g_slot[i][0] = '\0';
		g_len[i] = 0;
	}
	g_pushed = 0;
	g_drained = 0;
	g_lost = 0;
}
