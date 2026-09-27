#ifndef KENSHI_ZONE_OPT_NM_ADJACENCY_POLICY_H
#define KENSHI_ZONE_OPT_NM_ADJACENCY_POLICY_H

// Claim-time exclusion of navmesh jobs whose stitches touch each other's
// objects. The drain (NavMeshGenerator::update) releases a sector's or an
// interior's old mesh and graph without the build mutex, so no job may be in
// flight while another job that its stitches touch is in flight or published
// but not yet drained.
//
// Pure: no Windows header and no game pointer. The caller holds the
// generator's queue lock (+152) around every registry call; the runtime side
// (nm_adjacency.cpp) adds the lock, the event and the lock-free reader.

// A task's kind. An exterior stitches its four edge neighbours' sectors and
// walks the interior lists of its own and those sectors. An interior stitches
// every sector its building's box spans. UNKNOWN is a type-4 task whose output
// could not be read: treated as both.
enum NmAdjKind
{
	NMADJ_KIND_E = 0,
	NMADJ_KIND_I,
	NMADJ_KIND_UNKNOWN
};

struct NmJobDesc
{
	int x, y;         // task->zone->coordinates
	int type;         // flags & 7
	int kind;         // NmAdjKind
	// Cells an interior's stitch can reach, inclusive. The 8-neighbourhood of
	// its zone, widened to the box's cells when the box is known at claim.
	int rx0, ry0, rx1, ry1;
};

const float NMADJ_GRID_HALF_CELLS = 32.0f;    // UtilityT::getSubMapSector's origin shift
const unsigned int NMADJ_INTERIOR_UID_MIN = 0xFFFF;

// Cell of a world coordinate, as UtilityT::getSubMapSector (0x9B1EC0)
// computes it: floor((v + 32 * size) / size).
int NmAdjCellOf(float v, float cellSize);

// Builds the descriptor. uid: the type-4 output's uid (haveUid false when it
// could not be read). box: Ogre::Aabb {center xyz, halfSize xyz}, or NULL when
// the task's bounds are not set at claim (type 3: updateBT fills them later).
void NmAdjDescribe(int x, int y, int type, bool haveUid, unsigned int uid,
                   const float* box, float cellX, float cellZ, NmJobDesc* out);

// True when the two jobs may not be in flight (or undrained) together.
bool NmJobsConflict(const NmJobDesc& a, const NmJobDesc& b);

// True when an exterior cell lies inside an interior descriptor's reach.
bool NmAdjReaches(const NmJobDesc& d, int cx, int cy);

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

enum NmAdjState
{
	NMADJ_FREE = 0,
	NMADJ_CLAIMED,     // claimed, not yet published (or dropped)
	NMADJ_PUBLISHED,   // pushed to done (or dropped), not yet seen gone from it
	NMADJ_RESERVED     // a blocked job that later conflicting claims must wait for
};

// Owners. Workers are 0..NMADJ_BG-1. One reservation belongs to the bg thread's
// oldest blocked job, one to the oldest job the workers keep skipping. A
// reservation follows its task, not its queue position.
const int NMADJ_BG        = 6;
const int NMADJ_AGE       = 7;
const int NMADJ_NO_OWNER  = -1;
const int NMADJ_CAP       = 128;

struct NmAdjEntry
{
	NmJobDesc          d;
	unsigned __int64   task;     // compared, never dereferenced
	int                state;
	int                owner;
	long               pubSeq;
	long               resSeq;   // reservation order; 0 for CLAIMED/PUBLISHED
	__int64            since;    // caller's clock at claim / reservation
};

struct NmAdjRegistry
{
	NmAdjEntry e[NMADJ_CAP];
	int        high;       // entries at or above this index are FREE
	int        live;
	int        published;
	long       pubSeq;
	long       resSeq;
	unsigned __int64 pin;  // the node the original will pop; never moved or taken
	// The oldest candidate the workers skipped, and since when.
	unsigned __int64 ageTask;
	__int64          ageSince;
	// The oldest bg-only job the bg thread has seen runnable and not claimed,
	// and since when: its wait is what a stalled bg thread shows as.
	unsigned __int64 bgoTask;
	__int64          bgoSince;
	// When the bg thread last looked at the queue (a scan, an empty queue, or a
	// forwarded head). A bg-only job first seen now may have been queued any
	// time since, so its age starts there: an upper bound. Read and written only
	// on the bg thread, so the unlocked empty-queue return may write it.
	volatile __int64 bgLastLook;
	bool             bgLooked;
};

