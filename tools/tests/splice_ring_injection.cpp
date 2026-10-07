// Races the wall splice's record ring: three producer threads push 200,000 boxes each while one
// consumer drains at most 8 records per call and yields between calls, so the producers lap it and
// each other. Every drained record must be whole (its six values derived from its first two), and
// once every producer has returned and the drain has reached the cursor, drained + lost must equal
// written, with at least one record overwritten by a later lap. A second phase runs the same
// producers against a consumer with no cap and no yield and holds it to the same whole
// record and accounting checks. A third phase runs the producers through the refusing push against
// the starved consumer: every index taken is a record or a failed claim, and at least one push must
// be refused for a full window. The threads yield with SwitchToThread only.
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
	const int  WIDE      = 64;

	bool g_keepPace = false;
	bool g_tryPush = false;
	volatile LONG g_ok = 0;
	volatile LONG g_refused = 0;

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
		float out[WIDE][6];
		const int got = SpliceRingDrain(&g_ring, out, g_keepPace ? WIDE : BATCH);
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
			if (!g_tryPush)
				SpliceRingPush(&g_ring, box);
			else
			{
				switch (SpliceRingTryPush(&g_ring, box))
				{
				case SPLICE_PUSH_TAKEN: InterlockedIncrement(&g_ok); break;
				case SPLICE_PUSH_FULL:  InterlockedIncrement(&g_refused); break;
				default:                break;   // counted in the ring's claimFailed
				}
			}
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
			if (!g_keepPace)
				SwitchToThread();
		}
		return 0;
	}
}
using namespace splice_ring_injection_detail;

// One phase: fresh ring and counters, the producers against the consumer, then the final drain and
// the checks. Returns 0 when the phase held.
static int RunPhase(const char* name, bool keepPace, bool requireLap, bool tryPush)
{
	g_keepPace = keepPace;
	g_tryPush = tryPush;
	g_ok = 0;
	g_refused = 0;
	g_producersDone = 0;
	g_drained = 0;
	g_torn = 0;
	g_tornThread = g_tornN = -1;
	SpliceRingInit(&g_ring);

	HANDLE threads[PRODUCERS + 1];
	threads[0] = CreateThread(NULL, 0, Consumer, NULL, 0, NULL);
	for (int t = 0; t < PRODUCERS; ++t)
		threads[t + 1] = CreateThread(NULL, 0, Producer, (void*)(INT_PTR)t, 0, NULL);
	for (int t = 0; t <= PRODUCERS; ++t)
	{
		if (!threads[t])
		{
			printf("splice ring %s: thread creation failed\n", name);
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
		printf("splice ring %s: torn record thread=%d n=%d\n", name, g_tornThread, g_tornN);
		return 1;
	}
	if (g_ring.read != written || g_drained + lost != written)
	{
		printf("splice ring %s: accounting drained=%ld lost=%ld written=%ld\n", name, g_drained, lost, written);
		return 1;
	}
	if (tryPush)
	{
		const LONG ok = g_ok, refused = g_refused, claimFailed = g_ring.claimFailed;
		if (written != ok + claimFailed)
		{
			printf("splice ring %s: accounting written=%ld ok=%ld claimFailed=%ld\n", name, written, ok, claimFailed);
			return 1;
		}
		if (refused == 0)
		{
			printf("splice ring %s: never refused (refused=0)\n", name);
			return 1;
		}
		printf("splice ring %s: written=%ld ok=%ld refused=%ld claimFailed=%ld drained=%ld lost=%ld\n",
		       name, written, ok, refused, claimFailed, g_drained, lost);
		return 0;
	}
	if (requireLap && overwritten == 0)
	{
		printf("splice ring %s: no lap (overwritten=0); the harness did not exercise lapping\n", name);
		return 1;
	}
	printf("splice ring %s: pushed=%ld drained=%ld lost=%ld overwritten=%ld claimFailed=%ld\n",
	       name, written, g_drained, lost, overwritten, g_ring.claimFailed);
	return 0;
}

int main()
{
	if (RunPhase("starved", false, true, false) != 0)
		return 1;
	if (RunPhase("keepPace", true, false, false) != 0)
		return 1;
	if (RunPhase("tryPush", false, false, true) != 0)
		return 1;
	printf("splice ring: torn=0 lapped=yes keepPace=torn0 tryPush=refused\n");
	return 0;
}
