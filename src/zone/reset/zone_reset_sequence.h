#ifndef KEO_ZONE_RESET_SEQUENCE_H
#define KEO_ZONE_RESET_SEQUENCE_H

#include "zone/reset/zone_reset_fence.h"

// The save-load reset's fence around the game's own unload, in one fixed
// order: the generation drain; processJobCS on the budget the drain left; the
// native unload, whatever the fence reached; the cells still holding content,
// each unloaded or kept; then the release, and the drain's end after it. The
// native unload is never refused or put off: the rest of the reset and the
// load run in the same call, so a fence that ran out changes only which
// survivors are kept. Main thread. Pure: the operations are its only effects.

enum ZoneResetLock { ZONE_RESET_LOCK_NONE = 0, ZONE_RESET_LOCK_HELD, ZONE_RESET_LOCK_TIMEOUT };

struct ZoneResetFenceOps
{
	void* ctx;
	// Stops new generations from releasing processJobCS and waits up to
	// timeoutMs for those already released. The block stays until drainEnd.
	bool          (*drainBegin)(void* ctx, unsigned timeoutMs, unsigned* waitedMs);
	void          (*drainEnd)(void* ctx);
	ZoneResetLock (*lock)(void* ctx, unsigned timeoutMs, unsigned* waitedMs);
	void          (*unlock)(void* ctx);
	void          (*nativeUnload)(void* ctx);
	int           (*collectSurvivors)(void* ctx);     // the count of cells still holding content
	bool          (*survivorClaimed)(void* ctx, int i);
	void          (*unloadSurvivor)(void* ctx, int i);
	void          (*keepSurvivor)(void* ctx, int i);  // left loaded under a claim
};

struct ZoneResetFenceOutcome
{
	bool          drained;
	unsigned      drainMs;
	ZoneResetLock lock;
	unsigned      lockWaitMs;
	bool          fenceComplete;
	int           survivors;
	int           kept;
};

// unloadSurvivors false (the key off, or the unload unbound): every survivor is
// counted and left, and none is asked about. The lock is released only when it
// was held, before the drain ends, on every exit including an unwind out of an
// operation.
void ZoneResetRunFence(const ZoneResetFenceOps* ops, unsigned totalMs, unsigned floorMs,
                       bool unloadSurvivors, ZoneResetFenceOutcome* out);

#endif
