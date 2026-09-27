#ifndef KENSHI_ZONE_OPT_ZONE_RESET_FENCE_H
#define KENSHI_ZONE_OPT_ZONE_RESET_FENCE_H

// The save-load reset's boundary: the one deadline it runs on, the decision it
// makes about a zone a generation may still be reading, whether navmesh work
// may start while it runs, and the retirements it could not carry out. The
// decisions are pure and callable from any thread; the NavMesh threads call
// them only once the reset gate (zone_reset_gate.h) reads up or has made them
// wait. The record table below is main-thread state (the reset hook and the
// zone-lifecycle tick). Host-linkable: no game or KenshiLib header, no
// allocation and no lock.

// One deadline covers the generation drain and the processJobCS acquisition
// that follows it. Whatever the drain spent comes off the lock's budget, so
// the pair never stalls the main thread for two full bounds. `floorMs` keeps
// a lock attempt worth making after a slow drain and is the only way the sum
// can exceed `totalMs`, by at most `floorMs`.
unsigned ZoneResetLockBudgetMs(unsigned totalMs, unsigned drainWaitedMs, unsigned floorMs);

// What to do with one zone that still holds content once the native reset has
// unloaded its own.
enum ZoneResetSurvivorAction
{
	ZONE_RESET_UNLOAD = 0,     // safe to unload: the fence is complete, or no claim names the zone
	ZONE_RESET_SKIP_CLAIMED    // a generation may be reading the zone; leave it loaded
};

// `fenceComplete` is the drain and the lock together: only both of them keep
// every generation off the zone. With either missing, a zone a worker or the
// background thread has claimed is left loaded rather than freed under the
// generation reading it.
ZoneResetSurvivorAction ZoneResetDecideSurvivor(bool fenceComplete, bool claimed);

// Where navmesh work would start: a claim (under the generator's queue lock),
// a claimed job's cache lookup and HIT rebuild, or its collision build.
enum ZoneResetSite
{
	ZONE_RESET_SITE_CLAIM = 0,
	ZONE_RESET_SITE_HIT,
	ZONE_RESET_SITE_BUILD,
	ZONE_RESET_SITE_COUNT
};

enum ZoneResetAdmission
{
	ZONE_RESET_ADMIT = 0,       // start the work now
	ZONE_RESET_DEFER_RESET,     // a reset is running: a claim leaves the job queued, a
	                            // claimed job waits for the reset's end
	ZONE_RESET_DEFER_STOP       // NavMesh::stop was seen: this rule steps aside and the
	                            // stop's own handling at or after the site applies
};

// Whether work may start at `site`. The stop is checked first, so no thread
// waits out a reset once the stop is seen. Every site answers alike: none is
// exempt, and a site this build does not know is covered.
ZoneResetAdmission ZoneResetAdmit(bool resetInProgress, bool stopSeen, ZoneResetSite site);

// Whether a job that waited a reset out may go on: its zone still holds the
// content it held before the wait. A zone that held none before never passes.
bool ZoneResetContentKept(const void* before, const void* now);


// ---------------------------------------------------------------------------
// Retirements the fence began and could not verify
// ---------------------------------------------------------------------------
//
// A skipped zone is left loaded with its private flag still set, and the reset
// then drops every other trace of it: the preload table, the lifecycle records
// and the preparation ledger all go with the old world. The cell survives into
// the next world holding content nothing names, where it blocks both the mod's
// own preload and the game's activation lease.
//
// A record here is that cell. Membership is the whole state -- a record means
// the cell entered retirement and nothing has confirmed it left -- so there is
// no state or verified field to read: the record is removed when the cell is
// seen with no content, or when a later reset unloads it.
//
// The generation counter below advances once per reset, so a record made by
// the reset that is running now is not yet "held over": it becomes one when
// the next generation starts.

const int ZONE_RESET_FENCE_MAX = 16;

// The generation the reset now running belongs to. It counts resets this
// module has seen and is not a mirror of any other counter.
unsigned ZoneResetFenceGeneration();

// End of a reset: records made during it are held over from here on.
void ZoneResetFenceAdvanceGeneration();

// Records `gx,gy` as retiring-unverified in `generation`, or refreshes the
// generation of a record already there. False when the table is full (counted
// by the overflow below) or the coordinates are outside the grid.
bool ZoneResetFenceNoteUnverified(int gx, int gy, unsigned generation);

// True when a record for `gx,gy` was made in a generation strictly older than
// `currentGeneration`: the cell is held over from a previous world.
bool ZoneResetFenceHolds(int gx, int gy, unsigned currentGeneration);

// Removes the cell's record. True when one was there.
bool ZoneResetFenceClear(int gx, int gy);

int  ZoneResetFenceCount();
long ZoneResetFenceOverflow();

// The host tests. Nothing in the plugin empties this table wholesale: a
// world reset is exactly the event these records have to survive.
void ZoneResetFenceReset();

#endif // KENSHI_ZONE_OPT_ZONE_RESET_FENCE_H
