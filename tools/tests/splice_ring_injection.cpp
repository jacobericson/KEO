// Races the wall splice's record ring: three producer threads push 200,000 boxes each while one
// consumer drains at most 8 records per call and yields between calls, so the producers lap it and
// each other. Every drained record must be whole (its six values derived from its first two), and
// once every producer has returned and the drain has reached the cursor, drained + lost must equal
// written, with at least one record overwritten by a later lap. The threads yield with
// SwitchToThread only.
//
// Links src/navmesh/construction/splice_ring.cpp unmodified.

#include <windows.h>
#include <cstdio>
#include "navmesh/construction/splice_ring.h"

using namespace navmesh;

namespace splice_ring_injection_detail
{
	const int  PRODUCERS = 3;
	const LONG PUSHES    = 200000;
	const int  BATCH     = 8;

	SpliceRing    g_ring;
	volatile LONG g_producersDone = 0;
	LONG g_drained = 0;
	LONG g_torn = 0;
	int  g_tornThread = -1, g_tornN = -1;

	float Value(int n, int thread, int i)
	{
		return (float)((n * 31 + thread * 17 + i) % 8191);
	}

	void Check(const float box[6])
	{
		const int thread = (int)box[0];
		const int n = (int)box[1];
		bool whole = box[0] == (float)thread && box[1] == (float)n
		          && thread >= 0 && thread < PRODUCERS && n >= 0 && n < PUSHES;
		for (int i = 2; whole && i < 6; ++i)
			whole = box[i] == Value(n, thread, i);
		if (!whole)
		{
			if (g_torn == 0)
			{
				g_tornThread = thread;
				g_tornN = n;
			}
			++g_torn;
		}
	}

	void DrainOnce()
	{
		float out[BATCH][6];
		const int got = SpliceRingDrain(&g_ring, out, BATCH);
		for (int k = 0; k < got; ++k)
			Check(out[k]);
		g_drained += got;
	}

	DWORD WINAPI Producer(void* arg)
	{
		const int thread = (int)(INT_PTR)arg;
		for (LONG n = 0; n < PUSHES; ++n)
		{
			float box[6];
			box[0] = (float)thread;
			box[1] = (float)n;
			for (int i = 2; i < 6; ++i)
				box[i] = Value((int)n, thread, i);
			SpliceRingPush(&g_ring, box);
			if ((n & 1023) == 0) SwitchToThread();
		}
		InterlockedIncrement(&g_producersDone);
		return 0;
	}

	DWORD WINAPI Consumer(void*)
	{
		while (InterlockedCompareExchange(&g_producersDone, 0, 0) < PRODUCERS)
		{
			DrainOnce();
			SwitchToThread();
		}
		return 0;
	}
}
using namespace splice_ring_injection_detail;

int main()
{
	SpliceRingInit(&g_ring);

	HANDLE threads[PRODUCERS + 1];
	threads[0] = CreateThread(NULL, 0, Consumer, NULL, 0, NULL);
	for (int t = 0; t < PRODUCERS; ++t)
		threads[t + 1] = CreateThread(NULL, 0, Producer, (void*)(INT_PTR)t, 0, NULL);
	for (int t = 0; t <= PRODUCERS; ++t)
	{
		if (!threads[t])
		{
			printf("splice ring: thread creation failed\n");
			return 1;
		}
	}
	WaitForMultipleObjects(PRODUCERS + 1, threads, TRUE, INFINITE);
	for (int t = 0; t <= PRODUCERS; ++t)
		CloseHandle(threads[t]);

	// Every producer has returned: drain to the cursor, at most one call per index.
	const LONG written = g_ring.written;
	for (LONG calls = 0; g_ring.read < written && calls < written; ++calls)
		DrainOnce();

	const LONG lost = g_ring.lost;
	const LONG overwritten = g_ring.overwritten;
	if (g_torn != 0)
	{
		printf("splice ring: torn record thread=%d n=%d\n", g_tornThread, g_tornN);
		return 1;
	}
	if (g_ring.read != written || g_drained + lost != written)
	{
		printf("splice ring: accounting drained=%ld lost=%ld written=%ld\n", g_drained, lost, written);
		return 1;
	}
	if (overwritten == 0)
	{
		printf("splice ring: no lap (overwritten=0); the harness did not exercise lapping\n");
		return 1;
	}
	printf("splice ring: pushed=%ld drained=%ld lost=%ld overwritten=%ld claimFailed=%ld torn=0 lapped=yes\n",
	       written, g_drained, lost, overwritten, g_ring.claimFailed);
	return 0;
}