void NmAdjInit(NmAdjRegistry* r);

// Index of the first entry that blocks claiming `task`: a conflicting CLAIMED
// or PUBLISHED entry, or a conflicting reservation of another task that is
// older than ownResSeq (0 = the task holds no reservation, so every one
// counts). -1 when nothing blocks.
int NmAdjFindBlocker(const NmAdjRegistry* r, const NmJobDesc& d, unsigned __int64 task, long ownResSeq);

int  NmAdjAdd(NmAdjRegistry* r, const NmJobDesc& d, unsigned __int64 task, int state, int owner, __int64 now);
void NmAdjFree(NmAdjRegistry* r, int idx);
void NmAdjPublish(NmAdjRegistry* r, int idx);
int  NmAdjFindOwned(const NmAdjRegistry* r, int owner, int state);
int  NmAdjFindReservationOf(const NmAdjRegistry* r, unsigned __int64 task);

// Drain observer: frees every PUBLISHED entry with pubSeq <= seqAtRead whose
// task is not among the n tasks in done. Returns how many it freed.
int NmAdjReleaseDrained(NmAdjRegistry* r, long seqAtRead, const unsigned __int64* done, int n);

// ---------------------------------------------------------------------------
// Worker scan: one call per queue node, in queue order, under the lock.
// ---------------------------------------------------------------------------

enum NmAdjOffer
{
	NMADJ_OFFER_PASS = 0,   // not a candidate (ineligible)
	NMADJ_OFFER_SKIP,       // eligible, conflicts: leave it queued
	NMADJ_OFFER_TAKE        // claim this one
};

struct NmAdjScan
{
	bool             enforce;
	__int64          now;
	int              ageRes;          // index of the age reservation at scan start, -1 none
	unsigned __int64 ageResTask;
	bool             ageResSeen;
	bool             ageResEligible;
	unsigned __int64 firstSkipped;
	NmJobDesc        firstSkippedDesc;
	int              skips;
	bool             took;
	bool             tookConflicting;   // count mode: the taken job conflicts
	unsigned __int64 taken;
};

void       NmAdjScanBegin(NmAdjScan* s, const NmAdjRegistry* r, bool enforce, __int64 now);
NmAdjOffer NmAdjScanOffer(NmAdjScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                          const NmJobDesc* d, bool eligible);
// After a take: whether the walk must go on to find the age reservation's node.
bool       NmAdjScanWantsRest(const NmAdjScan* s);
void       NmAdjScanObserve(NmAdjScan* s, unsigned __int64 task, bool eligible);

struct NmAdjScanResult
{
	int      claimIdx;         // registered CLAIMED entry, -1 none
	bool     full;             // a take found no free entry
	bool     refused;          // enforce: the take was refused for lack of an entry
	bool     deferredTaken;    // the taken job was the tracked oldest skipped one
	__int64  deferredFor;      // its deferral, in the caller's clock
	bool     reservedAge;      // an age reservation was created
	bool     droppedAge;       // a stale age reservation was dropped
};

// Registers the take (if any) for `owner` and does the ageing bookkeeping.
// ageAfter: how long the oldest skipped job may wait before it is reserved.
// canReserve: false when no worker is alive to claim a reserved job.
void NmAdjScanEnd(NmAdjScan* s, NmAdjRegistry* r, int owner, const NmJobDesc* takenDesc,
                  __int64 ageAfter, bool canReserve, NmAdjScanResult* out);

// ---------------------------------------------------------------------------
// The bg thread's scan
//
// The bg thread never waits on a conflict while it has other work. It walks
// the queue in order and runs, first to last:
//   1. its reservation R, once nothing blocks R;
//   2. the first eligible job F, when there is no reservation and nothing
//      blocks F;
//   3. otherwise the first unblocked eligible job after that (a skip). When F
//      is blocked and there is no reservation, F is reserved first. A skip
//      never overtakes an eligible job it conflicts with, so the bg thread
//      never reorders conflicting jobs (workers and the prioritizer may).
// A reservation belongs to its task, wherever the task sits in the queue. Every
// claimer that does not hold an older reservation counts it, so nothing that
// conflicts with R starts after R is reserved.
//
// A skip is always a bgOnly job (types 2/3/4). Those never generate an
// exterior, so a reserved job waits at most one short bg job past its
// blockers; a type 0/1 skip could be a MISS of many seconds, and the workers
// take type 0/1 work anyway. A free worker is no substitute: it can take other
// work before the reserved job clears.
//
// The bg thread waits only when nothing in the queue can run.
// ---------------------------------------------------------------------------

