#ifndef KENSHI_ZONE_OPT_NM_INFLIGHT_H
#define KENSHI_ZONE_OPT_NM_INFLIGHT_H

#include <windows.h>

// The navmesh keys being generated right now, so a second job for the same
// key waits for the first instead of generating it again. Eight slots behind
// one leaf lock; nothing else is ever taken while that lock is held.

struct InflightKey { unsigned v[6]; };

enum InflightResult
{
	INFLIGHT_OWNER,     // registered: *slotOut owns the key until InflightRelease
	INFLIGHT_WAITED,    // another thread owned it and has released it; call again
	INFLIGHT_FULL,      // every slot holds another key: proceed unregistered
	INFLIGHT_STOPPED,   // stop() read true while waiting
	INFLIGHT_TIMEOUT    // the owner held it for timeoutMs
};

// Idempotent; call once before any other thread uses the table.
void InflightInit();
// Registers k, or waits (in 50 ms slices, checking stop) for the thread that
// owns it. *slotOut is -1 on every result but OWNER. Never call it while
// holding a lock another generation needs: the owner may be waiting for it.
InflightResult InflightRegisterOrWait(const InflightKey& k, DWORD timeoutMs, bool (*stop)(), int* slotOut);
// Releases an OWNER slot and wakes its waiters. A negative slot is a no-op.
void InflightRelease(int slot);

#endif
