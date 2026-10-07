// Races the backpack-first table's lock-free read against its writes: one writer thread rewrites
// slot 0 2,000,000 times, alternating it between (A, on) and (B, off) by an erase and a set that
// each take the lowest free slot, while two reader threads look both keys up in a loop. A read of
// A as off, or of B as on, pairs one write's key with another write's value: a torn read the slot
// sequence failed to refuse. A read that raced twice answers -1 and is counted, never torn.
// The writer starts only after both readers report ready; a handshake that runs out fails.
//
// Links src/inventory/backpack_table.cpp, backpack_policy.cpp and src/game/hand_key.cpp unmodified.

#include <windows.h>
#include <cstdio>
#include "inventory/backpack_table.h"

using namespace keo_inventory;

namespace backpack_table_injection_detail
{
	const LONG  WRITES       = 2000000;
	const DWORD HANDSHAKE_MS = 30000;

	const game::HandKey kKeyA = { 2, 7, 31, 405, 9001 };
	const game::HandKey kKeyB = { 2, 7, 31, 406, 9002 };

	HANDLE g_ready[2] = { NULL, NULL };
	volatile LONG g_done = 0;
	volatile LONG g_handshakeFailed = 0;
	volatile LONG g_writes = 0;
	volatile LONG g_reads = 0;
	volatile LONG g_torn = 0;

	DWORD WINAPI Writer(void*)
	{
		if (WaitForMultipleObjects(2, g_ready, TRUE, HANDSHAKE_MS) != WAIT_OBJECT_0)
		{
			InterlockedExchange(&g_handshakeFailed, 1);
			InterlockedExchange(&g_done, 1);
			return 0;
		}
		for (LONG i = 0; i < WRITES; ++i)
		{
			BackpackFirstErase(0);
			BackpackFirstSet(kKeyB, 0);
			BackpackFirstErase(0);
			BackpackFirstSet(kKeyA, 1);
			InterlockedIncrement(&g_writes);
			if ((i & 4095) == 0)
				SwitchToThread();
		}
		InterlockedExchange(&g_done, 1);
		return 0;
	}

	DWORD WINAPI Reader(void* arg)
	{
		const int id = (int)(INT_PTR)arg;
		LONG reads = 0, torn = 0;
		SetEvent(g_ready[id]);
		while (!g_done)
		{
			if (BackpackFirstGet(kKeyA) == 0)
				++torn;
			if (BackpackFirstGet(kKeyB) == 1)
				++torn;
			reads += 2;
		}
		InterlockedExchangeAdd(&g_reads, reads);
		InterlockedExchangeAdd(&g_torn, torn);
		return 0;
	}
}
using namespace backpack_table_injection_detail;

int main()
{
	if (!BackpackFirstSet(kKeyA, 1) || BackpackFirstGet(kKeyA) != 1 || BackpackFirstSlotCount() != 1)
	{
		std::printf("FAIL backpack table: setup (slot 0 not written)\n");
		return 1;
	}
	g_ready[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
	g_ready[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!g_ready[0] || !g_ready[1])
	{
		std::printf("FAIL backpack table: setup (CreateEvent)\n");
		return 1;
	}
	HANDLE threads[3];
	threads[0] = CreateThread(NULL, 0, Reader, (void*)(INT_PTR)0, 0, NULL);
	threads[1] = CreateThread(NULL, 0, Reader, (void*)(INT_PTR)1, 0, NULL);
	threads[2] = CreateThread(NULL, 0, Writer, NULL, 0, NULL);
	if (!threads[0] || !threads[1] || !threads[2])
	{
		std::printf("FAIL backpack table: setup (CreateThread)\n");
		return 1;
	}
	WaitForMultipleObjects(3, threads, TRUE, INFINITE);
	for (int i = 0; i < 3; ++i)
		CloseHandle(threads[i]);
	CloseHandle(g_ready[0]);
	CloseHandle(g_ready[1]);

	if (g_handshakeFailed)
	{
		std::printf("FAIL backpack table: handshake (readers not ready within %lu ms)\n", HANDSHAKE_MS);
		return 1;
	}
	const LONG t = g_torn, n = g_reads;
	if (t != 0 || n <= 0 || g_writes != WRITES)
	{
		std::printf("FAIL backpack table: torn reads (torn %ld, reads %ld, writes %ld of %ld, raced %ld)\n",
		            t, n, g_writes, WRITES, BackpackFirstRaceCount());
		return 1;
	}
	std::printf("backpack table: torn=%ld reads=%ld raced=%ld\n", t, n, BackpackFirstRaceCount());
	return 0;
}