enum NmAdjBgDecision
{
	NMADJ_BG_CLAIM = 0,   // registered CLAIMED (claimIdx); run `pick`
	NMADJ_BG_WAIT,        // something eligible is blocked, nothing can run: wait
	NMADJ_BG_FULL,        // enforce: no entry free; wait as for WAIT
	NMADJ_BG_NONE         // no eligible job at all
};

const int NMADJ_PASSED_CAP = 128;   // distinct descriptors; about 4.6 KB of stack
// Past its pick the walk looks this many nodes further for a runnable bg-only
// job to age. Every scan pays for them under +152.
const int NMADJ_TAIL_CAP = 32;

struct NmAdjBgScan
{
	bool             enforce;
	__int64          now;
	// The bg reservation at scan start.
	int              resIdx;
	unsigned __int64 resTask;
	bool             resSeen;
	bool             resEligible;
	bool             resRunnable;
	// F: the first eligible job that is not R.
	bool             haveFirst;
	unsigned __int64 first;
	NmJobDesc        firstDesc;
	bool             firstBlocked;
	// The first runnable job after F (or any runnable one while R exists).
	bool             haveAlt;
	unsigned __int64 alt;
	NmJobDesc        altDesc;
	// Eligible jobs the walk passed over, deduplicated; a skip may not
	// conflict with any of them. On overflow the walk stops looking for one.
	NmJobDesc        passed[NMADJ_PASSED_CAP];
	int              passedCount;
	bool             passedFull;
	// The first two bg-only jobs seen runnable (the walk goes on past the pick
	// to find them), and whether the tracked one (registry bgoTask) was seen
	// blocked.
	unsigned __int64 runnableBgOnly;
	unsigned __int64 runnableBgOnly2;
	int              tailSeen;        // nodes offered past the pick, capped at NMADJ_TAIL_CAP
	bool             bgoBlocked;
	bool             done;
};

struct NmAdjBgResult
{
	NmAdjBgDecision  decision;
	unsigned __int64 pick;
	bool             pickBgOnly;
	int              claimIdx;
	bool             conflicted;       // count mode: the pick conflicts
	bool             skipped;          // the pick is a later job; something earlier is blocked
	bool             newReservation;
	bool             droppedRes;       // R had left the queue or become ineligible
	__int64          deferredFor;      // the pick held a reservation: since when, in the caller's clock
	bool             deferred;
	bool             passedFull;       // the walk stopped looking for a skip at the cap
	bool             bgOnlyWaited;     // the pick is the tracked runnable bg-only job
	__int64          bgOnlyWait;       // how long it stayed runnable and unclaimed
};

void NmAdjBgScanBegin(NmAdjBgScan* s, const NmAdjRegistry* r, bool enforce, __int64 now);
// One call per queue node, in queue order, under the lock. eligible: the bg
// thread may run it now (zone and content set, not held for an unload).
// Returns true once the walk may stop. Past the pick the walk goes on only to
// find a runnable bg-only job (the bg-only age); NmAdjBgScanWants says whether
// a node still needs offering (and describing).
bool NmAdjBgScanOffer(NmAdjBgScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                      const NmJobDesc* d, bool eligible, bool bgOnly);
// Decides, registers the claim and keeps or moves the reservation. The pin is
// set to the pick when the pick is bgOnly (the original pops it), else cleared.
bool NmAdjBgScanWants(const NmAdjBgScan* s, bool bgOnly);
void NmAdjBgScanEnd(NmAdjBgScan* s, NmAdjRegistry* r, NmAdjBgResult* out);

// Drops the bg reservation and the pin (empty queue, stop).
void NmAdjBgRelease(NmAdjRegistry* r);
// The bg thread looked at the queue without a scan (empty, or a forwarded
// head): the bg-only age's clock.
void NmAdjBgLooked(NmAdjRegistry* r, __int64 now);
// The head is forwarded to the original with no stitch: pin it, and drop the
// reservation when it is the reserved task.
void NmAdjBgForward(NmAdjRegistry* r, unsigned __int64 head);

// The oldest reservation `task` holds (its resSeq), 0 when none.
long NmAdjOwnResSeq(const NmAdjRegistry* r, unsigned __int64 task);

#endif // KENSHI_ZONE_OPT_NM_ADJACENCY_POLICY_H
