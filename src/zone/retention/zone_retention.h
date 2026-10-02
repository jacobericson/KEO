#ifndef KEO_ZONE_RETENTION_H
#define KEO_ZONE_RETENTION_H

#include "base/config.h"

// Bounded retention for cells the game owns because the mod handed them
// over. The native camera/player/town countdowns keep their meaning; the
// policy only decides, at the moment all three are about to run out, whether
// this cell goes now or stays another frame. A stay is expressed by writing
// the town countdown, which no reader treats as a lease, so nothing else in
// the engine sees a hold.
//
// Main thread only. The decision runs in the ZoneMap::update prologue, once
// per Set B member per frame, so everything on the path a cell that is not
// expiring takes must be a handful of reads.

#if ZONEHAND_STEP >= 3

void ZoneRetentionInit();
void ZoneRetentionOnWorldReset();

// The frame boundary, published before any work the frame skips: clears the
// per-frame release slot and remembers the zone manager the prologue's
// decision needs. Without a frame of its own the pacing rules would have no
// meaning, so the prologue does nothing until this has run.
void ZoneRetentionBeginFrame(void* zoneMgr);

// The game has taken a cell over: it gets its residence window and its
// grace, with the revisit ladder applied.
void ZoneRetentionNoteAdopted(int gx, int gy, double now);
// The mod has stopped tracking a cell (the game finished with it, or it was
// never the game's in the first place).
void ZoneRetentionNoteRetired(int gx, int gy);

// Once per frame after adoption: refreshes the proximity map on its own
// cadence, renews prediction leases from the watched movers, recomputes
// pressure and prints the periodic line.
void ZoneRetentionTick(void* zoneMgr, double now);

// The prologue's decision for one cell: true when the caller must take the
// fences and let the original expire it. False covers both "not ours" and
// "held" -- the caller does the same thing for either, and when the answer is
// a hold the town countdown has already been written here.
bool ZoneRetentionWantsRelease(void* zoneEntry);

// The fences refused, so the release becomes a hold for this frame: writes
// the countdown and counts the reason.
enum ZoneRetentionDefer
{
	ZONE_RETENTION_DEFER_NAV = 0,   // a navmesh job or claim still names the cell
	ZONE_RETENTION_DEFER_PJ,        // processJobCS was held by a generation
	ZONE_RETENTION_DEFER_COUNT
};
void ZoneRetentionHoldInstead(void* zoneEntry, ZoneRetentionDefer reason);

// The original ran on the release path. `expired` is its answer inverted:
// true when it took its expiry branch and the cell left Set B.
void ZoneRetentionNoteReleased(void* zoneEntry, bool expired, bool fenced);

#else

inline void ZoneRetentionInit() {}
inline void ZoneRetentionOnWorldReset() {}
inline void ZoneRetentionBeginFrame(void*) {}
inline void ZoneRetentionNoteAdopted(int, int, double) {}
inline void ZoneRetentionNoteRetired(int, int) {}
inline void ZoneRetentionTick(void*, double) {}

#endif // ZONEHAND_STEP >= 3

#endif // KEO_ZONE_RETENTION_H
