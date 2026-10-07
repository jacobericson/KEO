// Races the throw-out hold table's lock-free read against its writers: two writer threads add
// 200,000 entries each, every entry's key serial, hand bytes and expiry stamped from one value,
// while a reader thread reads every entry in a loop. Every entry the read accepts must carry one
// stamp in all three; a mixed entry is a torn read the sequence word failed to refuse.
// The threads start together on a flag and wait with bounded compare-exchange spins only.
//
// Links src/fixes/world/throwout_hold.cpp and throwout_policy.cpp unmodified.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "fixes/world/throwout_hold.h"

using fixes::ThrowoutHand;
using game::HandKey;

namespace throwout_hold_injection_detail
{
	const unsigned ADDS      = 200000;
	const LONG     MAX_SPINS = 100000000;

	volatile LONG g_go = 0;
	volatile LONG g_writersDone = 0;
	volatile LONG g_adds = 0;
	volatile LONG g_lost = 0;
	LONG g_reads = 0, g_torn = 0;
	double g_tornExpiry = 0.0;
	unsigned g_tornSerial = 0;
	unsigned __int64 g_tornHand = 0;

	// Bounded: a thread that never sees the flag gives up rather than hanging the run.
	bool WaitGo()
	{
		for (LONG n = 0; n < MAX_SPINS; ++n)
		{
			if (InterlockedCompareExchange(&g_go, 0, 0))
				return true;
			YieldProcessor();
		}
		return false;
	}

	DWORD WINAPI Writer(void* arg)
	{
		const unsigned w = (unsigned)(size_t)arg;
		if (WaitGo())
		{
			for (unsigned i = 0; i < ADDS; ++i)
			{
				const unsigned serial = i ^ 0x5A5A;
				HandKey k = { 1, w, 0, i, serial };
				ThrowoutHand h;
				std::memset(h.bytes, 0, sizeof h.bytes);
				const unsigned __int64 stamp = serial;
				std::memcpy(h.bytes, &stamp, sizeof stamp);
				if (fixes::ThrowoutHoldAdd(k, h, (double)serial + 0.25))
					InterlockedIncrement(&g_adds);
				else
					InterlockedIncrement(&g_lost);
			}
		}
		InterlockedIncrement(&g_writersDone);
		return 0;
	}

	void ReadPass()
	{
		for (int i = 0; i < fixes::THROWOUT_HOLD_SLOTS; ++i)
		{
			HandKey k;
			ThrowoutHand h;
			double e;
			if (!fixes::ThrowoutHoldRead(i, &k, &h, &e))
				continue;
			g_reads++;
			unsigned __int64 stamp;
			std::memcpy(&stamp, h.bytes, sizeof stamp);
			if (e != (double)k.serial + 0.25 || stamp != (unsigned __int64)k.serial)
			{
				if (g_torn == 0)
				{
					g_tornExpiry = e;
					g_tornSerial = k.serial;
					g_tornHand = stamp;
				}
				g_torn++;
			}
		}
	}

	DWORD WINAPI Reader(void*)
	{
		if (WaitGo())
		{
			for (LONG pass = 0; pass < MAX_SPINS && InterlockedCompareExchange(&g_writersDone, 0, 0) < 2; ++pass)
				ReadPass();
		}
		ReadPass();
		return 0;
	}
}
using namespace throwout_hold_injection_detail;

int main()
{
	fixes::ThrowoutHoldClear();
	HANDLE t[3];
	t[0] = CreateThread(NULL, 0, Reader, NULL, 0, NULL);
	t[1] = CreateThread(NULL, 0, Writer, (void*)(size_t)1, 0, NULL);
	t[2] = CreateThread(NULL, 0, Writer, (void*)(size_t)2, 0, NULL);
	if (!t[0] || !t[1] || !t[2])
	{
		std::printf("hold table: setup failed (CreateThread)\n");
		return 1;
	}
	InterlockedExchange(&g_go, 1);
	WaitForMultipleObjects(3, t, TRUE, INFINITE);
	CloseHandle(t[0]);
	CloseHandle(t[1]);
	CloseHandle(t[2]);

	std::printf("hold table: %ld reads, %ld adds, %ld lost\n", g_reads, g_adds, g_lost);
	if (g_torn != 0)
	{
		std::printf("hold table: %ld TORN (first: serial %u, expiry %.2f, hand %llu)\n",
		            g_torn, g_tornSerial, g_tornExpiry, g_tornHand);
		return 1;
	}
	if (g_adds + g_lost != (LONG)(2 * ADDS) || g_reads == 0)
	{
		std::printf("hold table: incomplete run (%ld reads, %ld adds, %ld lost of %u)\n",
		            g_reads, g_adds, g_lost, 2 * ADDS);
		return 1;
	}
	std::printf("hold table: 0 torn\n");
	return 0;
}
