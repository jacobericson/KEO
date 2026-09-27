#include "navmesh/scheduling/nm_inflight.h"
#include <string.h>

static const int INFLIGHT_SLOTS = 8;
struct Slot { bool active; InflightKey key; LONG gen; HANDLE done; };
static Slot             s_slots[INFLIGHT_SLOTS];
static CRITICAL_SECTION s_cs;          // leaf: nothing is taken while holding it
static volatile LONG    s_init  = 0;
static volatile LONG    s_ready = 0;   // set once every slot has its event

void InflightInit()
{
	if (InterlockedCompareExchange(&s_init, 1, 0) != 0)
		return;
	InitializeCriticalSection(&s_cs);
	bool ok = true;
	for (int i = 0; i < INFLIGHT_SLOTS; ++i)
	{
		s_slots[i].active = false;
		s_slots[i].gen = 0;
		s_slots[i].done = CreateEvent(NULL, TRUE, TRUE, NULL);   // manual reset
		if (!s_slots[i].done)
			ok = false;
	}
	if (ok)
		InterlockedExchange(&s_ready, 1);
}

// WAITED means the owner finished (its slot was released or reused); the
// caller registers again. Without an initialised table every call is FULL,
// which callers treat as "proceed unregistered".
InflightResult InflightRegisterOrWait(const InflightKey& k, DWORD timeoutMs, bool (*stop)(), int* slotOut)
{
	*slotOut = -1;
	if (!InterlockedCompareExchange(&s_ready, 0, 0))
		return INFLIGHT_FULL;

	EnterCriticalSection(&s_cs);
	int freeIdx = -1, owned = -1;
	for (int i = 0; i < INFLIGHT_SLOTS; ++i)
	{
		if (s_slots[i].active && memcmp(&s_slots[i].key, &k, sizeof(k)) == 0) { owned = i; break; }
		if (!s_slots[i].active && freeIdx < 0) freeIdx = i;
	}
	if (owned < 0)
	{
		if (freeIdx < 0) { LeaveCriticalSection(&s_cs); return INFLIGHT_FULL; }
		s_slots[freeIdx].active = true;
		s_slots[freeIdx].key = k;
		s_slots[freeIdx].gen++;
		ResetEvent(s_slots[freeIdx].done);
		LeaveCriticalSection(&s_cs);
		*slotOut = freeIdx;
		return INFLIGHT_OWNER;
	}
	LONG gen = s_slots[owned].gen;
	HANDLE ev = s_slots[owned].done;
	LeaveCriticalSection(&s_cs);

	// The event is reset when the slot is reused, so a wait can miss the
	// release it was for; the generation check below catches that case.
	DWORD waited = 0;
	for (;;)
	{
		if (stop && stop()) return INFLIGHT_STOPPED;
		if (WaitForSingleObject(ev, 50) == WAIT_OBJECT_0) return INFLIGHT_WAITED;
		EnterCriticalSection(&s_cs);
		bool moved = !s_slots[owned].active || s_slots[owned].gen != gen;
		LeaveCriticalSection(&s_cs);
		if (moved) return INFLIGHT_WAITED;
		waited += 50;
		if (waited >= timeoutMs) return INFLIGHT_TIMEOUT;
	}
}

void InflightRelease(int slot)
{
	if (slot < 0 || slot >= INFLIGHT_SLOTS || !InterlockedCompareExchange(&s_ready, 0, 0)) return;
	EnterCriticalSection(&s_cs);
	s_slots[slot].active = false;
	SetEvent(s_slots[slot].done);
	LeaveCriticalSection(&s_cs);
}
