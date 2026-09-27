#pragma once
// Pure rules over the watched-mover set: how many entries the registry keeps,
// which entry may be overwritten when it is full, whether a zone is on a
// mover's route, and when the navmesh queue is due for reprioritization.
// No game or KenshiLib headers — host-testable (tools/tests/mover_policy_units.cpp).

// One watched entry, reduced to what the eviction rule reads.
struct MoverSlot {
	bool   hasMoveOrder;
	double addedTime;
};

// Entries the registry keeps. The full player-faction registry needs room for
// a whole faction; without it only order-carrying characters are tracked.
int WatchedCapacity(bool registryEnabled);

// Slot to overwrite when the registry is full: the oldest entry with no move
// order. Returns -1 when every entry carries one — such an entry is never
// evicted, so the caller drops the add instead.
int ChooseWatchedEvictSlot(const MoverSlot* slots, int count);

// True when this mover carries a move order and stands in this zone, i.e. the
// zone is on a route the player asked for.
bool IsRouteTierMatch(bool hasMoveOrder, int moverX, int moverY,
                      int gridX, int gridY);

// Why a reprioritization pass runs. Order matters: a request beats the timer.
enum ReprioReason {
	REPRIO_NONE  = 0,
	REPRIO_FLAG  = 1,   // a pass was explicitly requested since the last one
	REPRIO_TIMER = 2    // the backstop interval elapsed
};

// `hasWork` is the legacy gate (watched movers or queued preloads); with the
// fast cadence the backstop runs regardless, because a pass over an empty
// queue returns before taking the queue lock.
int ReprioDue(bool flagRequested, double now, double lastReprio,
              double intervalSec, bool hasWork, bool fastCadence);
