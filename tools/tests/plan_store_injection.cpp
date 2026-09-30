// Races the plan store's lock-free read against its rewrite: a writer thread rewrites slot 0 for
// one character 200,000 times, stamping its sequence number into the plan's destination and into
// every leg's point, while a reader thread reads the slot in a loop. Every view the read accepts
// must carry one stamp throughout; a mixed view is a torn read the sequence word failed to refuse.
// The threads yield with SwitchToThread only, so the interleaving is the scheduler's.
//
// Links src/planner/plan_store.cpp and plan_policy.cpp unmodified.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "planner/plan_store.h"

using namespace planner;

namespace plan_store_injection_detail
{
	const uintptr_t CM     = 0x10000;
	const LONG      WRITES = 200000;

	volatile LONG g_done = 0;
	volatile LONG g_writes = 0;
	LONG g_reads = 0, g_torn = 0, g_refused = 0;
	float g_tornFinal = 0.0f, g_tornLeg = 0.0f;
	int g_tornIndex = -1;

	void Stamp(PlanWrite* w, LONG seq)
	{
		memset(w, 0, sizeof(*w));
		w->cm = CM;
		w->verdict = PV_LEGGED;
		w->legCount = PLAN_MAX_LEGS;
		w->firstLeg = 0;
		w->finalDest[0] = (float)seq;
		for (int i = 0; i < PLAN_MAX_LEGS; ++i)
		{
			w->legs[i].point[0] = (float)seq;
			w->legs[i].isDestination = (i == PLAN_MAX_LEGS - 1) ? 1 : 0;
		}
	}

	DWORD WINAPI Writer(void*)
	{
		PlanWrite w;
		for (LONG seq = 1; seq <= WRITES; ++seq)
		{
			Stamp(&w, seq);
			if (PlanStoreWrite(w) != 0) break;
			InterlockedIncrement(&g_writes);
			if ((seq & 1023) == 0) SwitchToThread();
		}
		InterlockedExchange(&g_done, 1);
		return 0;
	}

	// One read; false when the view it accepted mixes two stamps.
	bool ReadOnce(PlanView* v)
	{
		if (!PlanStoreRead(0, v))
		{
			g_refused++;
			return true;
		}
		g_reads++;
		for (int i = 0; i < PLAN_MAX_LEGS; ++i)
		{
			if (v->legs[i].point[0] != v->finalDest[0])
			{
				g_torn++;
				g_tornFinal = v->finalDest[0];
				g_tornLeg = v->legs[i].point[0];
				g_tornIndex = i;
				return false;
			}
		}
		return true;
	}

	DWORD WINAPI Reader(void*)
	{
		PlanView v;
		while (!g_done)
			ReadOnce(&v);
		ReadOnce(&v);
		return 0;
	}
}
using namespace plan_store_injection_detail;

int main()
{
	PlanStoreReset();
	PlanStoreArm(PLANNER_ON);
	PlanWrite w;
	Stamp(&w, 0);
	if (PlanStoreWrite(w) != 0)
	{
		std::printf("plan race: setup failed (slot 0 not written)\n");
		return 1;
	}

	HANDLE reader = CreateThread(NULL, 0, Reader, NULL, 0, NULL);
	HANDLE writer = CreateThread(NULL, 0, Writer, NULL, 0, NULL);
	if (!reader || !writer)
	{
		std::printf("plan race: setup failed (CreateThread)\n");
		return 1;
	}
	WaitForSingleObject(writer, INFINITE);
	WaitForSingleObject(reader, INFINITE);
	CloseHandle(writer);
	CloseHandle(reader);

	if (g_torn != 0)
	{
		std::printf("plan race: TORN %ld view(s); first: finalDest=%.0f leg[%d]=%.0f (%ld reads, %ld writes, %ld refused)\n",
		            g_torn, g_tornFinal, g_tornIndex, g_tornLeg, g_reads, g_writes, g_refused);
		return 1;
	}
	if (g_writes != WRITES || g_reads == 0)
	{
		std::printf("plan race: incomplete run (%ld reads, %ld writes of %ld, %ld refused)\n",
		            g_reads, g_writes, WRITES, g_refused);
		return 1;
	}
	std::printf("plan race: %ld reads, 0 torn, %ld writes, %ld refused\n", g_reads, g_writes, g_refused);
	return 0;
}
