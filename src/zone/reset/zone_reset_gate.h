#ifndef KENSHI_ZONE_OPT_ZONE_RESET_GATE_H
#define KENSHI_ZONE_OPT_ZONE_RESET_GATE_H

#include <windows.h>
#include "zone/reset/zone_reset_fence.h"

// The save-load reset's admission gate: up from the reset hook's first
// statement to its exit. While it is up no navmesh claim is taken, and a job
// claimed before it rose waits before its HIT rebuild and before its collision
// build. Raised and lowered on the main thread, read on the NavMesh threads,
// which reach the admission policy only through this gate. Host-linkable: no
// game or KenshiLib header, no heap allocation and no lock of its own.

struct ZoneResetGate
{
	volatile LONG inProgress;                        // 1 while the reset runs
	volatile LONG deferredTotal;                     // refusals since the last raise
	volatile LONG deferred[ZONE_RESET_SITE_COUNT];   // the same, by site
	HANDLE        over;                              // manual reset; set while inProgress is 0
	HANDLE        parked;                            // optional; set by each wait that commits
	volatile LONG raises;                            // raises since init; published after the gate reads up, under +152 when a generator exists
};

extern ZoneResetGate g_zoneResetGate;

// Creates `over`, signalled. Call before any NavMesh thread exists. False
// when the event could not be made: the gate still works and its waits poll.
bool ZoneResetGateInit(ZoneResetGate* g);
// Zeroes the counts, resets `over`, then raises. Main thread.
void ZoneResetGateRaise(ZoneResetGate* g);
// Lowers, then sets `over`. Main thread.
void ZoneResetGateLower(ZoneResetGate* g);
// One interlocked read; any thread.
bool ZoneResetGateUp(const ZoneResetGate* g);
// One interlocked read. A claim snapshots this under the generator queue lock.
LONG ZoneResetGateRaises(const ZoneResetGate* g);
// One refusal at `site`. A site outside the table counts in the total only.
void ZoneResetGateNoteDeferred(ZoneResetGate* g, ZoneResetSite site);
long ZoneResetGateDeferredTotal(const ZoneResetGate* g);
long ZoneResetGateDeferredAt(const ZoneResetGate* g, ZoneResetSite site);

enum ZoneResetWait
{
	ZONE_RESET_WAIT_NONE = 0,   // no reset: start the work now
	ZONE_RESET_WAIT_WAITED,     // waited a reset out: re-validate the job before the work
	ZONE_RESET_WAIT_STOPPED     // NavMesh::stop was seen: carry on into the stop's own handling
};

// Any NavMesh thread, holding no lock. With the gate down, one interlocked
// read. With it up, counts one refusal, sets `parked` when given, then waits
// on `over` in slices, asking stopSeen each slice, until the gate is down or
// the stop is seen.
ZoneResetWait ZoneResetGateWait(ZoneResetGate* g, ZoneResetSite site, bool (*stopSeen)());
// A stable down-gate count changed since claim, or a committed wait, requires re-validation.
// An observed stop takes precedence. Any NavMesh thread, holding no lock.
ZoneResetWait ZoneResetGateWaitSince(ZoneResetGate* g, ZoneResetSite site,
	bool (*stopSeen)(), LONG raisesAtClaim);

// Raises on construction and lowers on destruction (an unwind included), each
// through its callback when given (raiseUnder, lowerUnder: the plugin passes the
// queue-lock +152 pair), directly otherwise; then calls afterLower when given.
struct ZoneResetGateScope
{
	ZoneResetGate* gate;
	void (*afterLower)();
	void (*lowerUnder)(ZoneResetGate*);
	ZoneResetGateScope(ZoneResetGate* g, void (*raiseUnder)(ZoneResetGate*), void (*afterLowerFn)(),
		void (*lowerUnderFn)(ZoneResetGate*) = NULL);
	~ZoneResetGateScope();
private:
	ZoneResetGateScope(const ZoneResetGateScope&);
	ZoneResetGateScope& operator=(const ZoneResetGateScope&);
};

#endif
